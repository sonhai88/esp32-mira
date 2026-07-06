"""
Mira Monitor — cửa sổ quản lý ESP32 × Mira
Chạy: python tools/monitor.py
Mở browser: http://localhost:5555
"""
import serial
import serial.tools.list_ports
import threading
import queue
import time
import webbrowser
import re
import urllib.request
from flask import Flask, Response, jsonify, render_template_string

app = Flask(__name__)

# ── Shared state ──
log_queue: queue.Queue = queue.Queue(maxsize=500)
state = {
    "esp32": False,       # ESP32 cắm vào chưa
    "wifi": False,        # WiFi kết nối chưa (parse từ log)
    "mira": False,        # Mira server reachable
    "port": None,         # COM port đang dùng
    "last_user": "",      # câu anh nói
    "last_mira": "",      # Mira trả lời
}

MIRA_URL = "https://deli2222-mira-ai.hf.space/healthz"


# ────────────────────────────────────────────
#  AUTO-DETECT + READ SERIAL
# ────────────────────────────────────────────
def find_esp32_port():
    """Tìm COM port của ESP32."""
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        if any(k in desc for k in ["cp210", "ch340", "ch341", "usb serial", "uart", "esp32"]):
            return p.device
    return None


def parse_log(line: str):
    """Cập nhật state từ log line."""
    if "✓ IP:" in line or "OK! IP:" in line:
        state["wifi"] = True
    if "WiFi" in line and ("FAIL" in line or "không kết nối" in line.lower()):
        state["wifi"] = False
    if "[STT] Anh nói:" in line:
        state["last_user"] = line.split("[STT] Anh nói:")[-1].strip()
    if "[Mira]" in line and "Sẵn sàng" not in line and "Khởi động" not in line:
        state["last_mira"] = line.split("[Mira]")[-1].strip()


def serial_reader():
    """Thread đọc serial từ ESP32, đẩy vào log_queue."""
    ser = None
    while True:
        # Tìm port nếu chưa có
        if ser is None or not ser.is_open:
            port = find_esp32_port()
            if port:
                try:
                    ser = serial.Serial(port, 115200, timeout=1)
                    state["esp32"] = True
                    state["port"] = port
                    push_log("system", f"ESP32 kết nối tại {port}")
                except Exception as e:
                    push_log("error", f"Không mở được {port}: {e}")
                    time.sleep(2)
                    continue
            else:
                state["esp32"] = False
                state["wifi"] = False
                state["port"] = None
                time.sleep(2)
                continue

        try:
            line = ser.readline().decode("utf-8", errors="replace").strip()
            if line:
                parse_log(line)
                # Phân loại log
                if "[ERROR]" in line or "FAIL" in line or "fail" in line.lower():
                    level = "error"
                elif "[Mira]" in line and "Sẵn sàng" not in line:
                    level = "mira"
                elif "[STT]" in line:
                    level = "user"
                elif "[WiFi]" in line or "[Mic]" in line or "[Speaker]" in line:
                    level = "system"
                else:
                    level = "info"
                push_log(level, line)
        except serial.SerialException:
            state["esp32"] = False
            state["wifi"] = False
            push_log("error", "ESP32 bị ngắt kết nối")
            try:
                ser.close()
            except:
                pass
            ser = None
            time.sleep(1)


def mira_checker():
    """Thread check Mira server mỗi 30 giây."""
    while True:
        try:
            urllib.request.urlopen(MIRA_URL, timeout=5)
            state["mira"] = True
        except:
            state["mira"] = False
        time.sleep(30)


def push_log(level: str, text: str):
    ts = time.strftime("%H:%M:%S")
    entry = {"ts": ts, "level": level, "text": text}
    try:
        log_queue.put_nowait(entry)
    except queue.Full:
        log_queue.get_nowait()
        log_queue.put_nowait(entry)


# ────────────────────────────────────────────
#  FLASK ROUTES
# ────────────────────────────────────────────
@app.route("/")
def index():
    return render_template_string(HTML)


@app.route("/api/state")
def api_state():
    return jsonify(state)


@app.route("/api/logs")
def api_logs():
    """Server-Sent Events — stream log realtime."""
    def generate():
        # Gửi log cũ trước
        items = list(log_queue.queue)
        for item in items[-100:]:
            yield f"data: {_json(item)}\n\n"

        # Stream log mới
        last_size = log_queue.qsize()
        while True:
            current = list(log_queue.queue)
            if len(current) > last_size:
                for item in current[last_size:]:
                    yield f"data: {_json(item)}\n\n"
                last_size = len(current)
            time.sleep(0.1)

    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


def _json(d):
    import json
    return json.dumps(d, ensure_ascii=False)


