"""
Mira Agent — chạy trên máy nhà, bridge ESP32 ↔ Cloud Relay
Cách dùng: python tools/agent.py

Lần đầu: sửa RELAY_URL bên dưới (hoặc set env RELAY_URL)
"""
import os, re, sys, json, time, threading, subprocess, shutil
from datetime import datetime
from pathlib import Path
import serial
import serial.tools.list_ports
import requests

# ── Config ──────────────────────────────────────────────────
RELAY_URL = os.environ.get("RELAY_URL", "https://deli2222-mira-relay.hf.space")
RELAY_KEY = os.environ.get("RELAY_KEY", "mira-relay-key-2026")
SYNC_INTERVAL = 1.0   # giây — push log lên cloud

PROJECT_DIR = Path(__file__).parent.parent

# ── Shared state ─────────────────────────────────────────────
_log_buffer: list[dict] = []
_buf_lock   = threading.Lock()

state = {
    "esp32": False,
    "wifi":  False,
    "mira":  False,
    "port":  None,
    "last_user":  "",
    "last_mira":  "",
    "upload_running": False,
}

_ser      = None
_ser_lock = threading.Lock()

# ── Error patterns ────────────────────────────────────────────
PATTERNS = [
    (r"HTTP 401",           "DEVICE_API_KEY sai → mở include/config.h sửa dòng DEVICE_API_KEY"),
    (r"HTTP 503|HTTP 502",  "Mira server đang khởi động → đợi 60s rồi thử lại"),
    (r"HTTP 404",           "URL sai → kiểm tra MIRA_BASE_URL trong config.h"),
    (r"PSRAM not found|psramFound.*false",
                            "PSRAM không tìm thấy → platformio.ini: board_build.arduino.memory_type = qio_opi"),
    (r"ps_malloc.*thất bại|ps_malloc.*fail",
                            "Hết PSRAM → giảm RECORD_SECONDS trong config.h (thử 3)"),
    (r"WiFi.*FAIL|WiFi.*✗", "Sai SSID hoặc password → kiểm tra config.h"),
    (r"i2s_driver_install.*lỗi|i2s.*error",
                            "Xung đột I2S hoặc sai GPIO → kiểm tra nối dây"),
    (r"Timeout sau|timeout","Server không respond → kiểm tra WiFi"),
    (r"JSON parse lỗi",     "Response không phải JSON → Mira server có thể trả HTML lỗi"),
    (r"Connection refused", "Không reach được server → kiểm tra MIRA_BASE_URL"),
    (r"Mất kết nối",        "WiFi bị ngắt → di chuyển ESP32 lại gần router"),
    (r"Audio.*lỗi|error_mp3","Lỗi phát audio → kiểm tra nối dây MAX98357A"),
]

def _detect_fix(text: str):
    for pattern, fix in PATTERNS:
        if re.search(pattern, text, re.IGNORECASE):
            return fix
    return None

def _classify(line: str) -> str:
    if re.search(r"✗|ERROR|FAIL|lỗi", line, re.IGNORECASE): return "error"
    if re.search(r"\[Mira\].*✓", line):                      return "mira"
    if re.search(r"\[STT\].*Anh nói", line):                 return "user"
    if re.search(r"\[WiFi\]|\[Mic\]|\[Speaker\]|\[BOOT\]",   line): return "system"
    if re.search(r"⚠|Warning|warn", line, re.IGNORECASE):   return "warn"
    return "info"

def push_log(level: str, text: str, fix: str | None = None):
    ts    = datetime.now().strftime("%H:%M:%S")
    entry = {"ts": ts, "level": level, "text": text, "fix": fix or ""}
    with _buf_lock:
        _log_buffer.append(entry)
    # Console output
    icon = {"error": "✗", "mira": "◈", "user": "▶", "warn": "⚠"}.get(level, "·")
    print(f"  {icon} [{ts}] {text}")
    if fix:
        print(f"    → FIX: {fix}")

