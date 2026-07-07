#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────
# Review pipeline cho firmware Mira — compile-check KHÔNG cần board
#
# Tầng 1 (script này): pio run → bắt lỗi build/syntax/API ngay tại máy,
#                      không phải chờ flash lên board mới biết.
# Tầng 2 (thủ công):   spawn reviewer agent soi bug logic khi feature lớn.
#
# Dùng: bash tools/check.sh
# Trả exit 0 nếu build sạch, 1 nếu fail (in lỗi gọn).
# ─────────────────────────────────────────────────────────────
set -u
cd "$(dirname "$0")/.."

PIO="$HOME/.local/bin/pio"
[ -x "$PIO" ] || PIO="$(command -v pio || true)"
[ -n "$PIO" ] || { echo "✗ Không tìm thấy pio — cài: pip install platformio"; exit 2; }

LOG="$(mktemp -t mira_build.XXXXXX.log)"
echo "▶ Compile-check firmware (pio run)..."

if "$PIO" run > "$LOG" 2>&1; then
  grep -E "RAM:|Flash:|\[SUCCESS\]|Took" "$LOG"
  echo "✓ Build sạch — code compile OK, an toàn để commit/flash"
  rm -f "$LOG"
  exit 0
else
  echo "✗ Build FAIL — lỗi cụ thể:"
  grep -E "error:|Error [0-9]|\[FAILED\]|undefined|not declared|syntax error" "$LOG" | head -25
  echo "  (log đầy đủ: $LOG)"
  exit 1
fi
