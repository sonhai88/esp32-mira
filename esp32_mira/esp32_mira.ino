/**
 * ESP32-Mira Voice Client v1.0
 *
 * Flow: Nhấn giữ nút → ghi âm mic → thả nút
 *       → /stt (Whisper) → /chat/stream → /tts/stream → phát loa
 *
 * Lib cần cài (Tools → Manage Libraries):
 *   - ESP32-audioI2S  by schreibfaul1  (phát MP3 từ URL)
 *   - ArduinoJson     by Benoit Blanchon (parse JSON)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Audio.h>        // ESP32-audioI2S
#include <ArduinoJson.h>
#include "config.h"

// ── Audio player (TTS) ──
Audio audio;

// ── Audio buffer ghi âm ──
// PSRAM_ATTR → lưu vào PSRAM ngoài (8MB), không ăn SRAM
int16_t* audioBuf = nullptr;
size_t   audioBufSamples = 0;

// ── State machine ──
enum State { IDLE, RECORDING, PROCESSING, SPEAKING };
volatile State state = IDLE;
volatile bool btnPressed = false;

// ────────────────────────────────────────────
//  SETUP
// ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[Mira] Khởi động...");

  // Cấp phát audio buffer từ PSRAM
  audioBuf = (int16_t*)ps_malloc(AUDIO_BUF_SIZE);
  if (!audioBuf) {
    Serial.println("[ERROR] Không cấp phát được PSRAM — kiểm tra board có PSRAM không");
    while (true) delay(1000);
  }

  pinMode(BTN_PIN, INPUT_PULLUP);
  attachInterrupt(BTN_PIN, onBtnChange, CHANGE);

  setupMic();

  // Speaker: dùng ESP32-audioI2S lib
  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
  audio.setVolume(15);  // 0-21

  connectWiFi();
  Serial.println("[Mira] Sẵn sàng! Nhấn giữ nút để nói...");
}

// ────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────
void loop() {
  // Khi đang SPEAKING → gọi audio.loop() liên tục để stream MP3
  if (state == SPEAKING) {
    audio.loop();
    return;
  }

  // Nhấn nút → bắt đầu ghi âm
  if (btnPressed && state == IDLE) {
    btnPressed = false;
    state = RECORDING;
    Serial.println("[Mic] Đang nghe...");
    recordAudio();

    if (audioBufSamples == 0) {
      state = IDLE;
      return;
    }

    state = PROCESSING;
    Serial.printf("[Mic] Ghi được %.1f giây\n", (float)audioBufSamples / SAMPLE_RATE);

    // Chạy pipeline
    String transcript = callSTT();
    if (transcript.length() == 0) {
      Serial.println("[STT] Không nghe rõ");
      state = IDLE;
      return;
    }
    Serial.println("[STT] Anh nói: " + transcript);

    String reply = callChat(transcript);
    if (reply.length() == 0) {
      Serial.println("[Chat] Không có reply");
      state = IDLE;
      return;
    }
    Serial.println("[Mira] " + reply);

    // TTS: ESP32-audioI2S tự stream + decode MP3
    String ttsUrl = String(MIRA_BASE_URL) + "/tts/stream?text=" + urlEncode(reply);
    state = SPEAKING;
    audio.connecttohost(ttsUrl.c_str());
    // audio.loop() sẽ chạy ở đầu loop() cho đến khi hết audio
  }

  delay(10);
}

// Callback từ ESP32-audioI2S khi phát xong
void audio_eof_mp3(const char* info) {
  Serial.println("[TTS] Phát xong → sẵn sàng lại");
  state = IDLE;
}

// ISR nút bấm
void IRAM_ATTR onBtnChange() {
  if (digitalRead(BTN_PIN) == LOW && state == IDLE) {
    btnPressed = true;
  }
}

// ────────────────────────────────────────────
//  WIFI
// ────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Kết nối " + String(WIFI_SSID) + " ");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✓ IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(" ✗ FAIL");
  }
}

// ────────────────────────────────────────────
//  I2S MIC (INMP441) — I2S_NUM_0
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
  ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL));
  ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pins));
  Serial.println("[Mic] I2S ready");
}

void recordAudio() {
  audioBufSamples = 0;
  unsigned long start = millis();
  size_t maxSamples = AUDIO_BUF_SIZE / 2;

  // Ghi khi nút vẫn nhấn và chưa đầy buffer
  while (digitalRead(BTN_PIN) == LOW &&
         millis() - start < (RECORD_SECONDS * 1000UL) &&
         audioBufSamples < maxSamples) {
    int16_t tmp[256];
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &bytesRead, 50);
    size_t n = bytesRead / 2;
    memcpy(audioBuf + audioBufSamples, tmp, bytesRead);
    audioBufSamples += n;
  }
}

// ────────────────────────────────────────────
//  HTTP /stt  → transcript
// ────────────────────────────────────────────
String callSTT() {
  // Build WAV (44 byte header + PCM)
  size_t pcmLen = audioBufSamples * 2;
  size_t wavLen = 44 + pcmLen;
  uint8_t* wav = (uint8_t*)ps_malloc(wavLen);
  if (!wav) return "";
  buildWAVHeader(wav, pcmLen);
  memcpy(wav + 44, (uint8_t*)audioBuf, pcmLen);

  // Build multipart body
  String boundary = "mira8266";
  String head = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"audio\"; filename=\"rec.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t bodyLen = head.length() + wavLen + tail.length();
  uint8_t* body = (uint8_t*)ps_malloc(bodyLen);
  if (!body) { free(wav); return ""; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), wav, wavLen);
  memcpy(body + head.length() + wavLen, tail.c_str(), tail.length());
  free(wav);

  HTTPClient http;
  http.begin(String(MIRA_BASE_URL) + "/stt");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.setTimeout(15000);

  int code = http.POST(body, bodyLen);
  free(body);

  if (code != 200) {
    Serial.printf("[STT] HTTP %d: %s\n", code, http.getString().c_str());
    http.end();
    return "";
  }

  // Parse JSON {"transcript":"..."}
  JsonDocument doc;
  deserializeJson(doc, http.getString());
  http.end();
  return doc["transcript"].as<String>();
}

// ────────────────────────────────────────────
//  HTTP /chat/stream → reply
// ────────────────────────────────────────────
String callChat(String message) {
  HTTPClient http;
  http.begin(String(MIRA_BASE_URL) + "/chat/stream");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);
  http.setTimeout(20000);

  // Escape message
  message.replace("\\", "\\\\");
  message.replace("\"", "\\\"");

  int code = http.POST("{\"message\":\"" + message + "\"}");
  if (code != 200) {
    Serial.printf("[Chat] HTTP %d\n", code);
    http.end();
    return "";
  }

  // Đọc NDJSON, tìm dòng {"done":true,...,"reply":"..."}
  String reply = "";
  WiFiClient* stream = http.getStreamPtr();
  String line = "";
  unsigned long t = millis();

  while ((http.connected() || stream->available()) && millis() - t < 15000) {
    if (!stream->available()) { delay(5); continue; }
    char c = stream->read();
    if (c == '\n') {
      if (line.indexOf("\"done\":true") >= 0) {
        JsonDocument doc;
        if (deserializeJson(doc, line) == DeserializationError::Ok) {
          reply = doc["reply"].as<String>();
        }
        break;
      }
      line = "";
    } else {
      line += c;
    }
  }
  http.end();
  return reply;
}

// ────────────────────────────────────────────
//  UTILS
// ────────────────────────────────────────────
void buildWAVHeader(uint8_t* buf, size_t pcmLen) {
  uint32_t sr = SAMPLE_RATE;
  uint32_t byteRate = sr * 2;   // mono 16-bit
  uint16_t blockAlign = 2;

  memcpy(buf,     "RIFF", 4);
  *(uint32_t*)(buf+4)  = 36 + pcmLen;
  memcpy(buf+8,   "WAVE", 4);
  memcpy(buf+12,  "fmt ", 4);
  *(uint32_t*)(buf+16) = 16;
  *(uint16_t*)(buf+20) = 1;          // PCM
  *(uint16_t*)(buf+22) = 1;          // mono
  *(uint32_t*)(buf+24) = sr;
  *(uint32_t*)(buf+28) = byteRate;
  *(uint16_t*)(buf+32) = blockAlign;
  *(uint16_t*)(buf+34) = 16;
  memcpy(buf+36,  "data", 4);
  *(uint32_t*)(buf+40) = pcmLen;
}

String urlEncode(String s) {
  String out = "";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isAlphaNumeric(c) || c=='-' || c=='_' || c=='.' || c=='~') {
      out += c;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
      out += hex;
    }
  }
  return out;
}