# ── Serial ────────────────────────────────────────────────────
def _find_esp32_port():
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        vid  = p.vid or 0
        if any(k in desc for k in ["cp210", "ch340", "ch341", "usb serial", "uart", "esp"]):
            return p.device
        if vid in (0x10C4, 0x1A86, 0x303A):
            return p.device
    return None

def _parse_state(line: str):
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
        if _upload_lock_port:
            time.sleep(1)
            continue
        with _ser_lock:
            cur = _ser
        if cur is None or not cur.is_open:
            port = _find_esp32_port()
            if port:
                try:
                    s = serial.Serial(port, 115200, timeout=1)
                    with _ser_lock:
                        _ser = s
                    state["esp32"] = True
                    state["port"]  = port
                    push_log("system", f"✓ ESP32 kết nối tại {port}")
                except Exception as e:
                    push_log("error", f"Không mở port {port}: {e}")
                    time.sleep(2)
                    continue
            else:
                if state["esp32"]:
                    state["esp32"] = False
                    state["wifi"]  = False
                    state["port"]  = None
                    push_log("warn", "Không thấy ESP32 — hãy cắm USB vào")
                time.sleep(2)
                continue

        with _ser_lock:
            s = _ser
        try:
            raw  = s.readline()
            line = raw.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            _parse_state(line)
            fix   = _detect_fix(line)
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

# ── Actions ───────────────────────────────────────────────────
def _find_pio():
    pio = shutil.which("pio")
    if pio: return pio
    for p in [
        Path.home() / ".platformio/penv/Scripts/pio.exe",
        Path.home() / ".platformio/penv/bin/pio",
    ]:
        if p.exists(): return str(p)
    return None

_upload_lock_port = False  # ngăn serial reader mở lại port trong lúc upload

def action_upload():
    if state["upload_running"]:
        push_log("warn", "Upload đang chạy rồi")
        return
    def run():
        global _ser, _upload_lock_port
        state["upload_running"] = True
        _upload_lock_port = True  # block serial reader
        push_log("system", "▶ Bắt đầu build + upload firmware...")
        pio = _find_pio()
        if not pio:
            push_log("error", "Không tìm thấy PlatformIO CLI",
                     "Cài PlatformIO extension trong VS Code")
            state["upload_running"] = False
            _upload_lock_port = False
            return
        cmd = [pio, "run", "--target", "upload", "--project-dir", str(PROJECT_DIR)]
        # Đóng serial và đợi port ổn định
        with _ser_lock:
            if _ser and _ser.is_open:
                try: _ser.close()
                except: pass
            _ser = None
        time.sleep(3)  # đợi Windows release COM port
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace")
            for line in proc.stdout:
                line = line.rstrip()
                if not line: continue
                fix = _detect_fix(line)
                lvl = "error" if re.search(r"error|failed", line, re.IGNORECASE) else \
                      "system" if re.search(r"success|done|uploading", line, re.IGNORECASE) else "info"
                push_log(lvl, f"[PIO] {line}", fix)
            proc.wait()
            if proc.returncode == 0:
                push_log("system", "✓ Upload thành công! ESP32 đang khởi động lại...")
            else:
                push_log("error", f"✗ Upload thất bại (exit {proc.returncode})")
        except Exception as e:
            push_log("error", f"✗ Lỗi chạy PIO: {e}")
        finally:
            state["upload_running"] = False
            _upload_lock_port = False  # cho phép serial reader kết nối lại
    threading.Thread(target=run, daemon=True).start()

def action_reset():
    with _ser_lock:
        s = _ser
    if not s or not s.is_open:
        push_log("warn", "ESP32 chưa kết nối")
        return
    try:
        s.dtr = False
        time.sleep(0.1)
        s.dtr = True
        push_log("system", "✓ ESP32 đã reset")
    except Exception as e:
        push_log("error", f"✗ Reset thất bại: {e}")

