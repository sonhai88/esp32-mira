#include "face.h"
#include <math.h>
#include <string.h>

// Toạ độ khung mặt trên 128x64
static const int EYE_L_X = 42;
static const int EYE_R_X = 86;
static const int EYE_Y   = 26;
static const int EYE_R   = 11;
static const int MOUTH_X = 64;
static const int MOUTH_Y = 48;

FaceEmotion faceEmotionFromStr(const char* s) {
  if (!s) return EMO_NEUTRAL;
  if (!strcmp(s, "happy"))   return EMO_HAPPY;
  if (!strcmp(s, "sad"))     return EMO_SAD;
  if (!strcmp(s, "curious")) return EMO_CURIOUS;
  if (!strcmp(s, "playful")) return EMO_PLAYFUL;
  if (!strcmp(s, "caring"))  return EMO_CARING;
  return EMO_NEUTRAL;   // neutral + bất kỳ giá trị lạ nào
}

// ── Vẽ 1 mắt ──
// leftEye=true cho mắt trái (dùng cho emotion bất đối xứng: curious, playful)
static void drawEye(U8G2& g, int cx, int cy, int r, bool blink,
                    FaceEmotion emo, bool leftEye) {
  if (blink) { g.drawHLine(cx - r, cy, 2 * r); return; }   // mắt nhắm = gạch ngang

  switch (emo) {
    case EMO_HAPPY:            // mắt cười ^  (2 đoạn thẳng)
      g.drawLine(cx - r, cy + 3, cx,     cy - r + 3);
      g.drawLine(cx,     cy - r + 3, cx + r, cy + 3);
      break;

    case EMO_SAD:             // mắt tròn + mi trên xiên buồn
      g.drawDisc(cx, cy, r);
      g.setDrawColor(0);
      g.drawBox(cx - r - 1, cy - r - 1, 2 * r + 2, r);      // xoá nửa trên
      g.setDrawColor(1);
      if (leftEye) g.drawLine(cx - r, cy - 4, cx + r, cy - 1);
      else         g.drawLine(cx - r, cy - 1, cx + r, cy - 4);
      break;

    case EMO_CURIOUS:         // 1 mắt to, 1 mắt nhỏ (nhướn)
      g.drawDisc(cx, cy, leftEye ? r : r - 4);
      break;

    case EMO_PLAYFUL:         // mắt trái nháy (nhắm), phải mở
      if (leftEye) g.drawHLine(cx - r, cy, 2 * r);
      else         g.drawDisc(cx, cy, r);
      break;

    case EMO_CARING:          // mắt tròn dịu (nhỏ hơn) + mi cong hiền
      g.drawDisc(cx, cy, r - 2);
      g.drawLine(cx - r, cy - r + 2, cx + r, cy - r + 2);
      break;

    case EMO_NEUTRAL:
    default:
      g.drawDisc(cx, cy, r);
      break;
  }
}

// ── Vẽ miệng ──
// speaking=true → miệng mở-đóng theo animMs (nói)
static void drawMouth(U8G2& g, FaceEmotion emo, bool speaking, unsigned long animMs) {
  if (speaking) {
    // Há miệng dao động: cao 3..11 px theo sin
    float ph = (animMs % 400) / 400.0f * 2.0f * (float)M_PI;
    int h = 4 + (int)(4.0f * (0.5f + 0.5f * sinf(ph)));
    g.drawRBox(MOUTH_X - 9, MOUTH_Y - h / 2, 18, h, 2);
    return;
  }
  switch (emo) {
    case EMO_HAPPY:
    case EMO_PLAYFUL:         // cười: cung dưới (3 đoạn xấp xỉ)
      g.drawLine(MOUTH_X - 12, MOUTH_Y,     MOUTH_X - 5, MOUTH_Y + 5);
      g.drawLine(MOUTH_X - 5,  MOUTH_Y + 5, MOUTH_X + 5, MOUTH_Y + 5);
      g.drawLine(MOUTH_X + 5,  MOUTH_Y + 5, MOUTH_X + 12, MOUTH_Y);
      break;
    case EMO_SAD:            // méo xuống (cung ngược)
      g.drawLine(MOUTH_X - 12, MOUTH_Y + 5, MOUTH_X - 5, MOUTH_Y);
      g.drawLine(MOUTH_X - 5,  MOUTH_Y,     MOUTH_X + 5, MOUTH_Y);
      g.drawLine(MOUTH_X + 5,  MOUTH_Y,     MOUTH_X + 12, MOUTH_Y + 5);
      break;
    case EMO_CURIOUS:        // chữ o nhỏ (tò mò)
      g.drawCircle(MOUTH_X, MOUTH_Y + 2, 4);
      break;
    case EMO_CARING:         // cười nhẹ (đường cong thấp)
      g.drawLine(MOUTH_X - 9, MOUTH_Y + 1, MOUTH_X, MOUTH_Y + 4);
      g.drawLine(MOUTH_X, MOUTH_Y + 4, MOUTH_X + 9, MOUTH_Y + 1);
      break;
    case EMO_NEUTRAL:
    default:                // đường ngang
      g.drawHLine(MOUTH_X - 8, MOUTH_Y + 2, 16);
      break;
  }
}

void faceDraw(U8G2& g, FaceState st, FaceEmotion emo, unsigned long animMs) {
  g.clearBuffer();
  g.setDrawColor(1);

  int r    = EYE_R;
  int cyL  = EYE_Y, cyR = EYE_Y;
  bool blink = false;

  // ── STATE điều chỉnh khung ──
  switch (st) {
    case FACE_IDLE:
      blink = (animMs % 3200) < 150;          // chớp mắt mỗi ~3.2s
      break;
    case FACE_LISTENING:
      r = EYE_R + 2;                          // mắt mở to (chăm chú nghe)
      break;
    case FACE_THINKING:
      cyL = cyR = EYE_Y - 5;                  // liếc lên (đang nghĩ)
      for (int i = 0; i < 3; i++)             // "..." nhấp nháy góc dưới
        if (((animMs / 350) % 4) > (unsigned long)i)
          g.drawDisc(90 + i * 9, 56, 2);
      break;
    case FACE_SPEAKING:
      break;                                  // mắt theo emotion, miệng động
  }

  // ── Mắt ──
  drawEye(g, EYE_L_X, cyL, r, blink, emo, true);
  drawEye(g, EYE_R_X, cyR, r, blink, emo, false);

  // ── Miệng ──
  drawMouth(g, emo, st == FACE_SPEAKING, animMs);

  g.sendBuffer();
}
