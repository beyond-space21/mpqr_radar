const $ = (id) => document.getElementById(id);

const els = {
  dot: $("dot"),
  conn: $("conn"),
  status: $("status"),
  statusMeta: $("status-meta"),
  lvl: $("lvl"),
  vel: $("vel"),
  flow: $("flow"),
  vol: $("vol"),
  empty: $("empty"),
  dir: $("dir"),
  msgCount: $("msg-count"),
  broker: $("broker"),
  chart: $("chart"),
  conns: $("conns"),
  connCount: $("conn-count"),
};

function fmt(v, digits = 2) {
  if (v === null || v === undefined || Number.isNaN(Number(v))) return "—";
  return Number(v).toLocaleString(undefined, {
    maximumFractionDigits: digits,
    minimumFractionDigits: Number.isInteger(Number(v)) ? 0 : Math.min(digits, 2),
  });
}

function dirLabel(d) {
  if (d === 0 || d === "0") return "with stream";
  if (d === 1 || d === "1") return "against";
  return "—";
}

function applySnapshot(s) {
  if (!s) return;
  els.status.textContent = s.status || "—";
  els.statusMeta.textContent = s.status_at
    ? `updated ${s.status_at}`
    : s.latest?.received_at
      ? `last telemetry ${s.latest.received_at}`
      : "waiting for MQTT…";

  const r = s.latest || {};
  els.lvl.textContent = fmt(r.lvl, 0);
  els.vel.textContent = fmt(r.vel, 2);
  els.flow.textContent = fmt(r.flow, 3);
  els.vol.textContent = fmt(r.vol, 0);
  els.empty.textContent = fmt(r.empty, 0);
  els.dir.textContent = dirLabel(r.dir);
  els.msgCount.textContent = `${s.msg_count || 0} messages`;
  if (s.broker) {
    els.broker.textContent = `${s.broker.host}:${s.broker.port}`;
  }
  renderConnections(s.connections || []);
  drawChart(s.history || []);
}

function renderConnections(conns) {
  els.connCount.textContent = `${conns.length} client${conns.length === 1 ? "" : "s"}`;
  els.conns.innerHTML = "";
  if (conns.length === 0) {
    const li = document.createElement("li");
    li.className = "conn empty";
    li.textContent = "No clients connected";
    els.conns.appendChild(li);
    return;
  }
  for (const c of conns) {
    const li = document.createElement("li");
    li.className = "conn";

    const id = document.createElement("span");
    id.className = "conn-id";
    id.textContent = c.client_id;
    li.appendChild(id);

    if (c.is_dashboard) {
      const tag = document.createElement("span");
      tag.className = "conn-tag";
      tag.textContent = "dashboard";
      li.appendChild(tag);
    }

    const addr = document.createElement("span");
    addr.className = "conn-addr";
    addr.textContent = c.address || "";
    li.appendChild(addr);

    const st = document.createElement("span");
    const stateOk = (c.state || "").includes("connected") && !(c.state || "").includes("dis");
    st.className = `conn-state ${stateOk ? "ok" : "off"}`;
    st.textContent = c.state || "unknown";
    li.appendChild(st);

    els.conns.appendChild(li);
  }
}

function drawChart(history) {
  const canvas = els.chart;
  const ctx = canvas.getContext("2d");
  const dpr = window.devicePixelRatio || 1;
  const cssW = canvas.clientWidth || 960;
  const cssH = 220;
  canvas.width = Math.floor(cssW * dpr);
  canvas.height = Math.floor(cssH * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

  ctx.clearRect(0, 0, cssW, cssH);
  ctx.strokeStyle = "rgba(231,242,238,0.1)";
  ctx.lineWidth = 1;
  for (let i = 0; i < 4; i++) {
    const y = (cssH / 4) * i + 0.5;
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(cssW, y);
    ctx.stroke();
  }

  const pts = history.filter((h) => h.lvl != null);
  if (pts.length < 2) {
    ctx.fillStyle = "rgba(142,174,164,0.8)";
    ctx.font = "13px DM Sans, sans-serif";
    ctx.fillText("Level trend appears after two samples", 8, cssH / 2);
    return;
  }

  const vals = pts.map((p) => Number(p.lvl));
  let min = Math.min(...vals);
  let max = Math.max(...vals);
  if (min === max) {
    min -= 1;
    max += 1;
  }
  const pad = (max - min) * 0.08;
  min -= pad;
  max += pad;

  ctx.beginPath();
  pts.forEach((p, i) => {
    const x = (i / (pts.length - 1)) * (cssW - 8) + 4;
    const y = cssH - ((Number(p.lvl) - min) / (max - min)) * (cssH - 16) - 8;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.strokeStyle = "#3dba8c";
  ctx.lineWidth = 2;
  ctx.stroke();
}

function setConn(ok, label) {
  els.conn.textContent = label;
  els.dot.classList.toggle("ok", ok);
  els.dot.classList.toggle("bad", !ok && label !== "connecting");
}

function connectWs() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws`);

  ws.onopen = () => setConn(true, "live");
  ws.onclose = () => {
    setConn(false, "reconnecting");
    setTimeout(connectWs, 1500);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    try {
      const msg = JSON.parse(ev.data);
      if (msg.snapshot) applySnapshot(msg.snapshot);
      else if (msg.type === "hello") applySnapshot(msg.snapshot);
    } catch (_) {
      /* ignore */
    }
  };
}

window.addEventListener("resize", () => {
  fetch("/api/state")
    .then((r) => r.json())
    .then(applySnapshot)
    .catch(() => {});
});

fetch("/api/state")
  .then((r) => r.json())
  .then(applySnapshot)
  .catch(() => {})
  .finally(connectWs);