def action_set_wifi(ssid: str, password: str):
    config_path   = PROJECT_DIR / "include" / "config.h"
    template_path = PROJECT_DIR / "include" / "config.h.example"
    if config_path.exists():
        content = config_path.read_text(encoding="utf-8")
    elif template_path.exists():
        content = template_path.read_text(encoding="utf-8")
        push_log("system", "Tạo config.h mới từ template")
    else:
        push_log("error", "Không tìm thấy config.h hoặc config.h.example")
        return
    content = re.sub(r'#define WIFI_SSID\s+"[^"]*"',
                     f'#define WIFI_SSID     "{ssid}"', content)
    content = re.sub(r'#define WIFI_PASSWORD\s+"[^"]*"',
                     f'#define WIFI_PASSWORD "{password}"', content)
    config_path.write_text(content, encoding="utf-8")
    push_log("system", f"✓ Đã ghi config.h — WiFi: {ssid}")
    action_upload()

def action_test_mira():
    def run():
        mira_url = "https://deli2222-mira-ai.hf.space"
        push_log("system", f"Ping Mira server...")
        t0 = time.time()
        try:
            requests.get(mira_url + "/healthz", timeout=10)
            ms = int((time.time() - t0) * 1000)
            state["mira"] = True
            push_log("system", f"✓ Mira server OK ({ms}ms)")
        except Exception as e:
            state["mira"] = False
            push_log("error", f"✗ Mira không reach được: {e}")
    threading.Thread(target=run, daemon=True).start()

COMMAND_HANDLERS = {
    "upload":    lambda: threading.Thread(target=action_upload,    daemon=True).start(),
    "reset":     action_reset,
    "test-mira": lambda: threading.Thread(target=action_test_mira, daemon=True).start(),
}

# ── Cloud sync ────────────────────────────────────────────────
_relay_ok = False

def cloud_sync():
    global _relay_ok
    while True:
        time.sleep(SYNC_INTERVAL)
        with _buf_lock:
            batch = _log_buffer.copy()
            _log_buffer.clear()

        payload = {
            "key":   RELAY_KEY,
            "state": state,
            "logs":  batch,
        }

        try:
            resp   = requests.post(f"{RELAY_URL}/ingest", json=payload, timeout=6)
            result = resp.json()

            if not _relay_ok:
                _relay_ok = True
                print(f"  · [relay] ✓ Kết nối thành công → {RELAY_URL}")

            cmd = result.get("command")
            if cmd:
                name   = cmd.get("name")
                params = cmd.get("params", {})
                push_log("system", f"← Lệnh từ cloud: {name}")
                if name == "set-wifi":
                    threading.Thread(
                        target=lambda: action_set_wifi(
                            params.get("ssid", ""), params.get("password", "")),
                        daemon=True).start()
                else:
                    handler = COMMAND_HANDLERS.get(name)
                    if handler:
                        handler()

        except requests.exceptions.RequestException as e:
            if _relay_ok:
                _relay_ok = False
                print(f"  · [relay] ✗ Mất kết nối relay: {e}")
            with _buf_lock:
                _log_buffer[:0] = batch
        except Exception as e:
            with _buf_lock:
                _log_buffer[:0] = batch

# ── Main ──────────────────────────────────────────────────────
def _ensure_config():
    config_path   = PROJECT_DIR / "include" / "config.h"
    template_path = PROJECT_DIR / "include" / "config.h.example"
    if not config_path.exists():
        if template_path.exists():
            import shutil as _sh
            _sh.copy(template_path, config_path)
            print(f"  · Tạo config.h từ template — dùng form WiFi trên relay để điền mật khẩu")
        else:
            print(f"  ⚠ Không tìm thấy config.h.example, hãy tạo include/config.h thủ công")

if __name__ == "__main__":
    _ensure_config()

    print(f"\n{'='*52}")
    print(f"  Mira Agent")
    print(f"  Relay : {RELAY_URL}")
    print(f"  UI    : {RELAY_URL}")
    print(f"  Log API: {RELAY_URL}/api/log/latest")
    print(f"  Ctrl+C để thoát")
    print(f"{'='*52}\n")

    push_log("system", f"Agent khởi động → {RELAY_URL}")

    threading.Thread(target=serial_reader, daemon=True).start()
    threading.Thread(target=cloud_sync,    daemon=True).start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n  Agent dừng.")
