# Hướng dẫn cài đặt ESP32 × Mira — Từng bước

---

## Bước 0 — Chuẩn bị phần mềm

Cài những thứ này trước (chỉ cần làm 1 lần):

| Phần mềm | Link tải | Ghi chú |
|---|---|---|
| VS Code | https://code.visualstudio.com | Editor chính |
| Python 3.10+ | https://www.python.org/downloads | Cần cho monitor tool |
| Git | https://git-scm.com | Để clone code |
| Driver USB-ESP32 | xem bên dưới | Để PC nhận ESP32 |

### Cài driver USB cho ESP32-S3

ESP32-S3 DevKit dùng chip USB-UART. Kiểm tra trên board có chữ gì:
- **CP2102** → tải tại: https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers
- **CH340** → tải tại: https://www.wch.cn/downloads/CH341SER_EXE.html
- Nếu không thấy chữ → thử cắm vào xem Device Manager có hiện không

Sau khi cài driver, cắm ESP32 vào USB → Device Manager (Windows) → Ports (COM & LPT) → phải thấy **COMx** (ví dụ COM3, COM5).

---

## Bước 1 — Clone code về

Mở Terminal (hoặc PowerShell trên Windows):

```bash
git clone https://github.com/sonhai88/esp32-mira.git
cd esp32-mira
code .
```

VS Code tự mở folder.

---

## Bước 2 — Cài PlatformIO (extension VS Code)

1. Trong VS Code, nhấn `Ctrl+Shift+X` → mở Extensions
2. Tìm **PlatformIO IDE**
3. Install → đợi ~2 phút
4. Restart VS Code khi được hỏi

Sau khi restart, thấy icon 🐜 ở thanh bên trái = cài thành công.

---

## Bước 3 — Điền thông tin kết nối

Mở file `include/config.h`, sửa 2 dòng:

```cpp
#define WIFI_SSID     "TenWifi_NhaAnh"     // ← tên WiFi nhà anh
#define WIFI_PASSWORD "MatKhauWifi"         // ← mật khẩu WiFi
#define DEVICE_API_KEY "mira-device-key-2026"  // ← giữ nguyên (hỏi anh Hải nếu đổi)
```

Các thông số khác (pin I2S) giữ nguyên nếu nối dây đúng theo sơ đồ bên dưới.

---

## Bước 4 — Nối dây phần cứng

### Mic INMP441 → ESP32-S3

```
INMP441 pin    →    ESP32-S3 pin
───────────────────────────────
VDD            →    3.3V
GND            →    GND
SCK            →    GPIO 41
WS             →    GPIO 42
SD             →    GPIO 2
L/R            →    GND          (chọn kênh trái)
```

### Loa MAX98357A → ESP32-S3

```
MAX98357A pin  →    ESP32-S3 pin
───────────────────────────────
VIN            →    5V  (hoặc 3.3V nếu không có 5V)
GND            →    GND
BCLK           →    GPIO 39
LRC            →    GPIO 40
DIN            →    GPIO 38
```

### Loa → MAX98357A

```
Loa dây đỏ (+)  →  chân OUT+ của MAX98357A
Loa dây đen (-) →  chân OUT- của MAX98357A
```

### Nút bấm

Không cần nút ngoài — dùng nút **BOOT** sẵn trên board ESP32-S3 (ghi chữ BOOT bên cạnh).

### Sơ đồ tổng quan

```
                    ┌─────────────────┐
    INMP441         │    ESP32-S3     │         MAX98357A
    ───────         │                 │         ──────────
    VDD ───────── 3.3V               5V ─────── VIN
    GND ───────── GND               GND ─────── GND
    SCK ──────── GPIO41           GPIO39 ─────── BCLK
    WS  ──────── GPIO42           GPIO40 ─────── LRC
    SD  ──────── GPIO2            GPIO38 ─────── DIN
    L/R ───────── GND                            │
                  BOOT button                  OUT+/OUT-
                  (nút sẵn)                      │
                  │                            [LOA]
                  └─────────────────┘
```

---

## Bước 5 — Build và Upload code

1. Trong VS Code, nhìn xuống thanh dưới cùng (status bar):
   - Icon ✓ = **Build** (biên dịch code)
   - Icon → = **Upload** (nạp vào ESP32)
   - Icon 🔌 = **Serial Monitor** (xem log)

2. Lần đầu nhấn ✓ Build:
   - PlatformIO tự tải thư viện `ESP32-audioI2S` và `ArduinoJson` (~2-3 phút)
   - Thấy `SUCCESS` ở cuối = thành công

3. Cắm ESP32 vào USB, nhấn → Upload:
   - Nếu fail "port not found" → xem lại bước 0 (driver)
   - Nếu fail "permission denied" (Linux/Mac) → chạy `sudo chmod a+rw /dev/ttyUSB0`

4. Mở Serial Monitor (icon 🔌) → chọn **115200 baud**

Kết quả mong đợi:
```
[Mira] Khởi động...
[Mic] I2S ready
[Speaker] OK
[WiFi] Kết nối TenWifi ............ ✓ IP: 192.168.1.x
[Mira] Sẵn sàng! Nhấn giữ nút để nói...
```

---

## Bước 6 — Mở Monitor Tool (cửa sổ quản lý)

Monitor tool cho phép xem log + trạng thái kết nối ngay trên browser, không cần Serial Monitor của VS Code.

### Cài thư viện Python (1 lần):
```bash
cd esp32-mira
pip install -r tools/requirements.txt
```

### Chạy monitor:
```bash
python tools/monitor.py
```

Browser tự mở tại `http://localhost:5555` — hiển thị:
- Trạng thái kết nối ESP32 + WiFi + Mira server
- Log realtime từ ESP32
- Câu anh nói và Mira trả lời

---

## Bước 7 — Test thử

1. Nhấn giữ nút BOOT trên ESP32
2. Nói vào mic (tiếng Việt hoặc tiếng Anh)
3. Thả nút
4. Đợi ~3-5 giây → Mira trả lời qua loa

Log mong đợi:
```
[Mic] Đang nghe...
[Mic] Ghi được 3.2 giây
[STT] Anh nói: hôm nay thời tiết thế nào
[Mira] Hà Nội hôm nay nắng, khoảng 34 độ anh ơi!
[TTS] Phát xong → sẵn sàng lại
```

---

## Troubleshooting

| Lỗi | Nguyên nhân | Fix |
|---|---|---|
| Không thấy COMx | Chưa cài driver | Làm lại Bước 0 |
| Build FAIL "PSRAM" | Chọn sai board setting | Tools → PSRAM → OPI PSRAM |
| WiFi FAIL | Sai SSID/pass | Kiểm tra config.h |
| STT trả về rỗng | Mic không hoạt động | Kiểm tra nối dây GPIO 2/41/42 |
| Không nghe tiếng | Loa không hoạt động | Kiểm tra nối dây GPIO 38/39/40 |
| HTTP 401 | Sai DEVICE_API_KEY | Hỏi anh Hải lấy key đúng |
| HTTP timeout | Mira server đang ngủ | Vào HF Space wake up, đợi 30s |
