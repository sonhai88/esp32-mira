# MIRA — HANDOFF cho session mới

> Dán/đọc file này khi bắt đầu conversation mới để hiểu ngay project đang ở đâu.
> Cập nhật lần cuối: 2026-07-13.

## 1. Mira là gì
Thiết bị **voice AI tiếng Việt** trên ESP32 (như Xiaozhi thu nhỏ). Giữ nút → nói → server nhận dạng (STT) → LLM trả lời → phát loa (TTS). Có mặt cảm xúc trên OLED.

## 2. Phần cứng
- Board: **ESP32-WROOM + CP2102, KHÔNG PSRAM** (không phải S3).
- Mic **INMP441** (I2S): SCK=14, WS=15, SD=34, **L/R phải nối GND**.
- Loa **MAX98357A** (I2S): BCLK=26, LRC=25, DIN=22.
- Màn **OLED SSD1306** (I2C): SDA=21, SCL=19, addr 0x3C.
- Nút = nút BOOT (GPIO0) — **CHỈ để nói (voice)**.
- GPIO nằm trong `platformio.ini` build_flags (KHÔNG để trong config.h kẻo override).

## 3. Kiến trúc 3 tầng + server
| Thành phần | Đường dẫn | Vai trò |
|---|---|---|
| Firmware | `/Volumes/hai/esp32-mira/src/main.cpp` + `face.cpp` | Chạy trên ESP32 (PlatformIO/Arduino) |
| Agent | `/Volumes/hai/esp32-mira/tools/agent.py` | Chạy trên **PC Windows nhà anh**, đọc serial ESP32 → push log lên relay; nhận lệnh từ relay → ghi serial / flash |
| Relay | `/Volumes/hai/esp32-mira/relay/` (repo riêng) | Deploy HF Space `deli2222-mira-relay.hf.space` — dashboard web + API |
| Server AI | `/Volumes/hai/hologram-robot/backend/main.py` ⚠️ | FastAPI "Mira", deploy `deli2222-mira-ai.hf.space`. Endpoint /stt /chat/stream /tts/stream. `/chat/stream` trả `emotion` |

Repo GitHub firmware: `sonhai88/esp32-mira`. Relay push HF: `git push hf HEAD:main`.

## 4. Cách làm việc (QUAN TRỌNG)
- **Anh KHÔNG ở cùng máy này** — anh cắm board ở PC nhà. Em điều khiển qua relay.
- **Compile-check không cần board**: `bash /Volumes/hai/esp32-mira/tools/check.sh` (pio ở `~/.pio-venv`). LUÔN chạy trước khi push. Flash đang ~91%.
- **Flash**: em push code → **anh phải `git pull` trên PC** → em `curl -X POST .../api/action/upload` → agent build+flash.
  - ⚠️ Anh HAY QUÊN git pull → flash ra bản cũ. Xác nhận bằng cách bảo anh chạy `git log --oneline -1` (phải ra commit mới nhất) HOẶC nhìn log có dòng mới của bản mới.
- **Điều khiển ESP32 realtime (không reflash)**: web bấm nút → agent ghi serial → firmware `handleSerialCommand()`. Lệnh: TEST/MUSIC/SCREEN/MIC.
- **Check log**: `curl -s "https://deli2222-mira-relay.hf.space/api/log/latest?n=40"` (JSON có `state` + `entries`). `state` có: esp32/wifi/oled_ok/mic_amp/speaker_ok/face_state/emotion/last_seen.
- Sau khi sửa agent.py → **anh phải restart agent** (Ctrl+C, `python tools/agent.py`).

## 5. Trạng thái hiện tại (2026-07-13)
- 🔊 **Loa: OK** ✅ — beep lúc boot + melody, anh nghe được.
- 🎤 **Mic: CHƯA test được** ❌
- 📺 **Màn OLED: CHƯA cắm** ❌ (I2C scan không thấy) — code sẵn sàng, chờ anh cắm 4 dây.
- 📶 **WiFi: FAIL** ❌ (SSID "FPT Telecom-DF00" pass "0002FBE6", RSSI thấp/không thấy — nghi không phải do xa).
- Voice flow chưa test (cần WiFi + mic).

## 6. VẤN ĐỀ ĐANG KẸT #1 — boot dừng ở i2s_set_pin
Mọi lần boot, log dừng ở:
```
[BOOT] Nút bấm: GPIO0
[Mic] → i2s_driver_install...
[Mic] → i2s_set_pin...        ← DỪNG Ở ĐÂY, không in "set_pin =" tiếp theo
```
- Bản 16-bit ĐẦU TIÊN (trước khi thêm nhiều feature) từng QUA được (có gap ~59s rồi tới mic ready/beep/wifi). Giờ không thấy qua.
- Đã thử mic 32-bit stereo → làm treo HẲN → **revert về 16-bit ONLY_LEFT** (commit `aafb231`).
- **Chưa phân biệt được**: (a) i2s_set_pin treo THẬT, hay (b) **agent serial-READER đơ** — agent vẫn WRITE được (`→ Gửi ESP32: TEST` xuất hiện) nhưng không đọc log về, nên có thể ESP32 đang chạy trong loop mà em không thấy.

### CÂU HỎI QUYẾT ĐỊNH (chưa trả lời):
**Bấm 🎵 Phát nhạc (hoặc 🧪 Self-Test) trên web → anh có NGHE loa kêu melody 4 nốt không?**
- **CÓ nghe** → ESP32 SỐNG, đang chạy loop bình thường; lỗi chỉ ở agent hiển thị log → **restart agent** (fresh serial connection) là thấy log lại. Mọi thứ có thể đã OK.
- **KHÔNG nghe** → ESP32 treo thật ở i2s_set_pin → cần đổi cách init mic. Giả thuyết: HW I2C OLED (Wire.setPins+begin+end trong `i2cScan`) có thể để lại state làm i2s_set_pin treo. Thử: reorder `setupMic()` chạy TRƯỚC `i2cScan()` trong setup(), hoặc thêm `Serial.flush()`, hoặc bỏ Wire.end.

## 7. Việc tiếp theo (theo thứ tự)
1. **Trả lời câu hỏi mục 6** (nghe nhạc không) → quyết hướng.
2. Nếu ESP32 sống: restart agent, verify log; test mic (bấm 🎤 web → `[Mic-test] max=?`); nếu max<80 → kiểm dây mic L/R=GND, SD=GPIO34.
3. Cắm OLED → test màn (bấm 📺 web) → mặt cảm xúc hiện.
4. Fix WiFi → test voice flow đầy đủ.
5. Roadmap nâng cấp (đồng ý 2026-07-07): (1)✅ mặt cảm xúc → (3) prompt offline → (2) định danh MAC/UUID → (4) WebSocket streaming ⭐ → (5) Opus. Wake word KHÔNG khả thi trên WROOM no-PSRAM.

## 8. Bài học đã rút
- Đọc EXCCAUSE trong Guru Meditation: boot loop trước đó = "Interrupt WDT timeout" do OLED không cắm + HW I2C bus thả nổi → fix `Wire.end()` sau scan.
- Notification "exit code 0" của `pio run | tee` là exit của tee, KHÔNG phải pio — phải grep SUCCESS/FAILED trong log.
- Git pull trên PC anh hay quên → luôn verify commit hash trước khi kết luận flash bản nào.
