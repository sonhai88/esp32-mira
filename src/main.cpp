/**
 * ESP32-Mira Voice Client v1.1
 *
 * Flow: Nhấn giữ nút BOOT → ghi âm → thả nút
 *       → POST /stt → POST /chat/stream → GET /tts/stream → phát loa
 *
 * Lib cần cài (PlatformIO tự cài qua platformio.ini):
 *   - schreibfaul1/ESP32-audioI2S
 *   - bblanchon/ArduinoJson
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Audio.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "config.h"

// ── OLED SSD1306 128x64 (I2C) ──
// HW I2C (phần cứng, ổn định trên ESP32). Chân được ép bằng Wire.setPins()
// trong i2cScan TRƯỚC khi u8g2 gọi Wire.begin() → không nhảy về GPIO22 (loa).
// NONAME = 128x64. Nếu màn 0.96" là 128x32 → đổi thành ..._128X32_...
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
bool oledOK = false;

// Vẽ text tự xuống dòng theo pixel-width (UTF-8 safe: cắt ở khoảng trắng)
void oledWrap(int x, int y, int maxW, int lineH, const String& text) {
  String line = "", word = "";
  int cy = y;
  for (size_t i = 0; i <= text.length(); i++) {
    char c = (i < text.length()) ? text[i] : ' ';
    if (c == ' ' || c == '\n') {
      String test = line.length() ? line + " " + word : word;
      if (oled.getUTF8Width(test.c_str()) > maxW && line.length()) {
        oled.drawUTF8(x, cy, line.c_str());
        cy += lineH;
        line = word;
      } else {
        line = test;
      }
      word = "";
      if (c == '\n') { oled.drawUTF8(x, cy, line.c_str()); cy += lineH; line = ""; }
    } else {
      word += c;
    }
  }
  if (line.length()) oled.drawUTF8(x, cy, line.c_str());
}

// Hiển thị 1 màn: title (đậm, trên) + body (wrap, dưới) + góc WiFi
void oledShow(const String& title, const String& body) {
  if (!oledOK) return;
  oled.clearBuffer();
  // WiFi góc phải trên
  oled.setFont(u8g2_font_5x7_tf);
  const char* w = (WiFi.status() == WL_CONNECTED) ? "wifi" : "----";
  oled.drawStr(128 - oled.getStrWidth(w), 7, w);
  // Title
  oled.setFont(u8g2_font_7x13B_tf);
  oled.drawUTF8(0, 11, title.c_str());
  oled.drawHLine(0, 15, 128);
  // Body — unifont hỗ trợ đầy đủ dấu tiếng Việt (cao ~16px)
  oled.setFont(u8g2_font_unifont_t_vietnamese2);
  oledWrap(0, 30, 128, 16, body);
  oled.sendBuffer();
}

// ── Quét I2C để CHẨN ĐOÁN: OLED có đấu đúng không + địa chỉ thật ──
// Wire.setPins() ép chân TRƯỚC Wire.begin() → u8g2 HW I2C sau này (gọi
// Wire.begin() no-arg) cũng dùng đúng 21/19, không nhảy về default 21/22.
uint8_t i2cScan() {
  Wire.setPins(OLED_SDA, OLED_SCL);
  Wire.begin();
  delay(50);
  uint8_t hit = 0;
  Serial.printf("[I2C] Quét bus SDA=%d SCL=%d ...\n", OLED_SDA, OLED_SCL);
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C]   ✓ Thấy thiết bị tại 0x%02X\n", addr);
      hit = addr;
    }
  }
  if (!hit)
    Serial.println("[I2C]   ✗ KHÔNG thấy gì — OLED chưa đấu / sai chân / thiếu nguồn "
                   "(cần VCC=3V3, GND, SDA=21, SCL=19)");
  return hit;   // 0 nếu không thấy; thường 0x3C hoặc 0x3D
}

void oledInit(uint8_t addr) {
  // HW I2C — Wire đã setPins(21,19) trong i2cScan nên begin() dùng đúng chân
  oled.setI2CAddress(addr << 1);
  oled.begin();
  oled.enableUTF8Print();
  Serial.printf("[OLED] ✓ init 0x%02X HW-I2C — SDA=%d SCL=%d\n", addr, OLED_SDA, OLED_SCL);
}

// ── Test beep khi boot để xác nhận loa hoạt động ──
// LƯU Ý: Audio lib (global object) đã install I2S_NUM_0 trong constructor.
// KHÔNG được i2s_driver_install lại → "register I2S object failed".
// Gọi HÀM NÀY SAU audio.setPinout() — dùng luôn driver của Audio lib.
void testBeep() {
  const int SR = 16000, FREQ = 880, MS = 400;
  const int N  = SR * MS / 1000;
  // Set clock cho tone (Audio lib sẽ tự set lại khi phát TTS sau)
  i2s_set_clk(I2S_NUM_0, SR, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  for (int i = 0; i < N; i++) {
    int16_t s = (int16_t)(8000 * sinf(2 * M_PI * FREQ * i / SR));
    int16_t buf[2] = {s, s};   // stereo — MAX98357A lấy 1 kênh
    size_t w; i2s_write(I2S_NUM_0, buf, sizeof(buf), &w, portMAX_DELAY);
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("[BOOT] Beep OK — loa hoạt động");
}

// ── Forward declarations (bắt buộc với PlatformIO C++) ──
void connectWiFi();
void ensureWiFi();
bool warmupMira();
void setupMic();
void micSelfTest();
void recordAudio();
String callSTT();
String callChat(const String& message);
void startTTS(const String& reply);
void buildWAVHeader(uint8_t* buf, size_t pcmLen);
String urlEncode(const String& s);
void logMem(const char* tag);
void IRAM_ATTR onBtnChange();

// ── Audio player (TTS) — dùng I2S_NUM_0 (Audio lib default) ──
Audio audio;

// ── Audio buffer ghi âm — PSRAM, tránh ăn SRAM ──
int16_t* audioBuf      = nullptr;
size_t   audioBufSamples = 0;

// ── State machine ──
enum State { IDLE, RECORDING, PROCESSING, SPEAKING };
volatile State state         = IDLE;
volatile bool  btnDown       = false;
volatile unsigned long lastBtnMs    = 0;   // volatile: ISR ghi, loop đọc
unsigned long          speakingStartMs = 0; // watchdog để unstick SPEAKING state

// ────────────────────────────────────────────
//  SETUP
// ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n========================================");
  Serial.println("  Mira Voice Client v1.1");
  Serial.println("========================================");

  // ── OLED: quét I2C chẩn đoán TRƯỚC (đo thật, không đoán), rồi init ──
  uint8_t oledAddr = i2cScan();
  oledOK = (oledAddr != 0);
  if (oledOK) {
    oledInit(oledAddr);
    oledShow("Mira", "Dang khoi dong...");
  } else {
    Serial.println("[OLED] Bỏ qua hiển thị — device vẫn chạy bình thường");
  }

  // ── Cấp phát audio buffer (PSRAM nếu có, fallback heap) ──
  // Không có PSRAM: giới hạn 3s để vừa heap sau khi WiFi init (~150KB free)
  size_t bufSize = psramFound() ? AUDIO_BUF_SIZE : (size_t)(SAMPLE_RATE * 3 * sizeof(int16_t));
  audioBuf = psramFound()
    ? (int16_t*)ps_malloc(bufSize)
    : (int16_t*)malloc(bufSize);
  if (!audioBuf) {
    Serial.printf("[ERROR] Không cấp phát được %u KB cho audio buffer!\n", bufSize / 1024);
    while (true) delay(1000);
  }
  Serial.printf("[BOOT] Audio buffer: %u KB OK (%s)\n",
    bufSize / 1024, psramFound() ? "PSRAM" : "heap");

  // ── Nút bấm (GPIO0 = BOOT button) ──
  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BTN_PIN), onBtnChange, CHANGE);
  Serial.printf("[BOOT] Nút bấm: GPIO%d\n", BTN_PIN);

  // ── Mic I2S (INMP441) — dùng I2S_NUM_1 ──
  // NOTE: Audio lib chiếm I2S_NUM_0, mic PHẢI dùng I2S_NUM_1
  setupMic();
  micSelfTest();   // đo biên độ tín hiệu mic — không cần nhấn nút

  // ── Speaker (MAX98357A) — dùng Audio lib trên I2S_NUM_0 ──
  // setPinout() TRƯỚC — gán chân cho driver mà Audio constructor đã install.
  // testBeep() SAU — dùng chung driver đó, KHÔNG install lại.
  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
  audio.setVolume(SPK_VOLUME);
  Serial.printf("[BOOT] Speaker: GPIO BCLK=%d LRC=%d DIN=%d Vol=%d\n",
                SPK_BCLK, SPK_LRC, SPK_DIN, SPK_VOLUME);
  testBeep();

  // ── WiFi ──
  connectWiFi();

  // ── Warmup Mira server (HF Space hay bị ngủ) ──
  if (WiFi.status() == WL_CONNECTED) {
    warmupMira();
  }

  logMem("BOOT xong");
  Serial.println("[Mira] ✓ Sẵn sàng! Nhấn giữ nút BOOT để nói...");
  Serial.println("========================================\n");
  oledShow("San sang", "Nhan giu nut de noi");
}

// ────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────
void loop() {
  // Khi đang phát TTS → gọi audio.loop() liên tục
  if (state == SPEAKING) {
    audio.loop();
    // Watchdog: nếu stuck SPEAKING > 30s (audio callback không fire) → force IDLE
    if (millis() - speakingStartMs > 30000) {
      Serial.println("[TTS] ⚠ Watchdog 30s → force IDLE");
      state = IDLE;
    }
    return;
  }

  // Kiểm tra WiFi định kỳ (mỗi 30 giây)
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    ensureWiFi();
    lastWifiCheck = millis();
  }

  // Phát hiện nút nhấn + debounce 50ms
  if (btnDown && state == IDLE && (millis() - lastBtnMs > 50)) {
    btnDown = false;
    state = RECORDING;

    Serial.println("\n[Mic] ▶ Bắt đầu ghi âm — thả nút khi nói xong");
    oledShow("Dang nghe", "Noi di, tha nut khi xong");
    recordAudio();

    if (audioBufSamples < 1600) {  // < 0.1 giây = quá ngắn
      Serial.printf("[Mic] Ghi được %u ms — quá ngắn, bỏ qua\n",
                    (unsigned)(audioBufSamples * 1000 / SAMPLE_RATE));
      oledShow("San sang", "Ghi qua ngan, thu lai");
      state = IDLE;
      return;
    }
    Serial.printf("[Mic] ■ Ghi xong: %.2f giây (%u samples)\n",
                  (float)audioBufSamples / SAMPLE_RATE, audioBufSamples);
    logMem("Sau ghi âm");

    state = PROCESSING;

    // ── Bước 1: STT ──
    Serial.println("[STT] → Gửi lên Whisper...");
    oledShow("Dang xu ly", "Nhan dang giong noi...");
    String transcript = callSTT();
    if (transcript.isEmpty()) {
      Serial.println("[STT] ✗ Không nhận được transcript");
      oledShow("San sang", "Khong nghe ro, thu lai");
      state = IDLE;
      return;
    }
    Serial.println("[STT] ✓ Anh nói: " + transcript);
    oledShow("Ban noi", transcript);

    // ── Bước 2: Chat ──
    Serial.println("[Chat] → Gửi lên Mira...");
    String reply = callChat(transcript);
    if (reply.isEmpty()) {
      Serial.println("[Chat] ✗ Không nhận được reply");
      oledShow("San sang", "Mira chua tra loi duoc");
      state = IDLE;
      return;
    }
    Serial.println("[Mira] ✓ " + reply);
    oledShow("Mira", reply);

    // ── Bước 3: TTS → phát loa ──
    startTTS(reply);
  }

  delay(10);
}

// ── Callbacks từ ESP32-audioI2S ──
void audio_eof_mp3(const char* info) {
  Serial.printf("[TTS] ✓ Phát xong (%s) → sẵn sàng\n", info ? info : "");
  logMem("Sau TTS");
  state = IDLE;
  oledShow("San sang", "Nhan giu nut de noi");
}
// audio_eof_stream không tồn tại trong ESP32-audioI2S v2 → đã xóa
// Watchdog 30s trong loop() sẽ unstick nếu content-type không phải mp3
void audio_error_mp3(const char* info) {
  Serial.printf("[TTS] ✗ Lỗi MP3: %s\n", info ? info : "");
  state = IDLE;
}
void audio_info(const char* info) {
  // Log thông tin stream từ Audio lib (bitrate, codec, etc.)
  if (info) Serial.printf("[Audio] %s\n", info);
}

// ── ISR nút bấm ──
void IRAM_ATTR onBtnChange() {
  bool pressed = (digitalRead(BTN_PIN) == LOW);
  if (pressed && state == IDLE) {
    btnDown   = true;
    lastBtnMs = millis();
  }
  // Thả nút: recordAudio() tự detect qua digitalRead trong vòng lặp
}

// ────────────────────────────────────────────
//  WIFI
// ────────────────────────────────────────────
void connectWiFi() {
  Serial.printf("[WiFi] Kết nối SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] ✓ Kết nối OK — IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[WiFi] ✗ FAIL — kiểm tra SSID/password trong config.h");
    Serial.println("       Device vẫn chạy nhưng HTTP sẽ fail cho đến khi có WiFi");
  }
}

void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("[WiFi] Mất kết nối (RSSI: %d) — đang reconnect...\n", WiFi.RSSI());
    WiFi.reconnect();
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 20) {
      delay(500); tries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] ✓ Reconnect OK — IP: %s\n",
                    WiFi.localIP().toString().c_str());
    } else {
      Serial.println("[WiFi] ✗ Reconnect thất bại");
    }
  }
}

// ── Warmup ping đến Mira (HF Space hay cold start 30-60s) ──
bool warmupMira() {
  Serial.print("[Mira] Ping server (HF Space có thể cold start 30-60s)");
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(MIRA_BASE_URL) + "/healthz");
  http.setTimeout(30000);

  unsigned long t0 = millis();
  int code = http.GET();
  unsigned long elapsed = millis() - t0;
  http.end();

  if (code == 200) {
    Serial.printf(" ✓ OK (%.1f giây)\n", elapsed / 1000.0);
    return true;
  } else {
    Serial.printf(" ✗ HTTP %d (%.1f giây) — tiếp tục nhưng request đầu có thể chậm\n",
                  code, elapsed / 1000.0);
    return false;
  }
}

// ────────────────────────────────────────────
//  I2S MIC (INMP441) — I2S_NUM_1
//  NOTE: Audio lib dùng I2S_NUM_0, mic PHẢI dùng I2S_NUM_1
// ────────────────────────────────────────────
void setupMic() {
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
  };
  i2s_pin_config_t pins = {
    .bck_io_num   = MIC_SCK,
    .ws_io_num    = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD,
  };

  esp_err_t err = i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[Mic] ✗ i2s_driver_install lỗi: %s\n", esp_err_to_name(err));
    Serial.println("       → Kiểm tra pin config, có thể xung đột GPIO");
    return;
  }
  err = i2s_set_pin(I2S_NUM_1, &pins);
  if (err != ESP_OK) {
    Serial.printf("[Mic] ✗ i2s_set_pin lỗi: %s\n", esp_err_to_name(err));
    return;
  }
  Serial.printf("[Mic] ✓ I2S_NUM_1 ready — SCK=%d WS=%d SD=%d\n",
                MIC_SCK, MIC_WS, MIC_SD);
}

// ── Đo biên độ tín hiệu mic (không cần nhấn nút) — chẩn đoán mic sống/chết ──
// INMP441 kể cả im lặng vẫn có nhiễu nền vài trăm. Nếu max ~0 → mic không có
// tín hiệu (thường do chân L/R sai kênh, SD chưa nối, hoặc thiếu nguồn/GND).
void micSelfTest() {
  const int TARGET = 4000;
  int16_t tmp[256];
  int32_t maxAbs = 0;
  int64_t sumAbs = 0;
  int cnt = 0;
  unsigned long t0 = millis();
  while (cnt < TARGET && millis() - t0 < 800) {
    size_t br = 0;
    if (i2s_read(I2S_NUM_1, tmp, sizeof(tmp), &br, pdMS_TO_TICKS(50)) != ESP_OK) continue;
    int n = br / sizeof(int16_t);
    for (int i = 0; i < n; i++) {
      int a = tmp[i] < 0 ? -tmp[i] : tmp[i];
      if (a > maxAbs) maxAbs = a;
      sumAbs += a;
      cnt++;
    }
  }
  int avg = cnt ? (int)(sumAbs / cnt) : 0;
  Serial.printf("[Mic-test] Đọc %d mẫu: biên độ max=%d, trung bình=%d\n",
                cnt, (int)maxAbs, avg);
  if (cnt == 0) {
    Serial.println("[Mic-test] ✗ Đọc 0 mẫu — driver mic lỗi (kiểm tra SCK=14 WS=15)");
  } else if (maxAbs < 80) {
    Serial.println("[Mic-test] ✗ Mic gần như IM — không có tín hiệu.");
    Serial.println("           Kiểm tra: chân L/R nối GND? SD→GPIO34? VDD=3.3V? GND chung?");
  } else {
    Serial.printf("[Mic-test] ✓ Mic CÓ tín hiệu (max=%d) — mic sống\n", (int)maxAbs);
  }
}

void recordAudio() {
  audioBufSamples = 0;
  size_t maxSamples = AUDIO_BUF_SIZE / sizeof(int16_t);
  unsigned long start = millis();
  unsigned long deadline = start + (RECORD_SECONDS * 1000UL);

  while (digitalRead(BTN_PIN) == LOW
         && millis() < deadline
         && audioBufSamples < maxSamples) {
    int16_t tmp[256];
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_1, tmp, sizeof(tmp), &bytesRead, pdMS_TO_TICKS(50));
    if (err != ESP_OK || bytesRead == 0) continue;
    size_t n = bytesRead / sizeof(int16_t);
    memcpy(audioBuf + audioBufSamples, tmp, bytesRead);
    audioBufSamples += n;
  }
}

// ────────────────────────────────────────────
//  HTTP /stt → transcript
// ────────────────────────────────────────────
String callSTT() {
  size_t pcmLen = audioBufSamples * sizeof(int16_t);
  size_t wavLen = 44 + pcmLen;

  // Alloc WAV từ PSRAM
  uint8_t* wav = (uint8_t*)ps_malloc(wavLen);
  if (!wav) {
    Serial.printf("[STT] ✗ ps_malloc WAV thất bại (%u bytes)\n", wavLen);
    return "";
  }
  buildWAVHeader(wav, pcmLen);
  memcpy(wav + 44, audioBuf, pcmLen);

  // Build multipart
  const char* boundary = "MiraBoundary2026";
  char headBuf[256];
  snprintf(headBuf, sizeof(headBuf),
    "--%s\r\n"
    "Content-Disposition: form-data; name=\"audio\"; filename=\"rec.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n", boundary);
  char tailBuf[64];
  snprintf(tailBuf, sizeof(tailBuf), "\r\n--%s--\r\n", boundary);

  size_t headLen = strlen(headBuf);
  size_t tailLen = strlen(tailBuf);
  size_t bodyLen = headLen + wavLen + tailLen;

  uint8_t* body = (uint8_t*)ps_malloc(bodyLen);
  if (!body) {
    Serial.printf("[STT] ✗ ps_malloc body thất bại (%u bytes)\n", bodyLen);
    free(wav);
    return "";
  }
  memcpy(body,              headBuf, headLen);
  memcpy(body + headLen,    wav,     wavLen);
  memcpy(body + headLen + wavLen, tailBuf, tailLen);
  free(wav);

  Serial.printf("[STT] Gửi %u bytes (WAV %.1f KB)...\n",
                bodyLen, wavLen / 1024.0);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(MIRA_BASE_URL) + "/stt");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  char ctBuf[64];
  snprintf(ctBuf, sizeof(ctBuf), "multipart/form-data; boundary=%s", boundary);
  http.addHeader("Content-Type", ctBuf);
  http.setTimeout(20000);

  unsigned long t0 = millis();
  int code = http.POST(body, bodyLen);
  free(body);
  unsigned long elapsed = millis() - t0;

  if (code != 200) {
    String err = http.getString();
    Serial.printf("[STT] ✗ HTTP %d (%.1fs): %s\n", code, elapsed / 1000.0, err.c_str());
    http.end();
    return "";
  }

  String resp = http.getString();
  http.end();
  Serial.printf("[STT] ✓ OK (%.1fs): %s\n", elapsed / 1000.0, resp.c_str());

  JsonDocument doc;
  auto jerr = deserializeJson(doc, resp);
  if (jerr != DeserializationError::Ok) {
    Serial.printf("[STT] ✗ JSON parse lỗi: %s\n", jerr.c_str());
    return "";
  }
  return doc["transcript"].as<String>();
}

// ────────────────────────────────────────────
//  HTTP /chat/stream → reply
// ────────────────────────────────────────────
String callChat(const String& message) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, String(MIRA_BASE_URL) + "/chat/stream");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  http.setTimeout(25000);

  // Escape JSON — thứ tự quan trọng: \\ phải đầu tiên
  String msg = message;
  msg.replace("\\", "\\\\");
  msg.replace("\"", "\\\"");
  msg.replace("\n", "\\n");
  msg.replace("\r", "\\r");
  msg.replace("\t", "\\t");
  String postBody = "{\"message\":\"" + msg + "\"}";

  Serial.printf("[Chat] POST body: %s\n", postBody.c_str());
  unsigned long t0 = millis();
  int code = http.POST(postBody);

  if (code != 200) {
    Serial.printf("[Chat] ✗ HTTP %d: %s\n", code, http.getString().c_str());
    http.end();
    return "";
  }

  // Đọc NDJSON — char buffer cố định, tránh String heap fragmentation
  // 3072: đủ cho done-line với reply 400+ ký tự (JSON overhead ~150 bytes)
  static char lineBuf[3072];
  int  linePos     = 0;
  bool lineInvalid = false;  // tràn buffer → skip parse khi gặp \n
  String reply = "";
  WiFiClient* stream = http.getStreamPtr();
  unsigned long deadline = millis() + 20000;

  while ((http.connected() || stream->available()) && millis() < deadline) {
    if (!stream->available()) { delay(2); continue; }

    char c = (char)stream->read();
    if (c == '\n') {
      if (linePos > 0 && !lineInvalid) {
        lineBuf[linePos] = '\0';
        // Chỉ parse dòng có "done":true — bỏ qua delta nhỏ
        if (strstr(lineBuf, "\"done\":true")) {
          JsonDocument doc;
          auto jerr = deserializeJson(doc, lineBuf);
          if (jerr == DeserializationError::Ok) {
            reply = doc["reply"].as<String>();
            Serial.printf("[Chat] ✓ Nhận reply (%.1fs)\n",
                          (millis() - t0) / 1000.0);
          } else {
            Serial.printf("[Chat] ✗ JSON parse lỗi: %s\n", jerr.c_str());
          }
          break;
        }
      }
      linePos     = 0;
      lineInvalid = false;
    } else {
      if (linePos < (int)sizeof(lineBuf) - 1) {
        lineBuf[linePos++] = c;
      } else {
        // Buffer tràn 3072 bytes → đánh dấu invalid, không parse dòng này
        Serial.println("[Chat] ⚠ Dòng NDJSON vượt 3072 bytes — bỏ qua");
        lineInvalid = true;
      }
    }
  }

  if (reply.isEmpty() && millis() >= deadline) {
    Serial.println("[Chat] ✗ Timeout sau 20 giây");
  }

  http.end();
  return reply;
}

// ────────────────────────────────────────────
//  TTS → phát loa
// ────────────────────────────────────────────
void startTTS(const String& reply) {
  // Giới hạn 80 ký tự raw: tiếng Việt encode 3 byte/char → %XX%XX%XX = 9 chars/char
  // 80 * 9 = 720 encoded + ~53 base URL = ~773 tổng cộng → an toàn (nginx limit 8KB)
  String text = reply;
  if (text.length() > 80) {
    text = text.substring(0, 80);
    Serial.printf("[TTS] ⚠ Reply quá dài, cắt bớt còn 80 ký tự\n");
  }

  String url = String(MIRA_BASE_URL) + "/tts/stream?text=" + urlEncode(text);
  Serial.printf("[TTS] → connecttohost (URL length: %u)\n", url.length());
  speakingStartMs = millis();
  state = SPEAKING;
  audio.connecttohost(url.c_str());
}

// ────────────────────────────────────────────
//  UTILS
// ────────────────────────────────────────────
void buildWAVHeader(uint8_t* buf, size_t pcmLen) {
  const uint32_t sr         = SAMPLE_RATE;
  const uint16_t channels   = 1;
  const uint16_t bps        = 16;
  const uint32_t byteRate   = sr * channels * bps / 8;
  const uint16_t blockAlign = channels * bps / 8;

  memcpy(buf,    "RIFF", 4);  *(uint32_t*)(buf+4)  = 36 + pcmLen;
  memcpy(buf+8,  "WAVE", 4);
  memcpy(buf+12, "fmt ", 4);  *(uint32_t*)(buf+16) = 16;
  *(uint16_t*)(buf+20) = 1;            // PCM
  *(uint16_t*)(buf+22) = channels;
  *(uint32_t*)(buf+24) = sr;
  *(uint32_t*)(buf+28) = byteRate;
  *(uint16_t*)(buf+32) = blockAlign;
  *(uint16_t*)(buf+34) = bps;
  memcpy(buf+36, "data", 4);  *(uint32_t*)(buf+40) = pcmLen;
}

String urlEncode(const String& s) {
  String out;
  out.reserve(s.length() * 3);
  for (unsigned int i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isAlphaNumeric(c) || c=='-' || c=='_' || c=='.' || c=='~') {
      out += (char)c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

void logMem(const char* tag) {
  Serial.printf("[MEM] %s → Heap: %u KB free / %u KB min | PSRAM: %u KB free\n",
                tag,
                ESP.getFreeHeap() / 1024,
                ESP.getMinFreeHeap() / 1024,
                ESP.getFreePsram() / 1024);
}
