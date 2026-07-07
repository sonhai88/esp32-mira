#pragma once
#include <U8g2lib.h>

// ── Mặt cảm xúc Mira trên OLED 128x64 ──
// 2 trục độc lập:
//   STATE   — firmware tự biết, điều khiển animation (chớp mắt, miệng nói)
//   EMOTION — parse từ /chat response ("emotion": ...), điều khiển hình mắt/miệng

enum FaceState   { FACE_IDLE, FACE_LISTENING, FACE_THINKING, FACE_SPEAKING };
enum FaceEmotion { EMO_NEUTRAL, EMO_HAPPY, EMO_SAD, EMO_CURIOUS, EMO_PLAYFUL, EMO_CARING };

// Map chuỗi emotion server trả về → enum (neutral nếu không khớp/null)
FaceEmotion faceEmotionFromStr(const char* s);

// Vẽ 1 frame mặt lên g (tự clearBuffer + sendBuffer).
// animMs = millis() để animation chạy (chớp mắt idle, miệng nói).
void faceDraw(U8G2& g, FaceState st, FaceEmotion emo, unsigned long animMs);
