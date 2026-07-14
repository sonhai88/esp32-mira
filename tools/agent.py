"""
Mira Agent — phần mềm chạy nền trên máy nhà. Cầu nối ESP32 ↔ Cloud Relay.

Máy này KHÔNG cần source code, git hay PlatformIO. Nó chỉ:
  · đọc serial ESP32 → đẩy log lên relay
  · nhận lệnh từ relay → ghi serial / flash firmware
  · flash: tải .bin (máy dev đã build) từ relay → esptool
  · tự cập nhật chính nó từ GitHub raw

Anh chỉ cắm dây USB. Mọi thứ khác tự chạy.
"""
import os, re, sys, json, time, threading, subprocess, shutil, hashlib, tempfile
from datetime import datetime
from pathlib import Path
import serial
import serial.tools.list_ports
import requests

# ── Config ──────────────────────────────────────────────────
RELAY_URL = os.environ.get("RELAY_URL", "https://deli2222-mira-relay.hf.space")
RELAY_KEY = os.environ.get("RELAY_KEY", "mira-relay-key-2026")
SYNC_INTERVAL = 1.0   # giây — push log lên cloud

# Nguồn tự cập nhật agent (repo public → không cần git trên máy nhà)
AGENT_RAW_URL = os.environ.get(
    "AGENT_RAW_URL",
    "https://raw.githubusercontent.com/sonhai88/esp32-mira/main/tools/agent.py")
UPDATE_INTERVAL = 120   # giây

APP_DIR = Path(__file__).resolve().parent

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
        if s is None:          # upload vừa đóng port giữa chừng — vòng sau mở lại
            continue
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
        except Exception as e:
            # Bất kỳ lỗi nào khác KHÔNG được giết thread đọc — nếu chết thì agent
            # vẫn ghi serial được nhưng không bao giờ đọc log về nữa (im lặng).
            push_log("error", f"Reader lỗi (bỏ qua dòng này): {type(e).__name__}: {e}")
            time.sleep(0.2)

# ── Flash firmware: tải .bin từ relay → esptool ───────────────
# Máy dev build và đẩy .bin lên relay. Máy này chỉ tải về và ghi vào chip.
_upload_lock_port = False  # ngăn serial reader mở lại port trong lúc flash

def _fetch_firmware() -> tuple[Path, str] | None:
    """Tải .bin từ relay, verify sha256. Trả (đường dẫn, commit) hoặc None."""
    try:
        meta = requests.get(f"{RELAY_URL}/api/firmware/meta", timeout=15)
        if meta.status_code == 404:
            push_log("error", "✗ Relay chưa có firmware",
                     "Bên máy dev chạy: python tools/mira.py flash")
            return None
        meta = meta.json()
        blob = requests.get(f"{RELAY_URL}/api/firmware/bin", timeout=120).content
    except Exception as e:
        push_log("error", f"✗ Tải firmware lỗi: {e}")
        return None

    sha = hashlib.sha256(blob).hexdigest()
    if sha != meta.get("sha256"):
        push_log("error", "✗ Firmware hỏng khi tải (sha256 không khớp) — thử lại")
        return None

    path = Path(tempfile.gettempdir()) / "mira-firmware.bin"
    path.write_bytes(blob)
    push_log("system", f"✓ Tải firmware OK — {len(blob)//1024} KB, commit {meta.get('commit')}")
    return path, meta.get("commit", "?")

def action_upload():
    if state["upload_running"]:
        push_log("warn", "Đang flash rồi")
        return
    def run():
        global _ser, _upload_lock_port
        state["upload_running"] = True
        _upload_lock_port = True
        try:
            got = _fetch_firmware()
            if not got:
                return
            fw_path, commit = got

            port = state.get("port") or _find_esp32_port()
            if not port:
                push_log("error", "✗ Không thấy ESP32 — kiểm tra dây USB")
                return

            # Nhả COM port trước khi esptool chiếm
            with _ser_lock:
                if _ser and _ser.is_open:
                    try: _ser.close()
                    except Exception: pass
                _ser = None
            time.sleep(2)   # đợi Windows release COM port

            push_log("system", f"▶ Flash commit {commit} qua {port}...")
            cmd = [sys.executable, "-m", "esptool", "--chip", "esp32",
                   "--port", port, "--baud", "460800",
                   "write_flash", "-z", "0x0", str(fw_path)]
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace")
            for line in proc.stdout:
                line = line.rstrip()
                if not line or line.startswith("Writing at"):
                    continue          # bỏ spam tiến độ
                push_log("info", f"[flash] {line}")
            proc.wait()

            if proc.returncode == 0:
                push_log("system", f"✓ Flash xong ({commit})! ESP32 đang khởi động lại...")
            else:
                push_log("error", f"✗ Flash thất bại (exit {proc.returncode})",
                         "Rút cắm lại dây USB rồi thử lại")
        except FileNotFoundError:
            push_log("error", "✗ Chưa cài esptool", "Chạy: pip install esptool")
        except Exception as e:
            push_log("error", f"✗ Lỗi flash: {type(e).__name__}: {e}")
        finally:
            state["upload_running"] = False
            _upload_lock_port = False   # cho serial reader kết nối lại
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
    # WiFi được biên dịch vào firmware → đổi SSID phải build lại bên máy dev.
    push_log("warn", f"Đổi WiFi ({ssid}) cần build lại firmware — báo máy dev chạy: "
                     f"mira.py flash --wifi")

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