# ────────────────────────────────────────────
#  HTML UI
# ────────────────────────────────────────────
HTML = """<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<title>Mira Monitor</title>
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
:root {
  --bg: #0d0d0d; --surface: #161616; --border: #2a2520;
  --accent: #b04832; --green: #22c55e; --yellow: #eab308; --red: #ef4444;
  --text: #f0ece8; --muted: #6b6460;
}
body { background: var(--bg); color: var(--text); font-family: -apple-system, 'Segoe UI', sans-serif; height: 100vh; display: flex; flex-direction: column; }

/* HEADER */
header { padding: 16px 24px; border-bottom: 1px solid var(--border); display: flex; align-items: center; justify-content: space-between; }
.logo { font-size: 15px; font-weight: 700; letter-spacing: .08em; color: var(--accent); }
.badges { display: flex; gap: 12px; }
.badge { display: flex; align-items: center; gap: 6px; font-size: 12px; color: var(--muted); }
.dot { width: 8px; height: 8px; border-radius: 50%; background: var(--red); transition: background .3s; }
.dot.on { background: var(--green); }
.dot.warn { background: var(--yellow); }

/* MAIN */
main { flex: 1; display: grid; grid-template-columns: 1fr 280px; overflow: hidden; }

/* LOG */
.log-panel { display: flex; flex-direction: column; border-right: 1px solid var(--border); }
.log-header { padding: 12px 16px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: .06em; }
.log-header button { background: none; border: 1px solid var(--border); color: var(--muted); border-radius: 6px; padding: 4px 10px; cursor: pointer; font-size: 11px; }
.log-header button:hover { border-color: var(--accent); color: var(--accent); }
#logEl { flex: 1; overflow-y: auto; padding: 8px 0; font-family: 'Fira Code', 'Cascadia Code', monospace; font-size: 12px; }
.log-line { display: flex; gap: 10px; padding: 3px 16px; line-height: 1.5; }
.log-line:hover { background: rgba(255,255,255,.03); }
.log-ts { color: var(--muted); min-width: 60px; user-select: none; }
.log-text { flex: 1; white-space: pre-wrap; word-break: break-all; }
.log-line.error .log-text { color: var(--red); }
.log-line.mira .log-text { color: #93c5fd; }
.log-line.user .log-text { color: #86efac; }
.log-line.system .log-text { color: var(--muted); }
.log-line.info .log-text { color: var(--text); }

/* SIDEBAR */
.sidebar { padding: 16px; display: flex; flex-direction: column; gap: 16px; overflow-y: auto; }
.card { background: var(--surface); border: 1px solid var(--border); border-radius: 12px; padding: 14px; }
.card-title { font-size: 11px; text-transform: uppercase; letter-spacing: .06em; color: var(--muted); margin-bottom: 10px; }
.conv-item { margin-bottom: 10px; }
.conv-label { font-size: 10px; color: var(--muted); margin-bottom: 4px; }
.conv-text { font-size: 13px; line-height: 1.5; color: var(--text); background: var(--bg); border-radius: 8px; padding: 8px 10px; min-height: 36px; border: 1px solid var(--border); }
.conv-text.mira-reply { border-color: rgba(176,72,50,.3); }

.port-info { font-size: 12px; color: var(--muted); }
.port-val { font-family: monospace; color: var(--text); }
</style>
</head>
<body>
<header>
  <span class="logo">MIRA MONITOR</span>
  <div class="badges">
    <div class="badge"><div class="dot" id="dotEsp32"></div><span>ESP32</span></div>
    <div class="badge"><div class="dot" id="dotWifi"></div><span>WiFi</span></div>
    <div class="badge"><div class="dot" id="dotMira"></div><span>Mira Server</span></div>
  </div>
</header>

<main>
  <div class="log-panel">
    <div class="log-header">
      <span>Serial Log</span>
      <button onclick="clearLog()">Xóa log</button>
    </div>
    <div id="logEl"></div>
  </div>

  <div class="sidebar">
    <div class="card">
      <div class="card-title">Hội thoại</div>
      <div class="conv-item">
        <div class="conv-label">Anh nói</div>
        <div class="conv-text" id="lastUser">—</div>
      </div>
      <div class="conv-item">
        <div class="conv-label">Mira trả lời</div>
        <div class="conv-text mira-reply" id="lastMira">—</div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Kết nối</div>
      <div class="port-info">Port: <span class="port-val" id="portVal">—</span></div>
    </div>
  </div>
</main>

<script>
// Cập nhật state mỗi 2 giây
async function updateState() {
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    setDot('dotEsp32', s.esp32);
    setDot('dotWifi', s.wifi);
    setDot('dotMira', s.mira);
    document.getElementById('portVal').textContent = s.port || '—';
    if (s.last_user) document.getElementById('lastUser').textContent = s.last_user;
    if (s.last_mira) document.getElementById('lastMira').textContent = s.last_mira;
  } catch {}
}

function setDot(id, on) {
  document.getElementById(id).className = 'dot' + (on ? ' on' : '');
}

// Stream log qua SSE
const logEl = document.getElementById('logEl');
let autoScroll = true;
logEl.addEventListener('scroll', () => {
  autoScroll = logEl.scrollTop + logEl.clientHeight >= logEl.scrollHeight - 20;
});

const es = new EventSource('/api/logs');
es.onmessage = e => {
  const d = JSON.parse(e.data);
  const line = document.createElement('div');
  line.className = 'log-line ' + d.level;
  line.innerHTML = `<span class="log-ts">${d.ts}</span><span class="log-text">${escHtml(d.text)}</span>`;
  logEl.appendChild(line);
  if (autoScroll) logEl.scrollTop = logEl.scrollHeight;
};

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

function clearLog() {
  logEl.innerHTML = '';
}

updateState();
setInterval(updateState, 2000);
</script>
</body>
</html>"""


# ────────────────────────────────────────────
#  MAIN
# ────────────────────────────────────────────
if __name__ == "__main__":
    push_log("system", "Mira Monitor khởi động...")

    threading.Thread(target=serial_reader, daemon=True).start()
    threading.Thread(target=mira_checker, daemon=True).start()

    # Mở browser sau 1 giây
    threading.Timer(1.0, lambda: webbrowser.open("http://localhost:5555")).start()

    print("=" * 50)
    print("  Mira Monitor đang chạy")
    print("  Mở browser: http://localhost:5555")
    print("  Ctrl+C để thoát")
    print("=" * 50)

    app.run(host="0.0.0.0", port=5555, debug=False, threaded=True)
