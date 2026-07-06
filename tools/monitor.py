"""
Mira Dev Tool — quản lý ESP32 × Mira
Chạy: python tools/monitor.py
UI:   http://localhost:5555
API:  http://localhost:5555/api/log/latest   ← Claude đọc trực tiếp
"""
import os, sys, re, json, time, queue, shutil, threading, subprocess
import webbrowser, urllib.request
from pathlib import Path
from datetime import datetime
import serial
import serial.tools.list_ports
from flask import Flask, Response, jsonify, request, send_file
import io

app = Flask(__name__)

# ── Paths ──
TOOLS_DIR   = Path(__file__).parent
PROJECT_DIR = TOOLS_DIR.parent
LOG_FILE    = TOOLS_DIR / "mira_session.log"

MIRA_URL    = "https://deli2222-mira-ai.hf.space"

# ── Shared state ──
log_entries: list[dict] = []   # {seq, ts, level, text, fix}
log_lock = threading.Lock()
_log_seq  = 0                  # monotonic counter — không bao giờ giảm kể cả khi clear

state = {
    "esp32": False,
    "wifi":  False,
    "mira":  False,
    "port":  None,
    "last_user":  "",
    "last_mira":  "",
    "upload_running": False,
    "issues": [],       # list of {msg, fix} hiện tại
}

# ── Đối tượng serial (để reset) ──
_ser: serial.Serial | None = None
_ser_lock = threading.Lock()

# ────────────────────────────────────────────
#  ERROR PATTERN → AUTO FIX HINTS
# ────────────────────────────────────────────
PATTERNS = [
    (r"HTTP 401",
     "DEVICE_API_KEY sai → mở include/config.h sửa dòng DEVICE_API_KEY"),
    (r"HTTP 503|HTTP 502",
     "Mira server đang khởi động (cold start) → đợi 60s rồi thử lại"),
    (r"HTTP 404",
     "URL sai → kiểm tra MIRA_BASE_URL trong config.h"),
    (r"PSRAM not found|psramFound.*false",
     "PSRAM không tìm thấy → platformio.ini: board_build.arduino.memory_type = qio_opi"),
    (r"ps_malloc.*thất bại|ps_malloc.*fail",
     "Hết PSRAM → giảm RECORD_SECONDS trong config.h (thử 3)"),
    (r"WiFi.*FAIL|WiFi.*✗",
     "Sai SSID hoặc password → kiểm tra config.h dòng WIFI_SSID / WIFI_PASSWORD"),
    (r"i2s_driver_install.*lỗi|i2s.*error",
     "Xung đột I2S hoặc sai GPIO → kiểm tra nối dây MIC_SCK/WS/SD trong config.h"),
    (r"Timeout sau|timeout",
     "Server không respond → kiểm tra WiFi còn kết nối không"),
    (r"JSON parse lỗi",
     "Response không phải JSON hợp lệ → có thể Mira server trả lỗi HTML"),
    (r"Connection refused|ECONNREFUSED",
     "Không reach được server → kiểm tra MIRA_BASE_URL"),
    (r"Mất kết nối",
     "WiFi bị ngắt giữa chừng → di chuyển ESP32 lại gần router"),
    (r"Audio.*lỗi|error_mp3",
     "Lỗi phát audio → kiểm tra nối dây MAX98357A, tăng SPK_VOLUME"),
]

def detect_fix(text: str) -> str | None:
    for pattern, fix in PATTERNS:
        if re.search(pattern, text, re.IGNORECASE):
            return fix
    return None

def update_issues(text: str, fix: str | None):
    if fix:
        issue = {"msg": text[:80], "fix": fix}
        with log_lock:
            # Tránh duplicate
            if not any(i["fix"] == fix for i in state["issues"]):
                state["issues"].append(issue)
                if len(state["issues"]) > 5:
                    state["issues"].pop(0)