def action_send_serial(cmd_text: str):
    """Ghi lệnh qua serial tới ESP32 đang chạy (điều khiển realtime, KHÔNG reflash)."""
    with _ser_lock:
        s = _ser
    if not s or not s.is_open:
        push_log("warn", f"ESP32 chưa kết nối — không gửi được lệnh '{cmd_text}'")
        return
    try:
        with _ser_lock:
            s.write((cmd_text + "\n").encode())
        push_log("system", f"→ Gửi ESP32: {cmd_text}")
    except Exception as e:
        push_log("error", f"✗ Gửi serial lỗi: {e}")

def action_update_agent():
    """Ép agent tải bản mới + restart ngay, không đợi vòng poll."""
    threading.Thread(target=lambda: _self_update(force=True), daemon=True).start()

COMMAND_HANDLERS = {
    "upload":       lambda: threading.Thread(target=action_upload, daemon=True).start(),
    "update-agent": action_update_agent,
    "reset":       action_reset,
    "test-mira":   lambda: threading.Thread(target=action_test_mira, daemon=True).start(),
    # Điều khiển ESP32 realtime qua serial (từ nút web)
    "self-test":   lambda: action_send_serial("TEST"),
    "play-music":  lambda: action_send_serial("MUSIC"),
    "test-screen": lambda: action_send_serial("SCREEN"),
    "test-mic":    lambda: action_send_serial("MIC"),
}

# ── Tự cập nhật: em push code lên GitHub → máy anh tự có bản mới ──
# Kéo thẳng file raw (repo public) → máy nhà không cần cài git.
def _self_update(force: bool = False) -> None:
    me = Path(__file__).resolve()
    try:
        r = requests.get(AGENT_RAW_URL, timeout=20)
        if r.status_code != 200:
            return
        new = r.content
    except Exception:
        return

    if hashlib.sha256(new).hexdigest() == hashlib.sha256(me.read_bytes()).hexdigest():
        if force:
            push_log("system", "✓ Agent đã là bản mới nhất")
        return

    # Ghi qua file tạm rồi thay thế — mất điện giữa chừng không để lại file hỏng
    tmp = me.with_suffix(".py.new")
    tmp.write_bytes(new)
    tmp.replace(me)

    push_log("system", "↻ Agent có bản mới — đang khởi động lại...")
    time.sleep(1.5)          # kịp đẩy nốt log lên relay
    try:
        os.execv(sys.executable, [sys.executable, *sys.argv])
    except Exception as e:
        push_log("error", f"Restart lỗi: {e} — hãy tắt/bật lại agent")

def auto_update():
    while True:
        time.sleep(UPDATE_INTERVAL)
        if not state["upload_running"]:   # đang flash thì không đụng vào code
            _self_update()


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
if __name__ == "__main__":
    print(f"\n{'='*52}")
    print(f"  Mira Agent — cắm dây USB là xong, không cần làm gì thêm")
    print(f"  Bảng điều khiển: {RELAY_URL}")
    print(f"  Cửa sổ này cứ để chạy nền. Ctrl+C để thoát.")
    print(f"{'='*52}\n")

    push_log("system", f"Agent khởi động → {RELAY_URL}")
    _self_update()   # nạp bản mới nhất ngay lúc bật máy

    threading.Thread(target=serial_reader, daemon=True).start()
    threading.Thread(target=cloud_sync,    daemon=True).start()
    threading.Thread(target=auto_update,   daemon=True).start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n  Agent dừng.")
