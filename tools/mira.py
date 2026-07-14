#!/usr/bin/env python3
"""
mira — CLI điều khiển Mira từ máy dev (máy này).

Máy này giữ source, build firmware, đẩy .bin lên relay và xem log/thông số.
Máy nhà chỉ cắm dây USB + chạy agent.

    mira status              trạng thái thiết bị (bảng)
    mira log [n]             n dòng log gần nhất (mặc định 40)
    mira watch               theo dõi log realtime
    mira build               build firmware (không flash)
    mira flash               build → đẩy .bin lên relay → ra lệnh agent flash
    mira mic|music|screen|test    lệnh realtime tới ESP32 (không reflash)
    mira reset               reset ESP32
    mira agent-update        ép agent ở nhà tải bản mới + restart
"""
import os, sys, time, json, subprocess, hashlib
from pathlib import Path
import requests

RELAY = os.environ.get("RELAY_URL", "https://deli2222-mira-relay.hf.space")
KEY   = os.environ.get("RELAY_KEY", "mira-relay-key-2026")
ROOT  = Path(__file__).resolve().parent.parent
BUILD = ROOT / ".pio" / "build" / "esp32dev"
PIO   = Path.home() / ".pio-venv" / "bin" / "pio"
ESPTOOL  = Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py"
BOOT_APP = Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

C = {"g": "\033[32m", "r": "\033[31m", "y": "\033[33m", "d": "\033[2m", "b": "\033[1m", "x": "\033[0m"}
def c(t, k): return f"{C[k]}{t}{C['x']}"
def die(m):  print(c(f"✗ {m}", "r")); sys.exit(1)


# ── Relay ─────────────────────────────────────────────────────
def get(path: str, timeout=20) -> tuple[int, dict]:
    r = requests.get(f"{RELAY}{path}", timeout=timeout)
    try:    return r.status_code, r.json()
    except Exception: return r.status_code, {}

def post(path: str, data: bytes = b"", headers: dict | None = None, timeout=180):
    r = requests.post(f"{RELAY}{path}", data=data, headers=headers or {}, timeout=timeout)
    try:    return r.json()
    except Exception: return {}

def action(name: str):
    post(f"/api/action/{name}")
    print(c(f"→ đã gửi lệnh: {name}", "g"))


# ── Build ─────────────────────────────────────────────────────
def build() -> None:
    if not PIO.exists():
        die(f"Không thấy pio ở {PIO}")
    print(c("▶ Build firmware...", "b"))
    r = subprocess.run([str(PIO), "run", "--project-dir", str(ROOT)],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    if "[SUCCESS]" not in out:                # exit code không đáng tin — grep SUCCESS
        print("\n".join(out.splitlines()[-25:]))
        die("Build FAILED")
    for line in out.splitlines():
        if line.startswith(("RAM:", "Flash:")):
            print("  " + line)
    print(c("✓ Build sạch", "g"))

def merge() -> Path:
    """Gộp bootloader+partitions+boot_app0+firmware → 1 file flash @0x0."""
    out = BUILD / "mira-merged.bin"
    r = subprocess.run([
        sys.executable, str(ESPTOOL), "--chip", "esp32", "merge_bin", "-o", str(out),
        "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
        "0x1000",  str(BUILD / "bootloader.bin"),
        "0x8000",  str(BUILD / "partitions.bin"),
        "0xe000",  str(BOOT_APP),
        "0x10000", str(BUILD / "firmware.bin"),
    ], capture_output=True, text=True)
    if r.returncode != 0 or not out.exists():
        die(f"merge_bin lỗi: {r.stderr[-300:]}")
    return out

def commit_hash() -> str:
    r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=str(ROOT),
                       capture_output=True, text=True)
    return r.stdout.strip() or "?"

def flash():
    build()
    blob = merge().read_bytes()
    sha  = hashlib.sha256(blob).hexdigest()
    ch   = commit_hash()
    print(c(f"▶ Đẩy firmware lên relay — {len(blob)//1024} KB, commit {ch}", "b"))
    res = post("/api/firmware", blob,
               {"X-Relay-Key": KEY, "X-Commit": ch,
                "Content-Type": "application/octet-stream"})
    if res.get("sha256") != sha:
        die("Relay nhận sai firmware (sha256 lệch)")
    print(c("✓ Relay đã nhận", "g"))
    action("upload")
    print(c("→ Agent ở nhà đang tải về và flash. Xem: mira watch", "d"))