# ────────────────────────────────────────────
#  LOG
# ────────────────────────────────────────────
def push_log(level: str, text: str, fix: str | None = None):
    global _log_seq
    ts = datetime.now().strftime("%H:%M:%S")
    with log_lock:
        seq = _log_seq
        _log_seq += 1
        entry = {"seq": seq, "ts": ts, "level": level, "text": text, "fix": fix or ""}
        log_entries.append(entry)
        if len(log_entries) > 2000:
            log_entries.pop(0)
    # Ghi ra file (ngoài lock, tránh giữ lock khi I/O)
    with open(LOG_FILE, "a", encoding="utf-8") as f:
        prefix = f"[{ts}][{level.upper()}]"
        f.write(f"{prefix} {text}\n")
        if fix:
            f.write(f"{prefix} → FIX: {fix}\n")

# ────────────────────────────────────────────
#  SERIAL READER
# ────────────────────────────────────────────
def parse_state(line: str):
    if re.search(r"✓ IP:|OK! IP:", line):
        state["wifi"] = True
    if re.search(r"WiFi.*FAIL|Mất kết nối", line):
        state["wifi"] = False
    if re.search(r"\[STT\].+Anh nói:", line):
        state["last_user"] = line.split("Anh nói:")[-1].strip()
    if re.search(r"\[Mira\]", line) and "Sẵn sàng" not in line and "Khởi động" not in line:
        state["last_mira"] = re.sub(r"\[Mira\]\s*✓?\s*", "", line).strip()

def serial_reader():
    global _ser
    while True:
        with _ser_lock:
            cur_ser = _ser
        if cur_ser is None or not cur_ser.is_open:
            port = _find_esp32_port()
            if port:
                try:
                    s = serial.Serial(port, 115200, timeout=1)
                    with _ser_lock:
                        _ser = s
                    state["esp32"] = True
                    state["port"] = port
                    push_log("system", f"✓ ESP32 kết nối tại {port}")
                except Exception as e:
                    push_log("error", f"Không mở port {port}: {e}")
                    time.sleep(2)
                    continue
            else:
                state["esp32"] = False
                state["wifi"]  = False
                state["port"]  = None
                time.sleep(2)
                continue

        with _ser_lock:
            s = _ser
        try:
            raw = s.readline()
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            parse_state(line)
            fix = detect_fix(line)
            update_issues(line, fix)
            level = _classify(line)
            push_log(level, line, fix)
        except serial.SerialException:
            state["esp32"] = False
            state["wifi"]  = False
            push_log("error", "ESP32 bị ngắt kết nối")
            with _ser_lock:
                try: _ser.close()
                except Exception: pass
                _ser = None
            time.sleep(1)

def _find_esp32_port() -> str | None:
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        vid  = p.vid or 0
        if any(k in desc for k in ["cp210", "ch340", "ch341", "usb serial", "uart", "esp"]):
            return p.device
        if vid in (0x10C4, 0x1A86, 0x303A):  # Silicon Labs, WCH, Espressif
            return p.device
    return None

def _classify(line: str) -> str:
    if re.search(r"✗|ERROR|FAIL|lỗi", line, re.IGNORECASE):
        return "error"
    if "→ FIX:" in line or "→" in line and "fix" in line.lower():
        return "fix"
    if re.search(r"\[Mira\].*✓", line):
        return "mira"
    if re.search(r"\[STT\].*Anh nói", line):
        return "user"
    if re.search(r"\[WiFi\]|\[Mic\]|\[Speaker\]|\[BOOT\]|\[MEM\]", line):
        return "system"
    if re.search(r"⚠|Warning|warn", line, re.IGNORECASE):
        return "warn"
    return "info"

# ────────────────────────────────────────────
#  MIRA CHECKER
# ────────────────────────────────────────────
def mira_checker():
    while True:
        try:
            urllib.request.urlopen(MIRA_URL + "/healthz", timeout=6)
            state["mira"] = True
        except Exception:
            state["mira"] = False
        time.sleep(30)

