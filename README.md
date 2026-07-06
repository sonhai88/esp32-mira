# ESP32 × Mira — Voice Client

Thiết bị voice kết nối với Mira AI backend. Nhấn nút → nói → Mira trả lời qua loa.

## Phần cứng

| Linh kiện | Giá | Ghi chú |
|---|---|---|
| ESP32-S3 DevKit (8MB PSRAM) | ~120k | Phải có PSRAM |
| INMP441 mic module | ~40k | Mic I2S |
| MAX98357A amp module | ~40k | Amp I2S |
| Loa 3W 4Ω | ~20k | |
| Breadboard + dây jumper | ~40k | |

## Sơ đồ nối dây

```
INMP441 → ESP32-S3
VDD  → 3.3V
GND  → GND
SCK  → GPIO 41
WS   → GPIO 42
SD   → GPIO 2
L/R  → GND

MAX98357A → ESP32-S3
VIN   → 5V (hoặc 3.3V)
GND   → GND
BCLK  → GPIO 39
LRC   → GPIO 40
DIN   → GPIO 38

Loa → MAX98357A chân + và -
Nút → GPIO 0 và GND (dùng nút BOOT sẵn trên board)
```

## Cài đặt

### Arduino IDE
1. Cài [Arduino IDE 2.x](https://www.arduino.cc/en/software)
2. Thêm ESP32 board: `File → Preferences → Additional boards` → paste URL:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. `Tools → Board → ESP32S3 Dev Module`
4. `Tools → PSRAM → OPI PSRAM` (quan trọng — cần PSRAM cho audio buffer)

### Config
Mở `config.h`, điền:
- `WIFI_SSID` / `WIFI_PASSWORD`
- `DEVICE_API_KEY` (lấy từ anh Hải)

### Upload
- Cắm USB-C vào ESP32
- `Sketch → Upload`
- Mở `Tools → Serial Monitor` (115200 baud) xem log

## Flow hoạt động

```
Nhấn giữ nút BOOT → record mic → thả nút
→ gửi audio lên Mira /stt → nhận transcript
→ gửi transcript lên Mira /chat/stream → nhận reply
→ gửi reply lên Mira /tts/stream → nhận audio
→ phát ra loa
```

## Status hiện tại

- [x] WiFi connect
- [x] Ghi âm I2S (INMP441)
- [x] Build WAV header
- [x] HTTP POST /stt
- [x] HTTP POST /chat/stream (NDJSON parse)
- [x] HTTP GET /tts/stream
- [ ] Decode MP3 → PCM → phát loa (cần lib ESP32-audioI2S)

## Lib cần cài thêm

`Tools → Manage Libraries` → tìm và cài:
- **ESP32-audioI2S** by schreibfaul1 — decode MP3/AAC phát qua I2S
