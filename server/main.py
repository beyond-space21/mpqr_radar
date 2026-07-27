"""
mpqr radar — MQTT broker + live HTML dashboard.

  MQTT : 0.0.0.0:1883
  Web  : http://0.0.0.0:8080

Device topics (from firmware config.h):
  mpqr/radar/01/telemetry
  mpqr/radar/01/status

Run:
  pip install -r requirements.txt
  python main.py
"""

from __future__ import annotations

import asyncio
import json
import logging
import threading
import time
from collections import deque
from contextlib import asynccontextmanager
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt
from amqtt.broker import Broker
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(message)s",
)
log = logging.getLogger("mpqr")

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"

MQTT_HOST = "0.0.0.0"
MQTT_PORT = 1883
WEB_HOST = "0.0.0.0"
WEB_PORT = 8090  # 8080/8081 are occupied by the IDE on the dev machine

TOPIC_TELEMETRY = "mpqr/radar/01/telemetry"
TOPIC_STATUS = "mpqr/radar/01/status"
TOPIC_WILDCARD = "mpqr/radar/#"

HISTORY_LEN = 120


@dataclass
class Telemetry:
    vol: float | None = None
    flow: float | None = None
    lvl: int | None = None
    vel: float | None = None
    empty: int | None = None
    dir: int | None = None
    received_at: str | None = None
    raw: str | None = None


@dataclass
class AppState:
    status: str = "unknown"
    status_at: str | None = None
    latest: Telemetry = field(default_factory=Telemetry)
    history: deque = field(default_factory=lambda: deque(maxlen=HISTORY_LEN))
    msg_count: int = 0
    last_topic: str | None = None


state = AppState()
ws_clients: set[WebSocket] = set()
ws_lock = asyncio.Lock()
main_loop: asyncio.AbstractEventLoop | None = None
broker_ref: Broker | None = None

DASHBOARD_CLIENT_ID = "mpqr-dashboard"


def connections_snapshot() -> list[dict[str, Any]]:
    """List sessions currently known to the embedded broker."""
    conns: list[dict[str, Any]] = []
    if broker_ref is None:
        return conns
    sessions = getattr(broker_ref, "_sessions", {})
    for client_id, entry in list(sessions.items()):
        session = entry[0] if isinstance(entry, tuple) else entry
        try:
            conn_state = str(session.transitions.state)
        except Exception:
            conn_state = "unknown"
        addr = getattr(session, "remote_address", None)
        port = getattr(session, "remote_port", None)
        conns.append(
            {
                "client_id": client_id,
                "address": f"{addr}:{port}" if addr else None,
                "state": conn_state,
                "is_dashboard": client_id == DASHBOARD_CLIENT_ID,
            }
        )
    conns.sort(key=lambda c: (c["is_dashboard"], c["client_id"]))
    return conns


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def snapshot() -> dict[str, Any]:
    return {
        "status": state.status,
        "status_at": state.status_at,
        "latest": asdict(state.latest),
        "history": list(state.history),
        "msg_count": state.msg_count,
        "last_topic": state.last_topic,
        "connections": connections_snapshot(),
        "topics": {
            "telemetry": TOPIC_TELEMETRY,
            "status": TOPIC_STATUS,
        },
        "broker": {"host": MQTT_HOST, "port": MQTT_PORT},
        "server_time": utc_now(),
    }


async def broadcast(event: dict[str, Any]) -> None:
    dead: list[WebSocket] = []
    async with ws_lock:
        clients = list(ws_clients)
    payload = json.dumps(event)
    for ws in clients:
        try:
            await ws.send_text(payload)
        except Exception:
            dead.append(ws)
    if dead:
        async with ws_lock:
            for ws in dead:
                ws_clients.discard(ws)


def schedule_broadcast(event: dict[str, Any]) -> None:
    if main_loop is None or main_loop.is_closed():
        return
    asyncio.run_coroutine_threadsafe(broadcast(event), main_loop)


# --- Embedded MQTT broker (amqtt) -------------------------------------------------

broker_config = {
    "listeners": {
        "default": {
            "type": "tcp",
            "bind": f"{MQTT_HOST}:{MQTT_PORT}",
        }
    },
    "sys_interval": 0,
    "topic-check": {"enabled": False},
}