# ────────────────────────────────────────────
#  ACTIONS
# ────────────────────────────────────────────
def _find_pio() -> str | None:
    # Tìm pio CLI
    pio = shutil.which("pio")
    if pio: return pio
    for p in [
        Path.home() / ".platformio/penv/Scripts/pio.exe",  # Windows
        Path.home() / ".platformio/penv/bin/pio",           # Mac/Linux
    ]:
        if p.exists():
            return str(p)
    return None

def action_upload():
    if state["upload_running"]:
        push_log("warn", "Upload đang chạy rồi")
        return
    def run():
        state["upload_running"] = True
        state["issues"].clear()
        push_log("system", "▶ Bắt đầu build + upload firmware...")

        pio = _find_pio()
        if not pio:
            push_log("error", "Không tìm thấy PlatformIO CLI",
                     "Cài PlatformIO extension trong VS Code, hoặc chạy: pip install platformio")
            state["upload_running"] = False
            return

        cmd = [pio, "run", "--target", "upload",
               "--project-dir", str(PROJECT_DIR)]
        push_log("system", f"CMD: {' '.join(cmd)}")

        # Tạm ngắt serial để nhường port cho upload
        with _ser_lock:
            if _ser and _ser.is_open:
                try: _ser.close()
                except: pass

        try:
            proc = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            for line in proc.stdout:
                line = line.rstrip()
                if not line: continue
                fix  = detect_fix(line)
                lvl  = "error" if re.search(r"error|failed", line, re.IGNORECASE) else \
                       "system" if re.search(r"success|done|uploading", line, re.IGNORECASE) else "info"
                push_log(lvl, f"[PIO] {line}", fix)

            proc.wait()
            if proc.returncode == 0:
                push_log("system", "✓ Upload thành công! ESP32 đang khởi động lại...")
            else:
                push_log("error", f"✗ Upload thất bại (exit {proc.returncode})",
                         "Xem log PIO ở trên để tìm nguyên nhân")
        except Exception as e:
            push_log("error", f"✗ Lỗi chạy PIO: {e}")
        finally:
            state["upload_running"] = False

    threading.Thread(target=run, daemon=True).start()

def action_reset():
    with _ser_lock:
        s = _ser
    if not s or not s.is_open:
        push_log("warn", "ESP32 chưa kết nối — không thể reset")
        return
    try:
        # DTR nối với EN pin qua tụ. False = pull EN low = reset.
        # Nếu không reset được → thử đổi ngược: True trước rồi False
        s.dtr = False
        time.sleep(0.1)
        s.dtr = True
        push_log("system", "✓ ESP32 đã reset (DTR toggle)")
        state["issues"].clear()
    except Exception as e:
        push_log("error", f"✗ Reset thất bại: {e}")

def action_test_mira():
    def run():
        push_log("system", f"Ping Mira server ({MIRA_URL}/healthz)...")
        t0 = time.time()
        try:
            urllib.request.urlopen(MIRA_URL + "/healthz", timeout=10)
            ms = int((time.time() - t0) * 1000)
            state["mira"] = True
            push_log("system", f"✓ Mira server OK ({ms}ms)")
        except Exception as e:
            state["mira"] = False
            push_log("error", f"✗ Mira server không reach được: {e}",
                     "Kiểm tra MIRA_BASE_URL trong config.h hoặc vào HF Space wake up")
    threading.Thread(target=run, daemon=True).start()

# ────────────────────────────────────────────
#  FLASK ROUTES
# ────────────────────────────────────────────
@app.route("/")
def index():
    return HTML_PAGE

@app.route("/api/state")
def api_state():
    return jsonify(state)

@app.route("/api/log/latest")
def api_log_latest():
    """
    Claude đọc endpoint này để xem log.
    GET /api/log/latest?n=100
    """
    n = int(request.args.get("n", 100))
    with log_lock:
        total   = len(log_entries)
        entries = list(log_entries[-n:])
    return jsonify({
        "total": total,
        "returned": len(entries),
        "state": state,
        "entries": entries,
    })

