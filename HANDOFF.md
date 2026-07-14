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

## 3. Kiến trúc — 2 máy, phân vai rõ
> **Máy dev (Mac này) giữ project + build. PC nhà chỉ cắm dây.**
> PC nhà KHÔNG có source code, KHÔNG cần git, KHÔNG cần PlatformIO.

| Thành phần | Ở đâu | Vai trò |
|---|---|---|
| Firmware | `src/main.cpp` + `face.cpp` | Chạy trên ESP32 (PlatformIO/Arduino) |
| **CLI `mira`** | `tools/mira.py` — **máy dev** | Build, đẩy `.bin` lên relay, xem log/thông số, ra lệnh |
| Agent | `tools/agent.py` — **PC nhà** | Đọc serial → push log; nhận lệnh → ghi serial / **tải `.bin` về flash bằng esptool**. Tự cập nhật từ GitHub raw |
| Relay | `relay/` (repo riêng) | HF Space `deli2222-mira-relay.hf.space` — dashboard + API + **kho firmware** |
| Server AI | `/Volumes/hai/hologram-robot/backend/main.py` ⚠️ | FastAPI "Mira" @ `deli2222-mira-ai.hf.space`. `/stt` `/chat/stream` `/tts/stream` |

Repo firmware: `sonhai88/esp32-mira` (**public** — agent tự update qua raw URL). Relay push HF: `cd relay && git push hf HEAD:main`.

## 4. Cách làm việc (QUAN TRỌNG)
Mọi thứ chạy từ máy dev qua `python3 tools/mira.py <lệnh>`:

| Lệnh | Làm gì |
|---|---|
| `mira status` | Bảng thông số: ESP32/WiFi/loa/OLED/mic amp/mặt + commit firmware trên relay |
| `mira log [n]` | n dòng log gần nhất (mặc định 40) |
| `mira watch` | Theo dõi log realtime |
| `mira flash` | **build → gộp 4 bin → đẩy relay → agent tải về flash.** Anh không làm gì |
| `mira mic` / `music` / `screen` / `test` | Lệnh realtime tới ESP32, KHÔNG reflash |
| `mira reset` | Reset ESP32 |
| `mira agent-update` | Ép agent nhà tải bản mới + restart ngay |

- **Flash an toàn**: `.bin` verify sha256 hai đầu trước khi ghi chip. Log ra `▶ Flash commit <hash>` — hết cửa flash nhầm bản cũ (không còn phụ thuộc `git pull` của anh).
- **Sửa `agent.py`**: push lên GitHub → trong 120s máy nhà tự tải + tự restart. Ép ngay: `mira agent-update`.
- **Compile-check nhanh không cần board**: `bash tools/check.sh`. Flash đang ~91%.
- **Heartbeat**: firmware in `[HB] up=..s state=.. heap=..` **mỗi 5s** → nhìn log là biết ESP32 sống hay treo, KHÔNG suy luận.
- **Cài trên PC nhà (1 lần duy nhất)**: chạy `tools\mira-setup.bat` → tự cài Python deps, tải agent về `%LOCALAPPDATA%\Mira`, tự khởi động cùng Windows, crash tự bật lại.
- ⚠️ **Đổi WiFi phải build lại** (SSID/pass nướng vào firmware qua `include/config.h`) → sửa ở máy dev rồi `mira flash`. Nút set-wifi trên web giờ chỉ báo nhắc. *Cải tiến sau: lưu WiFi vào NVS để đổi không cần reflash.*

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
1. **Anh cài 1 lần trên PC**: tải + chạy `mira-setup.bat`
   → https://raw.githubusercontent.com/sonhai88/esp32-mira/main/tools/mira-setup.bat
   Từ đó chỉ cắm dây USB, không gõ lệnh gì nữa.
2. `mira status` → thấy ESP32 ● có. `mira flash` → `mira watch`: có `[HB] up=..` chạy đều ⇒ ESP32 sống chắc chắn.
3. `mira mic` → `[Mic-test] max=?`; nếu max<80 → kiểm dây mic L/R=GND, SD=GPIO34.
4. Cắm OLED → `mira screen` → mặt cảm xúc hiện.
5. Fix WiFi (sửa `include/config.h` + `mira flash`) → test voice flow đầy đủ.
5. Roadmap nâng cấp (đồng ý 2026-07-07): (1)✅ mặt cảm xúc → (3) prompt offline → (2) định danh MAC/UUID → (4) WebSocket streaming ⭐ → (5) Opus. Wake word KHÔNG khả thi trên WROOM no-PSRAM.

## 8. Bài học đã rút
- Đọc EXCCAUSE trong Guru Meditation: boot loop trước đó = "Interrupt WDT timeout" do OLED không cắm + HW I2C bus thả nổi → fix `Wire.end()` sau scan.
- Notification "exit code 0" của `pio run | tee` là exit của tee, KHÔNG phải pio — phải grep SUCCESS/FAILED trong log.
- Git pull trên PC anh hay quên → luôn verify commit hash trước khi kết luận flash bản nào.
