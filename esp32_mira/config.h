#pragma once

// ── WiFi ──
#define WIFI_SSID     "TenWifi"
#define WIFI_PASSWORD "MatKhau"

// ── Mira Server ──
#define MIRA_BASE_URL  "https://deli2222-mira-ai.hf.space"
#define DEVICE_API_KEY "mira-device-key-2026"

// ── Pin I2S Mic (INMP441) ──
#define MIC_SCK   41
#define MIC_WS    42
#define MIC_SD    2

// ── Pin I2S Loa (MAX98357A) ──
#define SPK_BCLK  39
#define SPK_LRC   40
#define SPK_DIN   38

// ── Nút bấm ──
#define BTN_PIN   0   // GPIO0 = nút BOOT sẵn trên board

// ── Ghi âm ──
#define SAMPLE_RATE     16000   // Hz — Whisper yêu cầu 16kHz
#define RECORD_SECONDS  5       // giây ghi âm tối đa
#define AUDIO_BUF_SIZE  (SAMPLE_RATE * RECORD_SECONDS * 2)  // 16-bit = 2 bytes/sample