@app.route("/api/diagnose")
def api_diagnose():
    """Auto-diagnose dựa trên log gần nhất — Claude gọi để hiểu vấn đề."""
    with log_lock:
        recent = log_entries[-50:]
    issues = []
    for e in recent:
        if e.get("fix"):
            issues.append({"log": e["text"], "fix": e["fix"], "ts": e["ts"]})
    # Dedup
    seen = set()
    unique = []
    for i in issues:
        if i["fix"] not in seen:
            seen.add(i["fix"])
            unique.append(i)
    return jsonify({
        "status": "ok" if not unique else "issues_found",
        "issue_count": len(unique),
        "issues": unique,
        "device_state": state,
    })

@app.route("/api/log/export")
def api_log_export():
    """Download full log file."""
    if LOG_FILE.exists():
        return send_file(str(LOG_FILE), as_attachment=True,
                         download_name="mira_log.txt", mimetype="text/plain")
    with log_lock:
        text = "\n".join(f"[{e['ts']}][{e['level']}] {e['text']}" for e in log_entries)
    buf = io.BytesIO(text.encode("utf-8"))
    return send_file(buf, as_attachment=True,
                     download_name="mira_log.txt", mimetype="text/plain")

@app.route("/api/action/upload", methods=["POST"])
def api_upload():
    action_upload()
    return jsonify({"ok": True})

@app.route("/api/action/reset", methods=["POST"])
def api_reset():
    action_reset()
    return jsonify({"ok": True})

@app.route("/api/action/test-mira", methods=["POST"])
def api_test_mira():
    action_test_mira()
    return jsonify({"ok": True})

@app.route("/api/action/clear-log", methods=["POST"])
def api_clear_log():
    with log_lock:
        log_entries.clear()
        state["issues"].clear()
    if LOG_FILE.exists():
        LOG_FILE.unlink()
    return jsonify({"ok": True})

@app.route("/api/stream")
def api_stream():
    """SSE stream cho log realtime."""
    def generate():
        # Track bằng seq (monotonic) thay vì list index
        # → robust khi log_entries.pop(0) shift index hoặc khi clear
        last_seq = -1
        while True:
            with log_lock:
                new_entries = [e for e in log_entries if e["seq"] > last_seq]
            if new_entries:
                last_seq = new_entries[-1]["seq"]
                for e in new_entries:
                    yield f"data: {json.dumps(e, ensure_ascii=False)}\n\n"
            time.sleep(0.15)
    return Response(generate(), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})

