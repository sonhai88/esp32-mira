/**
 * ESP32-Mira Voice Client
 *
 * Flow: Nhấn nút → ghi âm → /stt → /chat/stream → /tts → phát loa
 *
 * Phần cứng:
 *   - ESP32-S3 DevKit
 *   - INMP441 (mic I2S)
 *   - MAX98357A (amp I2S)
 *   - Loa 3W
 *   - Nút bấm (GPIO0 = nút BOOT sẵn)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include "config.h"

// ── State ──
enum State { IDLE, RECORDING, PROCESSING, SPEAKING };
State state = IDLE;

// ── Audio buffer ──
int16_t audioBuf[AUDIO_BUF_SIZE / 2];
size_t  audioBufLen = 0;

// ────────────────────────────────────────────
//  SETUP
// ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("[Mira] Khởi động...");

  pinMode(BTN_PIN, INPUT_PULLUP);

  setupMic();
  setupSpeaker();
  connectWiFi();

  Serial.println("[Mira] Sẵn sàng. Nhấn nút để nói.");
}

// ────────────────────────────────────────────
//  LOOP
// ────────────────────────────────────────────
void loop() {
  // Nhấn nút (LOW = nhấn vì INPUT_PULLUP)
  if (digitalRead(BTN_PIN) == LOW && state == IDLE) {
    Serial.println("[Mira] Đang nghe...");
    state = RECORDING;
    recordAudio();
    state = PROCESSING;
    Serial.println("[Mira] Xử lý...");
    pipeline();
    state = IDLE;
    Serial.println("[Mira] Sẵn sàng.");
  }
  delay(10);
}

// ────────────────────────────────────────────
//  PIPELINE: audio → STT → Chat → TTS → Loa
// ────────────────────────────────────────────
void pipeline() {
  // 1. STT
  String transcript = callSTT();
  if (transcript.length() == 0) {
    Serial.println("[STT] Không nghe rõ");
    return;
  }
  Serial.println("[STT] " + transcript);

  // 2. Chat
  String reply = callChat(transcript);
  if (reply.length() == 0) {
    Serial.println("[Chat] Không có reply");
    return;
  }
  Serial.println("[Mira] " + reply);

  // 3. TTS → phát loa
  state = SPEAKING;
  playTTS(reply);
}

// ────────────────────────────────────────────
//  WIFI
// ────────────────────────────────────────────
void connectWiFi() {
  Serial.print("[WiFi] Kết nối " + String(WIFI_SSID));
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" OK! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println(" FAIL — kiểm tra SSID/password");
  }
}

// ────────────────────────────────────────────
//  I2S MIC (INMP441)
// ────────────────────────────────────────────
void setupMic() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = MIC_SCK,
    .ws_io_num  = MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = MIC_SD,
  };
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  Serial.println("[Mic] OK");
}

void recordAudio() {
  audioBufLen = 0;
  size_t bytesRead = 0;
  unsigned long start = millis();

  // Ghi cho đến khi thả nút HOẶC hết RECORD_SECONDS
  while (digitalRead(BTN_PIN) == LOW &&
         millis() - start < (unsigned long)(RECORD_SECONDS * 1000) &&
         audioBufLen < AUDIO_BUF_SIZE / 2) {
    int16_t tmp[256];
    i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &bytesRead, 100);
    size_t samples = bytesRead / 2;
    memcpy(audioBuf + audioBufLen, tmp, bytesRead);
    audioBufLen += samples;
  }
  Serial.printf("[Mic] Ghi được %d samples (%.1f giây)\n",
                audioBufLen, (float)audioBufLen / SAMPLE_RATE);
}

// ────────────────────────────────────────────
//  I2S SPEAKER (MAX98357A)
// ────────────────────────────────────────────
void setupSpeaker() {
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0,
  };
  i2s_pin_config_t pins = {
    .bck_io_num = SPK_BCLK,
    .ws_io_num  = SPK_LRC,
    .data_out_num = SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE,
  };
  i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pins);
  Serial.println("[Speaker] OK");
}

// ────────────────────────────────────────────
//  HTTP: /stt
// ────────────────────────────────────────────
String callSTT() {
  // Build WAV header + PCM data
  uint8_t* wav = buildWAV((uint8_t*)audioBuf, audioBufLen * 2);
  size_t wavLen = 44 + audioBufLen * 2;

  HTTPClient http;
  http.begin(String(MIRA_BASE_URL) + "/stt");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);

  // Multipart boundary
  String boundary = "----Esp32Mira";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String bodyStart = "--" + boundary + "\r\n"
    "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\n"
    "Content-Type: audio/wav\r\n\r\n";
  String bodyEnd = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = bodyStart.length() + wavLen + bodyEnd.length();
  uint8_t* body = (uint8_t*)malloc(totalLen);
  if (!body) { free(wav); return ""; }

  memcpy(body, bodyStart.c_str(), bodyStart.length());
  memcpy(body + bodyStart.length(), wav, wavLen);
  memcpy(body + bodyStart.length() + wavLen, bodyEnd.c_str(), bodyEnd.length());
  free(wav);

  int code = http.POST(body, totalLen);
  free(body);

  if (code != 200) {
    Serial.printf("[STT] HTTP %d\n", code);
    return "";
  }

  String resp = http.getString();
  http.end();

  // Parse {"transcript": "..."}
  int idx = resp.indexOf("\"transcript\"");
  if (idx < 0) return "";
  int s = resp.indexOf('"', idx + 13) + 1;
  int e = resp.indexOf('"', s);
  return resp.substring(s, e);
}

// ────────────────────────────────────────────
//  HTTP: /chat/stream
// ────────────────────────────────────────────
String callChat(String message) {
  HTTPClient http;
  http.begin(String(MIRA_BASE_URL) + "/chat/stream");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_API_KEY);

  // Escape quotes trong message
  message.replace("\"", "\\\"");
  String body = "{\"message\":\"" + message + "\"}";
  int code = http.POST(body);

  if (code != 200) {
    Serial.printf("[Chat] HTTP %d\n", code);
    return "";
  }

  // Đọc NDJSON stream, gom field "reply" từ dòng {"done":true,...}
  String fullReply = "";
  WiFiClient* stream = http.getStreamPtr();
  String line = "";
  while (http.connected() || stream->available()) {
    if (stream->available()) {
      char c = stream->read();
      if (c == '\n') {
        if (line.length() > 0) {
          // Tìm "reply" trong dòng done
          if (line.indexOf("\"done\":true") >= 0) {
            int ri = line.indexOf("\"reply\"");
            if (ri >= 0) {
              int rs = line.indexOf('"', ri + 8) + 1;
              int re = line.indexOf('"', rs);
              fullReply = line.substring(rs, re);
            }
          }
          line = "";
        }
      } else {
        line += c;
      }
    }
  }
  http.end();
  return fullReply;
}

// ────────────────────────────────────────────
//  HTTP: /tts/stream → phát loa
// ────────────────────────────────────────────
void playTTS(String text) {
  HTTPClient http;
  String url = String(MIRA_BASE_URL) + "/tts/stream?text=" + urlEncode(text);
  http.begin(url);
  int code = http.GET();

  if (code != 200) {
    Serial.printf("[TTS] HTTP %d\n", code);
    return;
  }

  // Stream MP3 bytes vào I2S
  // NOTE: cần decode MP3 → PCM. Dùng ESP32AudioI2S lib cho đơn giản.
  // Tạm thời: đọc raw bytes ra Serial để debug, sẽ thêm decoder sau.
  WiFiClient* stream = http.getStreamPtr();
  int total = 0;
  while (http.connected() || stream->available()) {
    if (stream->available()) {
      uint8_t buf[512];
      int n = stream->readBytes(buf, sizeof(buf));
      total += n;
      // TODO: decode MP3 → PCM → i2s_write(I2S_NUM_1, pcm, len, &w, 100)
    }
  }
  Serial.printf("[TTS] Nhận %d bytes audio\n", total);
  http.end();
}

// ────────────────────────────────────────────
//  UTILS
// ────────────────────────────────────────────
// Build WAV header (44 bytes) + PCM data
uint8_t* buildWAV(uint8_t* pcm, size_t pcmLen) {
  size_t totalLen = 44 + pcmLen;
  uint8_t* wav = (uint8_t*)malloc(totalLen);
  uint32_t sampleRate = SAMPLE_RATE;
  uint16_t channels = 1;
  uint16_t bitsPerSample = 16;
  uint32_t byteRate = sampleRate * channels * bitsPerSample / 8;
  uint16_t blockAlign = channels * bitsPerSample / 8;

  memcpy(wav,      "RIFF", 4);
  *(uint32_t*)(wav+4)  = totalLen - 8;
  memcpy(wav+8,    "WAVE", 4);
  memcpy(wav+12,   "fmt ", 4);
  *(uint32_t*)(wav+16) = 16;
  *(uint16_t*)(wav+20) = 1;           // PCM
  *(uint16_t*)(wav+22) = channels;
  *(uint32_t*)(wav+24) = sampleRate;
  *(uint32_t*)(wav+28) = byteRate;
  *(uint16_t*)(wav+32) = blockAlign;
  *(uint16_t*)(wav+34) = bitsPerSample;
  memcpy(wav+36,   "data", 4);
  *(uint32_t*)(wav+40) = pcmLen;
  memcpy(wav+44, pcm, pcmLen);
  return wav;
}

String urlEncode(String str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str[i];
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      encoded += '%';
      encoded += String(c, HEX);
    }
  }
  return encoded;
}