async def run_broker() -> Broker:
    broker = Broker(broker_config)
    await broker.start()
    log.info("MQTT broker listening on %s:%s", MQTT_HOST, MQTT_PORT)
    return broker


# --- Local MQTT subscriber (feeds dashboard) --------------------------------------

def on_connect(client: mqtt.Client, _userdata: Any, _flags: Any, reason_code: Any, _props: Any = None) -> None:
    rc = getattr(reason_code, "value", reason_code)
    if rc == 0:
        log.info("Dashboard subscriber connected")
        client.subscribe(TOPIC_WILDCARD)
    else:
        log.error("MQTT subscribe connect failed rc=%s", rc)


def on_message(_client: mqtt.Client, _userdata: Any, msg: mqtt.MQTTMessage) -> None:
    text = msg.payload.decode("utf-8", errors="replace").strip()
    state.msg_count += 1
    state.last_topic = msg.topic
    now = utc_now()

    if msg.topic.endswith("/status") or msg.topic == TOPIC_STATUS:
        state.status = text
        state.status_at = now
        schedule_broadcast({"type": "status", "status": text, "status_at": now, "snapshot": snapshot()})
        log.info("status: %s", text)
        return

    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        log.warning("non-JSON on %s: %s", msg.topic, text[:120])
        return

    reading = Telemetry(
        vol=data.get("vol"),
        flow=data.get("flow"),
        lvl=data.get("lvl"),
        vel=data.get("vel"),
        empty=data.get("empty"),
        dir=data.get("dir"),
        received_at=now,
        raw=text,
    )
    state.latest = reading
    state.history.append(
        {
            "t": now,
            "vol": reading.vol,
            "flow": reading.flow,
            "lvl": reading.lvl,
            "vel": reading.vel,
        }
    )
    schedule_broadcast({"type": "telemetry", "reading": asdict(reading), "snapshot": snapshot()})
    log.info("telemetry: %s", text)


def start_subscriber() -> mqtt.Client:
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="mpqr-dashboard",
        protocol=mqtt.MQTTv311,
    )
    client.on_connect = on_connect
    client.on_message = on_message

    def _run() -> None:
        backoff = 1.0
        while True:
            try:
                client.connect("127.0.0.1", MQTT_PORT, keepalive=60)
                backoff = 1.0
                client.loop_forever(retry_first_connection=True)
            except Exception as exc:
                log.warning("subscriber reconnect in %.0fs (%s)", backoff, exc)
                time.sleep(backoff)
                backoff = min(backoff * 2, 15.0)

    threading.Thread(target=_run, name="mqtt-sub", daemon=True).start()
    return client


# --- FastAPI ----------------------------------------------------------------------

async def watch_connections() -> None:
    """Push a fresh snapshot to the dashboard whenever broker sessions change."""
    prev: list[dict[str, Any]] | None = None
    while True:
        cur = connections_snapshot()
        if cur != prev:
            prev = cur
            await broadcast({"type": "connections", "snapshot": snapshot()})
        await asyncio.sleep(2)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global main_loop, broker_ref
    main_loop = asyncio.get_running_loop()
    broker_ref = await run_broker()
    await asyncio.sleep(0.3)
    start_subscriber()
    watcher = asyncio.create_task(watch_connections())
    log.info("Dashboard http://%s:%s", WEB_HOST, WEB_PORT)
    try:
        yield
    finally:
        watcher.cancel()
        await broker_ref.shutdown()


app = FastAPI(title="mpqr radar", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=STATIC), name="static")


@app.get("/")
async def index() -> FileResponse:
    return FileResponse(STATIC / "index.html")


@app.get("/api/state")
async def api_state() -> dict[str, Any]:
    return snapshot()


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket) -> None:
    await ws.accept()
    async with ws_lock:
        ws_clients.add(ws)
    try:
        await ws.send_text(json.dumps({"type": "hello", "snapshot": snapshot()}))
        while True:
            # Keep alive; ignore client messages
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        async with ws_lock:
            ws_clients.discard(ws)


def main() -> None:
    import uvicorn

    uvicorn.run(
        "main:app",
        host=WEB_HOST,
        port=WEB_PORT,
        reload=False,
        log_level="info",
    )


if __name__ == "__main__":
    main()