# ── Xem ───────────────────────────────────────────────────────
def status():
    try:
        _, d = get("/api/log/latest?n=1")
    except Exception as e:
        die(f"Không reach được relay: {e}")
    s = d.get("state", {})

    def dot(v):
        if v is True:  return c("● có", "g")
        if v is False: return c("● không", "r")
        return c("○ chưa rõ", "d")

    print(c("\n  MIRA — trạng thái\n", "b"))
    rows = [
        ("ESP32 cắm dây", dot(s.get("esp32")), s.get("port") or ""),
        ("WiFi",          dot(s.get("wifi")),  ""),
        ("Server AI",     dot(s.get("mira")),  ""),
        ("Loa",           dot(s.get("speaker_ok")), ""),
        ("Màn OLED",      dot(s.get("oled_ok")),
                          f"addr {s['oled_addr']}" if s.get("oled_addr") else "chưa cắm?"),
        ("Mic biên độ",   c(str(s.get("mic_amp")), "g") if (s.get("mic_amp") or 0) >= 80
                          else c(str(s.get("mic_amp")), "r") if s.get("mic_amp") is not None
                          else c("○ chưa đo", "d"),
                          "cần ≥80 mới coi là sống"),
        ("Mặt / cảm xúc", c(f"{s.get('face_state') or '—'} / {s.get('emotion')}", "y"), ""),
        ("Đang flash",    dot(s.get("upload_running")), ""),
    ]
    for k, v, note in rows:
        print(f"  {k:<16} {v:<22} {c(note, 'd')}")

    print(f"\n  {c('Lần cuối thấy', 'd')}  {s.get('last_seen') or c('chưa bao giờ', 'r')}")
    code, fw = get("/api/firmware/meta")
    if code == 200:
        print(f"  {c('Firmware trên relay', 'd')}  commit {fw.get('commit')} "
              f"({fw.get('size', 0)//1024} KB, {fw.get('ts')})")
    else:
        print(f"  {c('Firmware trên relay', 'd')}  {c('chưa có — chạy: mira flash', 'y')}")
    print(f"  {c('Code máy này', 'd')}  commit {commit_hash()}\n")

def show_log(n=40):
    _, d = get(f"/api/log/latest?n={n}")
    ent = d.get("entries", [])
    if not ent:
        print(c("  (log trống — agent ở nhà chưa chạy, hoặc relay vừa restart)", "y"))
        return
    for e in ent:
        lvl  = e.get("level", "info")
        tint = {"error": "r", "warn": "y", "system": "g", "mira": "b"}.get(lvl, "d")
        print(f"  {c(e['ts'], 'd')} {c(e['text'], tint)}")
        if e.get("fix"):
            print(f"           {c('→ FIX: ' + e['fix'], 'y')}")

def watch():
    print(c(f"  Theo dõi log — Ctrl+C để dừng\n", "d"))
    seen = set()
    while True:
        try:
            _, d = get("/api/log/latest?n=30")
            for e in d.get("entries", []):
                k = (e["ts"], e["text"])
                if k in seen:
                    continue
                seen.add(k)
                lvl  = e.get("level", "info")
                tint = {"error": "r", "warn": "y", "system": "g", "mira": "b"}.get(lvl, "d")
                print(f"  {c(e['ts'], 'd')} {c(e['text'], tint)}")
                if e.get("fix"):
                    print(f"           {c('→ FIX: ' + e['fix'], 'y')}")
        except Exception:
            pass
        time.sleep(2)


CMDS = {
    "test":   lambda: action("self-test"),
    "music":  lambda: action("play-music"),
    "screen": lambda: action("test-screen"),
    "mic":    lambda: action("test-mic"),
    "reset":  lambda: action("reset"),
    "agent-update": lambda: action("update-agent"),
}

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    try:
        if   cmd == "status": status()
        elif cmd == "log":    show_log(int(sys.argv[2]) if len(sys.argv) > 2 else 40)
        elif cmd == "watch":  watch()
        elif cmd == "build":  build()
        elif cmd == "flash":  flash()
        elif cmd in CMDS:     CMDS[cmd]()
        else:
            print(__doc__)
    except KeyboardInterrupt:
        print()
