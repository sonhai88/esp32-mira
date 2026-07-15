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

## 5. Trạng thái hiện tại (2026-07-14)
- ✅ **Boot/loop: OK** — device sống, `[HB]` đập đều mỗi 5s. Cái kẹt i2s_set_pin đã fix (mục 6).
- 🔊 **Loa: OK** — `[BOOT] Beep OK`.
- 🎤 **Mic: `max=0`** ❌ — KHÔNG phải nguồn (radio khỏe, scan ra 22 mạng ⇒ nguồn đủ). → **dây INMP441**: kiểm SD→GPIO34, L/R→GND, VDD→3.3V, GND chung.
- 📺 **OLED: chưa cắm** ❌ (I2C không thấy) — code sẵn sàng, chờ cắm 4 dây.
- 📶 **WiFi: mạng nhà là 5GHz** ❌ — `FPT Telecom-DF00` KHÔNG có trong scan 2.4GHz của ESP32 (cả xóm thì hiện đủ). ESP32 chỉ bắt 2.4GHz. → cần **hotspot iPhone (bật Maximize Compatibility = 2.4GHz)** để test ngay, hoặc bật băng 2.4GHz trên modem FPT để dùng lâu dài.
- Voice flow chưa test (cần WiFi 2.4GHz + mic).

## 6. ĐÃ GIẢI QUYẾT — hai lỗi CHỒNG nhau ở "boot dừng ở i2s_set_pin"
Triệu chứng: log luôn dừng ở `[Mic] → i2s_set_pin...`, không in `set_pin =`.

Thực tế có **2 lỗi độc lập chồng lên nhau**, phải gỡ lần lượt:

**Lỗi A — agent reader chết im lặng (commit `93fd40a`).** `serial_reader()` chỉ catch `SerialException`; lúc flash `_ser=None` → `AttributeError` → thread đọc chết, agent vẫn GHI được nên tưởng ESP32 treo. Fix: guard `None` + `except Exception`, thêm `[HB]` heartbeat. → cho log SẠCH để nhìn ra lỗi B.

**Lỗi B — i2s_set_pin treo THẬT (commit `3b8df68`).** `mck_io_num` là field ĐẦU của `i2s_pin_config_t`, code bỏ trống = `0` = **GPIO0**. GPIO0 vừa `attachInterrupt` (nút BOOT) ngay trên → `i2s_set_pin` kẹt khi route MCLK ra chân đang giữ interrupt. Fix: khai báo `.mck_io_num = I2S_PIN_NO_CHANGE` (INMP441 không cần MCLK). Sau fix, log qua `set_pin = ESP_OK` và `[HB]` chạy.

⚠️ **Bài học (2 cái):**
1. Đừng suy luận "sống/treo" từ log im lặng khi tầng vận chuyển log có thể chết — sửa kênh quan sát TRƯỚC, rồi mới đọc bằng chứng.
2. Em từng kết luận "ESP32 không treo, anh nghe beep là bằng chứng qua i2s_set_pin" — **SAI**. Beep đó từ bản firmware cũ hơn (thứ tự setup khác). Bằng chứng gián tiếp ("nghe beep") không đủ; chỉ log sạch mới cho sự thật. Cũng như giả thuyết "nguồn yếu" cho WiFi 0-mạng — SAI, là bug scan. **Đo thật > đoán, mỗi lần.**

## 7. Việc tiếp theo (theo thứ tự)
1. **WiFi 2.4GHz** (blocker chính): anh cấp hotspot iPhone (Maximize Compatibility ON) hoặc bật 2.4GHz modem FPT → em sửa `include/config.h` (máy dev) + `mira flash` → log phải ra `[WiFi] ✓ IP: ...`.
2. **Mic**: kiểm dây INMP441 (SD→34, L/R→GND, VDD→3V3) → `mira flash` lại → `[Mic-test] max=` phải ≥80.
3. Có WiFi + mic → **test voice flow**: giữ nút BOOT nói → STT → LLM → TTS phát loa.
4. Cắm OLED → `mira screen` → mặt cảm xúc hiện.
5. Roadmap: lưu WiFi vào NVS (đổi mạng không cần reflash) → WebSocket streaming → Opus.
5. Roadmap nâng cấp (đồng ý 2026-07-07): (1)✅ mặt cảm xúc → (3) prompt offline → (2) định danh MAC/UUID → (4) WebSocket streaming ⭐ → (5) Opus. Wake word KHÔNG khả thi trên WROOM no-PSRAM.

## 8. Bài học đã rút
- Đọc EXCCAUSE trong Guru Meditation: boot loop trước đó = "Interrupt WDT timeout" do OLED không cắm + HW I2C bus thả nổi → fix `Wire.end()` sau scan.
- Notification "exit code 0" của `pio run | tee` là exit của tee, KHÔNG phải pio — phải grep SUCCESS/FAILED trong log.
- Git pull trên PC anh hay quên → luôn verify commit hash trước khi kết luận flash bản nào.
