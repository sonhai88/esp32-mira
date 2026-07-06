# ESP32 × Mira — Voice Device

Thiết bị voice tự chế kết nối với [Mira AI](https://deli2222-mira-ai.hf.space). Nhấn giữ nút → nói → Mira trả lời qua loa. Không cần điện thoại, không cần màn hình. Chi phí phần cứng ~260k.

```
[Người nói] ──mic──► ESP32-S3 ──WiFi──► Mira Server ──WiFi──► ESP32-S3 ──loa──► [Mira trả lời]
```

---

## Phần cứng

| Linh kiện | Vai trò | Giá | Mua ở đâu |
|---|---|---|---|
| **ESP32-S3 DevKit** (8MB PSRAM) | Não trung tâm + WiFi | ~120k | Shopee "ESP32-S3-DevKitC-1" |
| **INMP441** | Mic I2S kỹ thuật số | ~40k | Shopee "INMP441 module" |
| **MAX98357A** | Khuếch đại âm thanh I2S | ~40k | Shopee "MAX98357A module" |
| Loa 3W 4Ω | Phát tiếng | ~20k | Shopee "loa 3W 4ohm" |
| Breadboard 830 lỗ + dây jumper | Cắm thử, không hàn | ~40k | Shopee |

> **Quan trọng:** ESP32-S3 phải có **8MB PSRAM** — dùng để lưu audio buffer khi ghi âm. Loại không có PSRAM sẽ báo lỗi ngay lúc khởi động.

---

## Sơ đồ nối dây

```
                    ┌─────────────────────┐
   INMP441          │      ESP32-S3       │        MAX98357A
   ───────          │                     │        ─────────
   VDD  ──────────  3.3V             5V  ──────── VIN
   GND  ──────────  GND             GND  ──────── GND
   SCK  ──────── GPIO 41         GPIO 39 ──────── BCLK
   WS   ──────── GPIO 42         GPIO 40 ──────── LRC
   SD   ──────── GPIO  2         GPIO 38 ──────── DIN
   L/R  ──────── GND                              │
                    │                           OUT+ OUT-
                 [BOOT btn]                       │
                 GPIO 0                         [LOA]
                    └─────────────────────┘
```

**Nút bấm:** Dùng nút **BOOT** có sẵn trên board (ghi chữ BOOT), không cần mua thêm.

---

## Cài đặt

### Bước 1 — Clone

```bash
git clone https://github.com/sonhai88/esp32-mira.git
cd esp32-mira
```

### Bước 2 — Cài PlatformIO trong VS Code

1. Mở VS Code → Extensions (`Ctrl+Shift+X`)
2. Tìm **PlatformIO IDE** → Install → Restart VS Code
3. Mở folder `esp32-mira` trong VS Code

### Bước 3 — Điền thông tin kết nối

Mở `include/config.h`:

```cpp
#define WIFI_SSID      "TenWifi_NhaAnh"
#define WIFI_PASSWORD  "MatKhauWifi"
#define DEVICE_API_KEY "mira-device-key-2026"   // hỏi admin lấy key
```

### Bước 4 — Build & Upload

Cắm ESP32 vào USB, sau đó dùng một trong hai cách:

**Cách A — Dùng Dev Tool (khuyến nghị):**
```bash
pip install -r tools/requirements.txt
python tools/monitor.py
```
Nhấn nút **Build & Upload** trong browser.

**Cách B — Dùng VS Code:**
Click icon **→** ở thanh dưới cùng VS Code (lần đầu tự tải lib ~3 phút).

---

## Dev Tool

Tool quản lý chạy tại `http://localhost:5555` — mở browser tự động khi khởi động.

```
python tools/monitor.py
```

**Tính năng:**

| Tính năng | Mô tả |
|---|---|
| Build & Upload | Nạp firmware vào ESP32, stream log build realtime |
| Reset ESP32 | Reset board không cần rút cắm USB |
| Ping Mira | Kiểm tra server còn sống không |
| Export Log | Download toàn bộ log ra file `.txt` |
| Auto-diagnose | Tự phát hiện lỗi từ log, hiện gợi ý fix ngay trong UI |
| Serial log | Màu sắc theo loại: đỏ=lỗi, xanh lá=anh nói, xanh dương=Mira reply |

**API cho Claude đọc log trực tiếp:**

```
GET http://localhost:5555/api/log/latest    # 100 log entries gần nhất
GET http://localhost:5555/api/diagnose      # issues hiện tại + fix hints
GET http://localhost:5555/api/log/export    # download full log
```

---

## Cách hoạt động

```
1. Nhấn giữ nút BOOT
2. INMP441 thu âm → lưu vào PSRAM (tối đa 5 giây)
3. Thả nút
4. POST audio WAV → Mira /stt  → nhận transcript (Whisper)
5. POST transcript → Mira /chat/stream → nhận reply (stream NDJSON)
6. GET reply text  → Mira /tts/stream  → nhận audio MP3
7. Phát MP3 qua MAX98357A → loa
8. Sẵn sàng lại
```

**Latency thực tế:** ~3-5 giây từ lúc thả nút đến khi nghe tiếng Mira.

---

## Troubleshooting

| Lỗi trong log | Nguyên nhân | Fix |
|---|---|---|
| `PSRAM not found` | Board không có PSRAM hoặc sai config | `platformio.ini` → `board_build.arduino.memory_type = qio_opi` |
| `WiFi ✗ FAIL` | Sai SSID/password | Kiểm tra `config.h` dòng WIFI_SSID |
| `HTTP 401` | Sai DEVICE_API_KEY | Kiểm tra `config.h` dòng DEVICE_API_KEY |
| `HTTP 503` | Mira server đang cold start | Đợi 60s, server HF Space tự wake up |
| `i2s_driver_install lỗi` | Sai GPIO hoặc xung đột | Kiểm tra nối dây MIC_SCK/WS/SD |
| Không nghe tiếng | MAX98357A chưa hoạt động | Kiểm tra GPIO 38/39/40, tăng `SPK_VOLUME` trong config.h |
| Log im lặng sau upload | Serial Monitor chưa mở | Mở Dev Tool hoặc VS Code Serial Monitor 115200 baud |

---

## Cấu trúc project

```
esp32-mira/
├── src/
│   └── main.cpp          # Firmware chính (voice pipeline)
├── include/
│   └── config.h          # WiFi, API key, GPIO pins
├── tools/
│   ├── monitor.py        # Dev Tool (browser UI + Claude API)
│   └── requirements.txt  # Python deps: flask, pyserial
├── platformio.ini         # Build config, lib dependencies
└── SETUP.md              # Hướng dẫn chi tiết từng bước
```

---

## Stack

- **Firmware:** C++ / Arduino framework / ESP-IDF
- **AI Backend:** [Mira](https://deli2222-mira-ai.hf.space) — FastAPI + Groq Whisper (STT) + Llama 3.3 (Chat) + Edge-TTS (TTS)
- **Dev Tool:** Python 3 / Flask / pyserial
- **Build tool:** PlatformIO
