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
- **Anh KHÔNG ở cùng máy này** — anh chỉ **cắm dây USB** ở PC nhà. Agent là phần mềm chạy nền, tự lo phần còn lại. Em điều khiển 100% qua relay.
- **Compile-check không cần board**: `bash /Volumes/hai/esp32-mira/tools/check.sh` (pio ở `~/.pio-venv`). LUÔN chạy trước khi push. Flash đang ~91%.
- **Flash**: em push code → em `curl -X POST .../api/action/upload` → agent **tự `git pull`** rồi build+flash. Anh KHÔNG phải làm gì.
  - Agent log `▶ Flash commit <hash>` — đối chiếu hash đó với `git log --oneline -1` bên này là biết chắc flash đúng bản.
- **Sửa agent.py**: em push → trong 60s agent tự pull + tự restart. Ép ngay: `curl -X POST .../api/action/update-agent`.
- **Điều khiển ESP32 realtime (không reflash)**: web bấm nút → agent ghi serial → firmware `handleSerialCommand()`. Lệnh: TEST/MUSIC/SCREEN/MIC.
- **Check log**: `curl -s "https://deli2222-mira-relay.hf.space/api/log/latest?n=40"` (JSON có `state` + `entries`).
  - Firmware in `[HB] up=..s state=.. heap=..` **mỗi 5s** → nhìn log tail là biết ESP32 sống hay treo, KHÔNG suy luận.
- **Cài lần đầu trên PC (1 lần duy nhất)**: chạy `tools\install-startup.bat` → agent tự khởi động cùng Windows, crash tự bật lại.

## 5. Trạng thái hiện tại (2026-07-13)
- 🔊 **Loa: OK** ✅ — beep lúc boot + melody, anh nghe được.
- 🎤 **Mic: CHƯA test được** ❌
- 📺 **Màn OLED: CHƯA cắm** ❌ (I2C scan không thấy) — code sẵn sàng, chờ anh cắm 4 dây.
- 📶 **WiFi: FAIL** ❌ (SSID "FPT Telecom-DF00" pass "0002FBE6", RSSI thấp/không thấy — nghi không phải do xa).
- Voice flow chưa test (cần WiFi + mic).

## 6. ĐÃ GIẢI QUYẾT — "boot dừng ở i2s_set_pin" là do AGENT, không phải ESP32
Triệu chứng cũ: log luôn dừng ở `[Mic] → i2s_set_pin...`, không bao giờ in `set_pin =`.

**Kết luận (2026-07-13): ESP32 KHÔNG treo. Thread đọc serial của agent chết im lặng.**
- Bằng chứng 1: `testBeep()` nằm SAU `setupMic()` trong setup(). Anh NGHE beep lúc boot ⇒ code đã chạy qua `i2s_set_pin` rồi.
- Bằng chứng 2: không có đường nào block được ở đó — `i2s_set_pin` chỉ ghi register; `micSelfTest()` cap cứng 800ms.
- Root cause: `serial_reader()` chỉ catch `serial.SerialException`. Lúc flash, `action_upload` set `_ser = None` → race → `None.readline()` ném `AttributeError` → **thread reader chết**. Agent vẫn sống và vẫn GHI serial được (nên `→ Gửi ESP32: TEST` vẫn hiện) nhưng không đọc log về nữa. Xảy ra **sau mỗi lần flash** → đúng triệu chứng.
- Fix (commit `93fd40a`): guard `_ser is None` + `except Exception` bao ngoài → reader không thể chết. Thêm heartbeat `[HB]` mỗi 5s trong `loop()` để sống/treo là **quan sát được**, không phải suy luận.

⚠️ Bài học: đừng suy luận "firmware treo" từ log im lặng khi tầng vận chuyển log có thể chết. Tìm bằng chứng độc lập với kênh đang nghi (ở đây: tiếng beep).

## 7. Việc tiếp theo (theo thứ tự)
1. **Anh chạy `tools\install-startup.bat` 1 lần** trên PC (cài agent chạy nền). Từ đó chỉ cắm dây USB.
2. Em flash bản mới → đọc log: có `[HB] up=..` chạy đều ⇒ ESP32 sống chắc chắn.
3. Test mic (bấm 🎤 web → `[Mic-test] max=?`); nếu max<80 → kiểm dây mic L/R=GND, SD=GPIO34.
4. Cắm OLED → test màn (bấm 📺 web) → mặt cảm xúc hiện.
5. Fix WiFi → test voice flow đầy đủ.
5. Roadmap nâng cấp (đồng ý 2026-07-07): (1)✅ mặt cảm xúc → (3) prompt offline → (2) định danh MAC/UUID → (4) WebSocket streaming ⭐ → (5) Opus. Wake word KHÔNG khả thi trên WROOM no-PSRAM.

## 8. Bài học đã rút
- Đọc EXCCAUSE trong Guru Meditation: boot loop trước đó = "Interrupt WDT timeout" do OLED không cắm + HW I2C bus thả nổi → fix `Wire.end()` sau scan.
- Notification "exit code 0" của `pio run | tee` là exit của tee, KHÔNG phải pio — phải grep SUCCESS/FAILED trong log.
- Git pull trên PC anh hay quên → luôn verify commit hash trước khi kết luận flash bản nào.
