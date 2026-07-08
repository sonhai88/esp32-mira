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
#include "face.h"

// AUDIO_BUF_SIZE define trong code (KHÔNG qua build_flags) — ngoặc ( ) trong
// -D macro bị shell macOS/Linux hiểu nhầm là cú pháp shell → build fail.
// SAMPLE_RATE, RECORD_SECONDS là số đơn từ build_flags nên truyền shell OK.
#ifndef AUDIO_BUF_SIZE
#define AUDIO_BUF_SIZE (SAMPLE_RATE * RECORD_SECONDS * 2)
#endif

// ── OLED SSD1306 128x64 (I2C) ──
// HW I2C (phần cứng, ổn định trên ESP32). Chân được ép bằng Wire.setPins()
// trong i2cScan TRƯỚC khi u8g2 gọi Wire.begin() → không nhảy về GPIO22 (loa).
// NONAME = 128x64. Nếu màn 0.96" là 128x32 → đổi thành ..._128X32_...
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
bool oledOK = false;

// Khi hiện text (transcript/lỗi), giữ màn tới mốc này trước khi animation mặt
// idle vẽ đè — để anh kịp đọc.
unsigned long g_holdUntil = 0;

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
  g_holdUntil = millis() + 2500;   // giữ text 2.5s trước khi mặt idle vẽ đè
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
  // Gỡ I2C driver + ISR ngay sau scan. Nếu OLED KHÔNG cắm, bus thả nổi (không
  // pullup) sinh nhiễu → I2C ISR bắn liên tục → Interrupt WDT timeout → boot
  // loop. oledInit() sẽ Wire.begin() lại (qua u8g2) nếu tìm thấy OLED.
  Wire.end();
  return hit;   // 0 nếu không thấy; thường 0x3C hoặc 0x3D
}

void oledInit(uint8_t addr) {
  // setPins lại vì i2cScan đã Wire.end(). u8g2 oled.begin() gọi Wire.begin()
  // no-arg → cần chân đã set trước.
  Wire.setPins(OLED_SDA, OLED_SCL);
  oled.setI2CAddress(addr << 1);
  oled.begin();
  oled.enableUTF8Print();
  Serial.printf("[OLED] ✓ init 0x%02X HW-I2C — SDA=%d SCL=%d\n", addr, OLED_SDA, OLED_SCL);
}

// ── Mặt cảm xúc ──
// g_emotion cập nhật từ /chat response; g_faceState để loop() chạy animation.
// volatile: g_emotion/g_faceState được ghi từ audio_eof_mp3 (callback fire
// trong audio.loop()) và đọc/ghi ở loop() — volatile phòng khi lib audio đổi
// sang task riêng trong tương lai.
volatile FaceEmotion g_emotion   = EMO_NEUTRAL;
volatile FaceState   g_faceState = FACE_IDLE;

void faceShow(FaceState st) {
  // Log CHỈ khi đổi state (animation gọi hàm này liên tục — tránh spam log).
  // Đặt trước check oledOK để vẫn track state kể cả khi không có màn.
  if (st != g_faceState) {
    static const char* NM[] = {"IDLE", "LISTENING", "THINKING", "SPEAKING"};
    static const char* EM[] = {"neutral", "happy", "sad", "curious", "playful", "caring"};
    Serial.printf("[Face] → %s (emotion: %s)\n", NM[st], EM[g_emotion]);
    g_faceState = st;
  }
  if (!oledOK) return;
  faceDraw(oled, st, g_emotion, millis());
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
int16_t* audioBuf        = nullptr;
size_t   audioBufSamples = 0;
size_t   audioBufCap     = 0;   // sức chứa THỰC (số mẫu) — dùng để chặn tràn
int      g_micChannel    = 0;   // kênh mic có tín hiệu (0=Left,1=Right) — micSelfTest tự dò
// INMP441 xuất 24-bit trong khung 32-bit → đọc 32-bit rồi dịch >>14 ra 16-bit
#define MIC_SHIFT 14

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
  audioBufCap = bufSize / sizeof(int16_t);   // sức chứa THỰC — recordAudio bound theo cái này
  Serial.printf("[BOOT] Audio buffer: %u KB OK (%s) — %u mẫu\n",
    bufSize / 1024, psramFound() ? "PSRAM" : "heap", (unsigned)audioBufCap);

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
  faceShow(FACE_IDLE);
}

