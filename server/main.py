"""
mpqr radar — live HTML dashboard (MQTT subscriber).

Connects to the same broker the Nano uses (config.h):
  217.217.249.208:1883

  Web  : http://0.0.0.0:8081

Device topics:
  mpqr/radar/01/telemetry
  mpqr/radar/01/status

Run:
  pip install -r requirements.txt
  python main.py

Override broker with env if needed:
  MQTT_BROKER=217.217.249.208 MQTT_PORT=1883 python main.py
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import threading
import time
from collections import deque
from contextlib import asynccontextmanager
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import paho.mqtt.client as mqtt
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

# Same broker as firmware include/config.h
MQTT_BROKER = os.getenv("MQTT_BROKER", "217.217.249.208")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "")
MQTT_PASS = os.getenv("MQTT_PASS", "")

WEB_HOST = os.getenv("WEB_HOST", "0.0.0.0")
WEB_PORT = int(os.getenv("WEB_PORT", "8081"))

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
    broker_ok: bool = False


state = AppState()
ws_clients: set[WebSocket] = set()
ws_lock = asyncio.Lock()
main_loop: asyncio.AbstractEventLoop | None = None


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
        "broker_ok": state.broker_ok,
        "topics": {
            "telemetry": TOPIC_TELEMETRY,
            "status": TOPIC_STATUS,
        },
        "broker": {"host": MQTT_BROKER, "port": MQTT_PORT},
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


def on_connect(client: mqtt.Client, _userdata: Any, _flags: Any, reason_code: Any, _props: Any = None) -> None:
    rc = getattr(reason_code, "value", reason_code)
    if rc == 0:
        state.broker_ok = True
        log.info("Connected to broker %s:%s", MQTT_BROKER, MQTT_PORT)
        client.subscribe(TOPIC_WILDCARD)
        schedule_broadcast({"type": "broker", "snapshot": snapshot()})
    else:
        state.broker_ok = False
        log.error("MQTT connect failed rc=%s", rc)


def on_disconnect(_client: mqtt.Client, _userdata: Any, _flags: Any, reason_code: Any, _props: Any = None) -> None:
    state.broker_ok = False
    rc = getattr(reason_code, "value", reason_code)
    log.warning("MQTT disconnected rc=%s", rc)
    schedule_broadcast({"type": "broker", "snapshot": snapshot()})


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


def start_subscriber() -> None:
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="mpqr-dashboard",
        protocol=mqtt.MQTTv311,
    )
    if MQTT_USER:
        client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    def _run() -> None:
        backoff = 1.0
        while True:
            try:
                log.info("Connecting to %s:%s …", MQTT_BROKER, MQTT_PORT)
                client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
                backoff = 1.0
                client.loop_forever(retry_first_connection=True)
            except Exception as exc:
                state.broker_ok = False
                log.warning("subscriber reconnect in %.0fs (%s)", backoff, exc)
                time.sleep(backoff)
                backoff = min(backoff * 2, 15.0)

    threading.Thread(target=_run, name="mqtt-sub", daemon=True).start()


@asynccontextmanager
async def lifespan(_app: FastAPI):
    global main_loop
    main_loop = asyncio.get_running_loop()
    start_subscriber()
    log.info("Dashboard http://%s:%s  (broker %s:%s)", WEB_HOST, WEB_PORT, MQTT_BROKER, MQTT_PORT)
    yield


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