# ────────────────────────────────────────────
#  HTML UI
# ────────────────────────────────────────────
HTML_PAGE = """<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mira Dev Tool</title>
<style>
*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0d0d0d;--surface:#161616;--surface2:#1e1e1e;--border:#252525;
  --accent:#b04832;--green:#22c55e;--yellow:#eab308;--red:#ef4444;--blue:#60a5fa;
  --text:#f0ece8;--muted:#6b6460;--fix:#fbbf24;
}
body{background:var(--bg);color:var(--text);font-family:-apple-system,'Segoe UI',sans-serif;
  height:100vh;display:flex;flex-direction:column;overflow:hidden}

/* ── HEADER ── */
header{padding:12px 20px;border-bottom:1px solid var(--border);
  display:flex;align-items:center;gap:20px;flex-shrink:0}
.logo{font-size:13px;font-weight:700;letter-spacing:.1em;color:var(--accent)}
.badges{display:flex;gap:10px;flex:1}
.badge{display:flex;align-items:center;gap:5px;font-size:11px;color:var(--muted);
  background:var(--surface);border:1px solid var(--border);border-radius:6px;padding:4px 8px}
.dot{width:7px;height:7px;border-radius:50%;background:var(--red);transition:background .4s}
.dot.on{background:var(--green)}
.badge.uploading .dot{background:var(--yellow);animation:blink 1s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.3}}
.claude-tip{font-size:11px;color:var(--muted);background:var(--surface);
  border:1px solid var(--border);border-radius:6px;padding:5px 10px;white-space:nowrap}
.claude-tip code{color:var(--blue);font-family:monospace;font-size:10px}

/* ── LAYOUT ── */
main{flex:1;display:grid;grid-template-columns:220px 1fr 240px;overflow:hidden}

/* ── LEFT: ACTIONS ── */
.actions{border-right:1px solid var(--border);padding:14px;display:flex;flex-direction:column;gap:8px}
.section-label{font-size:10px;text-transform:uppercase;letter-spacing:.08em;
  color:var(--muted);margin-bottom:2px;margin-top:8px}
.section-label:first-child{margin-top:0}
.btn{width:100%;padding:9px 12px;border-radius:9px;border:1px solid var(--border);
  background:var(--surface2);color:var(--text);cursor:pointer;font-size:13px;
  text-align:left;display:flex;align-items:center;gap:8px;transition:all .15s}
.btn:hover{border-color:var(--accent);color:var(--accent)}
.btn:active{opacity:.7}
.btn.primary{background:var(--accent);border-color:var(--accent);color:#fff;font-weight:600}
.btn.primary:hover{opacity:.9}
.btn:disabled{opacity:.4;cursor:not-allowed}
.icon{font-size:14px}

/* ── CENTER: LOG ── */
.log-panel{display:flex;flex-direction:column}
.log-bar{padding:8px 14px;border-bottom:1px solid var(--border);display:flex;
  align-items:center;justify-content:space-between;flex-shrink:0}
.log-bar span{font-size:11px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}
.log-bar-right{display:flex;gap:8px}
.log-bar-btn{background:none;border:1px solid var(--border);color:var(--muted);
  border-radius:6px;padding:3px 10px;cursor:pointer;font-size:11px}
.log-bar-btn:hover{border-color:var(--accent);color:var(--accent)}
#logEl{flex:1;overflow-y:auto;padding:4px 0;font-family:'Cascadia Code','Fira Code',monospace;font-size:11.5px}
.ll{display:flex;gap:10px;padding:2px 14px;line-height:1.6}
.ll:hover{background:rgba(255,255,255,.025)}
.lts{color:var(--muted);min-width:58px;user-select:none;flex-shrink:0}
.lt{flex:1;white-space:pre-wrap;word-break:break-all}
.ll.error .lt{color:var(--red)}
.ll.mira  .lt{color:#93c5fd}
.ll.user  .lt{color:#86efac}
.ll.warn  .lt{color:var(--yellow)}
.ll.system .lt{color:var(--muted)}
.ll.fix   .lt{color:var(--fix)}
.fix-tag{font-size:10px;background:rgba(251,191,36,.15);color:var(--fix);
  border-radius:4px;padding:1px 5px;margin-top:2px;display:block;white-space:normal}

/* ── RIGHT: DIAGNOSE + CONV ── */
.sidebar{border-left:1px solid var(--border);padding:12px;
  display:flex;flex-direction:column;gap:12px;overflow-y:auto}
.card{background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:12px}
.card-title{font-size:10px;text-transform:uppercase;letter-spacing:.08em;
  color:var(--muted);margin-bottom:8px}
.issue-item{margin-bottom:8px;padding-bottom:8px;border-bottom:1px solid var(--border)}
.issue-item:last-child{border-bottom:none;margin-bottom:0;padding-bottom:0}
.issue-msg{font-size:11px;color:var(--red);margin-bottom:3px;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.issue-fix{font-size:11px;color:var(--fix);line-height:1.4}
.no-issues{font-size:12px;color:var(--green);text-align:center;padding:8px 0}
.conv-label{font-size:10px;color:var(--muted);margin-bottom:4px}
.conv-text{font-size:12px;line-height:1.5;color:var(--text);background:var(--bg);
  border-radius:7px;padding:7px 9px;min-height:32px;border:1px solid var(--border)}
.conv-text.mira-t{border-color:rgba(176,72,50,.3)}
.port-row{font-size:11px;color:var(--muted);display:flex;justify-content:space-between}
.port-row span:last-child{font-family:monospace;color:var(--text)}
</style>
</head>
<body>
<header>
  <span class="logo">MIRA DEV TOOL</span>
  <div class="badges">
    <div class="badge" id="badgeEsp"><div class="dot" id="dotEsp"></div>ESP32</div>
    <div class="badge" id="badgeWifi"><div class="dot" id="dotWifi"></div>WiFi</div>
    <div class="badge" id="badgeMira"><div class="dot" id="dotMira"></div>Mira Server</div>
    <div class="badge" id="badgeUpload" style="display:none">
      <div class="dot uploading"></div>Đang upload...
    </div>
  </div>
  <div class="claude-tip">Claude API: <code>GET /api/log/latest</code> | <code>/api/diagnose</code></div>
</header>

<main>
  <!-- ACTIONS -->
  <div class="actions">
    <div class="section-label">Firmware</div>
    <button class="btn primary" id="btnUpload" onclick="doAction('upload')">
      <span class="icon">⬆</span> Build &amp; Upload
    </button>
    <button class="btn" onclick="doAction('reset')">
      <span class="icon">↺</span> Reset ESP32
    </button>

    <div class="section-label">Server</div>
    <button class="btn" onclick="doAction('test-mira')">
      <span class="icon">📡</span> Ping Mira
    </button>

    <div class="section-label">Log</div>
    <button class="btn" onclick="exportLog()">
      <span class="icon">💾</span> Export Log
    </button>
    <button class="btn" onclick="doAction('clear-log')">
      <span class="icon">🗑</span> Xóa Log
    </button>

    <div class="section-label" style="margin-top:auto">Claude API</div>
    <button class="btn" onclick="copyApi('/api/log/latest')">
      <span class="icon">📋</span> Copy Log URL
    </button>
    <button class="btn" onclick="copyApi('/api/diagnose')">
      <span class="icon">🔍</span> Copy Diagnose URL
    </button>
  </div>

  <!-- LOG -->
  <div class="log-panel">
    <div class="log-bar">
      <span>Serial Log <span id="logCount" style="color:var(--accent)"></span></span>
      <div class="log-bar-right">
        <label style="font-size:11px;color:var(--muted);display:flex;align-items:center;gap:4px">
          <input type="checkbox" id="chkScroll" checked> Auto-scroll
        </label>
      </div>
    </div>
    <div id="logEl"></div>
  </div>

  <!-- SIDEBAR -->
  <div class="sidebar">
    <div class="card">
      <div class="card-title">Chẩn đoán</div>
      <div id="diagEl"><div class="no-issues">✓ Không phát hiện vấn đề</div></div>
    </div>

    <div class="card">
      <div class="card-title">Hội thoại</div>
      <div style="display:flex;flex-direction:column;gap:8px">
        <div>
          <div class="conv-label">Anh nói</div>
          <div class="conv-text" id="lastUser">—</div>
        </div>
        <div>
          <div class="conv-label">Mira trả lời</div>
          <div class="conv-text mira-t" id="lastMira">—</div>
        </div>
      </div>
    </div>

    <div class="card">
      <div class="card-title">Thông tin</div>
      <div style="display:flex;flex-direction:column;gap:5px">
        <div class="port-row"><span>Port</span><span id="portVal">—</span></div>
        <div class="port-row"><span>Entries</span><span id="entryCount">0</span></div>
      </div>
    </div>
  </div>
</main>

<script>
const logEl  = document.getElementById('logEl');
const chkSc  = document.getElementById('chkScroll');
let   logLen = 0;

// ── SSE stream ──
const es = new EventSource('/api/stream');
es.onmessage = e => {
  const d = JSON.parse(e.data);
  appendLog(d);
  logLen++;
  document.getElementById('logCount').textContent = '(' + logLen + ')';
  document.getElementById('entryCount').textContent = logLen;
};

function appendLog(d) {
  const row  = document.createElement('div');
  row.className = 'll ' + d.level;
  let fixHtml = '';
  if (d.fix) fixHtml = `<span class="fix-tag">→ FIX: ${esc(d.fix)}</span>`;
  row.innerHTML =
    `<span class="lts">${esc(d.ts)}</span>` +
    `<span class="lt">${esc(d.text)}${fixHtml}</span>`;
  logEl.appendChild(row);
  if (chkSc.checked) logEl.scrollTop = logEl.scrollHeight;
}

// ── State polling ──
async function pollState() {
  try {
    const r = await fetch('/api/state');
    const s = await r.json();
    setDot('dotEsp',  s.esp32, 'badgeEsp');
    setDot('dotWifi', s.wifi,  'badgeWifi');
    setDot('dotMira', s.mira,  'badgeMira');
    document.getElementById('portVal').textContent = s.port || '—';
    if (s.last_user) document.getElementById('lastUser').textContent = s.last_user;
    if (s.last_mira) document.getElementById('lastMira').textContent = s.last_mira;
    document.getElementById('btnUpload').disabled = s.upload_running;
    document.getElementById('badgeUpload').style.display = s.upload_running ? 'flex' : 'none';
    renderDiag(s.issues || []);
  } catch {}
}

function setDot(dotId, on) {
  document.getElementById(dotId).className = 'dot' + (on ? ' on' : '');
}

function renderDiag(issues) {
  const el = document.getElementById('diagEl');
  if (!issues.length) {
    el.innerHTML = '<div class="no-issues">✓ Không phát hiện vấn đề</div>';
    return;
  }
  el.innerHTML = issues.map(i =>
    `<div class="issue-item">
      <div class="issue-msg">⚠ ${esc(i.msg)}</div>
      <div class="issue-fix">→ ${esc(i.fix)}</div>
    </div>`
  ).join('');
}

// ── Actions ──
async function doAction(name) {
  const method = name === 'clear-log' ? 'POST' : 'POST';
  await fetch('/api/action/' + name, { method });
  if (name === 'clear-log') {
    logEl.innerHTML = '';
    logLen = 0;
    document.getElementById('logCount').textContent = '';
  }
}

function exportLog() {
  window.open('/api/log/export', '_blank');
}

function copyApi(path) {
  const url = window.location.origin + path;
  navigator.clipboard.writeText(url).then(() => {
    alert('Copied!\n' + url + '\n\nDán vào Claude: "đọc log tại URL này"');
  });
}

function esc(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

pollState();
setInterval(pollState, 2000);
</script>
</body>
</html>
"""

# ────────────────────────────────────────────
#  MAIN
# ────────────────────────────────────────────
if __name__ == "__main__":
    # Xóa log cũ khi khởi động
    if LOG_FILE.exists():
        LOG_FILE.unlink()

    push_log("system", "Mira Dev Tool khởi động...")
    push_log("system", f"Project: {PROJECT_DIR}")
    push_log("system", f"Log file: {LOG_FILE}")
    push_log("system", "Claude API: http://localhost:5555/api/log/latest")
    push_log("system", "Claude API: http://localhost:5555/api/diagnose")

    threading.Thread(target=serial_reader, daemon=True).start()
    threading.Thread(target=mira_checker,  daemon=True).start()

    threading.Timer(1.2, lambda: webbrowser.open("http://localhost:5555")).start()

    print("\n" + "="*55)
    print("  Mira Dev Tool đang chạy")
    print("  Browser: http://localhost:5555")
    print()
    print("  Claude đọc log: http://localhost:5555/api/log/latest")
    print("  Claude diagnose: http://localhost:5555/api/diagnose")
    print("  Ctrl+C để thoát")
    print("="*55 + "\n")

    app.run(host="0.0.0.0", port=5555, debug=False, threaded=True)