// ────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────
void loop() {
  // Khi đang phát TTS → gọi audio.loop() liên tục
  if (state == SPEAKING) {
    audio.loop();
    // Mặt nói: miệng nhấp nháy theo nhịp (~8fps đủ mượt, không cản audio)
    static unsigned long lastFaceSpeak = 0;
    if (millis() - lastFaceSpeak > 120) {
      lastFaceSpeak = millis();
      faceShow(FACE_SPEAKING);
    }
    // Watchdog: nếu stuck SPEAKING > 30s (audio callback không fire) → force IDLE
    if (millis() - speakingStartMs > 30000) {
      Serial.println("[TTS] ⚠ Watchdog 30s → force IDLE");
      state = IDLE;
      g_emotion = EMO_NEUTRAL;
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
    faceShow(FACE_LISTENING);
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
    faceShow(FACE_THINKING);
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
    faceShow(FACE_SPEAKING);   // mặt nói + emotion vừa parse từ /chat

    // ── Bước 3: TTS → phát loa ──
    startTTS(reply);
  }

  // Animation mặt idle (chớp mắt) khi rảnh — không đè text đang được giữ
  static unsigned long lastFaceIdle = 0;
  if (state == IDLE && (long)(millis() - g_holdUntil) > 0 && millis() - lastFaceIdle > 150) {
    lastFaceIdle = millis();
    faceShow(FACE_IDLE);
  }

  delay(10);
}

// ── Callbacks từ ESP32-audioI2S ──
void audio_eof_mp3(const char* info) {
  Serial.printf("[TTS] ✓ Phát xong (%s) → sẵn sàng\n", info ? info : "");
  logMem("Sau TTS");
  state = IDLE;
  g_emotion = EMO_NEUTRAL;   // reset về trung tính khi xong
  faceShow(FACE_IDLE);
}
// audio_eof_stream không tồn tại trong ESP32-audioI2S v2 → đã xóa
// Watchdog 30s trong loop() sẽ unstick nếu content-type không phải mp3
void audio_error_mp3(const char* info) {
  Serial.printf("[TTS] ✗ Lỗi MP3: %s\n", info ? info : "");
  state = IDLE;
  g_emotion = EMO_NEUTRAL;   // reset kẻo mặt kẹt emotion cũ (loop sẽ vẽ IDLE)
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
  // 32-bit STEREO: INMP441 xuất 24-bit trong khung 32-bit (đọc 16-bit ra 0/rác).
  // Đọc cả 2 kênh để micSelfTest tự dò kênh nào có tín hiệu (không phụ thuộc
  // chân L/R đấu GND hay VDD).
  i2s_config_t cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
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
  const int TARGET = 4000;             // số FRAME stereo cần đo
  int32_t tmp[128];                    // 64 frame stereo/lần đọc (2 int32/frame)
  int32_t maxL = 0, maxR = 0;
  int frames = 0;
  unsigned long t0 = millis();
  while (frames < TARGET && millis() - t0 < 800) {
    size_t br = 0;
    if (i2s_read(I2S_NUM_1, tmp, sizeof(tmp), &br, pdMS_TO_TICKS(50)) != ESP_OK) continue;
    int nf = br / (2 * sizeof(int32_t));
    for (int f = 0; f < nf; f++) {
      int32_t l = tmp[f * 2]     >> MIC_SHIFT;
      int32_t r = tmp[f * 2 + 1] >> MIC_SHIFT;
      if (l < 0) l = -l;  if (r < 0) r = -r;
      if (l > maxL) maxL = l;
      if (r > maxR) maxR = r;
      frames++;
    }
  }
  // Chọn kênh có biên độ lớn hơn — mic nằm ở kênh đó
  g_micChannel = (maxR > maxL) ? 1 : 0;
  int32_t best = g_micChannel ? maxR : maxL;
  Serial.printf("[Mic-test] Đọc %d frame — kênh L max=%d, kênh R max=%d → chọn kênh %s\n",
                frames, (int)maxL, (int)maxR, g_micChannel ? "R" : "L");
  if (frames == 0) {
    Serial.println("[Mic-test] ✗ Đọc 0 frame — driver mic lỗi (kiểm tra SCK=14 WS=15)");
  } else if (best < 80) {
    Serial.println("[Mic-test] ✗ Cả 2 kênh IM — mic không có tín hiệu.");
    Serial.println("           Kiểm tra: SD→GPIO34? VDD=3.3V? GND chung? Dây có tuột không?");
  } else {
    Serial.printf("[Mic-test] ✓ Mic SỐNG (kênh %s, max=%d)\n",
                  g_micChannel ? "R" : "L", (int)best);
  }
}

void recordAudio() {
  audioBufSamples = 0;
  size_t maxSamples = audioBufCap;   // bound theo buffer THỰC đã cấp phát (chống tràn heap)
  unsigned long start = millis();
  unsigned long deadline = start + (RECORD_SECONDS * 1000UL);

  int32_t tmp[128];   // 64 frame stereo/lần đọc
  while (digitalRead(BTN_PIN) == LOW
         && millis() < deadline
         && audioBufSamples < maxSamples) {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_NUM_1, tmp, sizeof(tmp), &bytesRead, pdMS_TO_TICKS(50));
    if (err != ESP_OK || bytesRead == 0) continue;
    int nf = bytesRead / (2 * sizeof(int32_t));
    for (int f = 0; f < nf && audioBufSamples < maxSamples; f++) {
      int32_t v = tmp[f * 2 + g_micChannel] >> MIC_SHIFT;   // lấy kênh đã dò
      if (v > 32767)  v = 32767;                            // clamp chống tràn int16
      if (v < -32768) v = -32768;
      audioBuf[audioBufSamples++] = (int16_t)v;
    }
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
            g_emotion = faceEmotionFromStr(doc["emotion"] | "neutral");
            Serial.printf("[Chat] ✓ Nhận reply (%.1fs) | emotion=%s\n",
                          (millis() - t0) / 1000.0, doc["emotion"] | "neutral");
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

  if (reply.isEmpty()) {
    if (millis() >= deadline)
      Serial.println("[Chat] ✗ Timeout sau 20 giây — server không trả done-line");
    else
      Serial.println("[Chat] ✗ Stream đóng sớm (server ngắt kết nối?) — chưa nhận reply");
  }

  http.end();
  return reply;
}

// ────────────────────────────────────────────
//  TTS → phát loa
// ────────────────────────────────────────────
void startTTS(const String& reply) {
  // Giới hạn ~80 byte raw để URL không quá dài (nginx limit 8KB).
  // QUAN TRỌNG: length()/substring() đếm BYTE. Tiếng Việt = 3 byte/char nên
  // cắt thẳng byte 80 có thể xẻ đôi 1 ký tự UTF-8 → chuỗi hỏng → TTS lỗi.
  // Lùi điểm cắt về đầu ký tự (byte tiếp theo KHÔNG phải continuation 10xxxxxx).
  String text = reply;
  if (text.length() > 80) {
    int cut = 80;
    while (cut > 0 && ((uint8_t)text[cut] & 0xC0) == 0x80) cut--;
    text = text.substring(0, cut);
    Serial.printf("[TTS] ⚠ Reply dài, cắt còn %d byte (theo ranh giới UTF-8)\n", cut);
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
