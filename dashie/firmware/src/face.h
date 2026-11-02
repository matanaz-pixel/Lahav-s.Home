#pragma once
#include <M5Unified.h>

enum class Mood : uint8_t { NEUTRAL, HAPPY, WORRIED, ANGRY, CONFUSED, LISTEN, THINK, TALK };

struct FaceColors { uint16_t bg, fg, acc; };

static inline void faceMouth(M5Canvas& c, int cx, int cy, int w, int curve, int th, uint16_t col) {
  int px = cx - w, py = cy;
  for (int i = 1; i <= 16; i++) {
    float t = -1.0f + 2.0f * i / 16.0f;
    int x = cx + (int)(t * w);
    int y = cy + (int)(curve * (1.0f - t * t));
    for (int k = -th / 2; k <= th / 2; k++) c.drawLine(px, py + k, x, y + k, col);
    px = x; py = y;
  }
}

// cx,cy = face centre, s = scale (1.0 = 320x240 hero face)
static void drawFace(M5Canvas& c, int cx, int cy, float s, Mood mood, uint32_t now, const FaceColors& k, float mouthOpen = 0.0f) {
  static uint32_t nextBlink = 2500, blinkEnd = 0;
  if (now >= nextBlink) { blinkEnd = now + 130; nextBlink = now + random(2500, 6000); }
  float open = (now < blinkEnd) ? 0.08f : 1.0f;

  float ew = 56 * s, eh = 76 * s;
  float gx = sinf(now / 2300.0f) * 7 * s, gy = sinf(now / 3100.0f) * 3 * s;
  if (mood == Mood::LISTEN) { ew *= 1.2f; eh *= 1.2f; open = 1.0f; gx = gy = 0; }
  if (mood == Mood::THINK) { gx = cosf(now / 500.0f) * 10 * s - 8 * s; gy = -10 * s + sinf(now / 500.0f) * 6 * s; }

  float h = eh * open;
  int ex = (int)(62 * s), ey = cy - (int)(12 * s);
  int lx = cx - ex - (int)(ew / 2) + (int)gx;
  int rx = cx + ex - (int)(ew / 2) + (int)gx;
  int top = ey - (int)(h / 2) + (int)gy;
  int w = (int)ew, hh = (int)h;
  float hr = (mood == Mood::CONFUSED) ? 0.6f : 1.0f;
  int hhR = (int)(h * hr);
  int topR = ey - hhR / 2 + (int)gy;
  int rad = (int)min(ew * 0.3f, h * 0.5f);
  if (rad < 1) rad = 1;
  int radR = (int)min(ew * 0.3f, hhR * 0.5f);
  if (radR < 1) radR = 1;

  c.fillRoundRect(lx, top, w, hh, rad, k.fg);
  c.fillRoundRect(rx, topR, w, hhR, radR, k.fg);

  switch (mood) {
    case Mood::HAPPY:
      c.fillRect(lx - 2, top + (int)(hh * 0.55f), w + 4, (int)(hh * 0.5f) + 2, k.bg);
      c.fillRect(rx - 2, top + (int)(hh * 0.55f), w + 4, (int)(hh * 0.5f) + 2, k.bg);
      break;
    case Mood::ANGRY:
      c.fillTriangle(lx - 2, top - 2, lx + w + 2, top - 2, lx + w + 2, top + (int)(hh * 0.5f), k.bg);
      c.fillTriangle(rx - 2, top - 2, rx + w + 2, top - 2, rx - 2, top + (int)(hh * 0.5f), k.bg);
      break;
    case Mood::WORRIED:
      c.fillTriangle(lx - 2, top - 2, lx + w + 2, top - 2, lx - 2, top + (int)(hh * 0.5f), k.bg);
      c.fillTriangle(rx - 2, top - 2, rx + w + 2, top - 2, rx + w + 2, top + (int)(hh * 0.5f), k.bg);
      break;
    default: break;
  }

  int my = cy + (int)(62 * s);
  int th = max(2, (int)(4 * s));
  switch (mood) {
    case Mood::NEUTRAL: faceMouth(c, cx, my, (int)(26 * s), (int)(6 * s), th, k.fg); break;
    case Mood::HAPPY:   faceMouth(c, cx, my, (int)(36 * s), (int)(18 * s), th, k.fg); break;
    case Mood::WORRIED: faceMouth(c, cx, my + (int)(8 * s), (int)(24 * s), -(int)(10 * s), th, k.fg); break;
    case Mood::ANGRY:   faceMouth(c, cx, my + (int)(8 * s), (int)(30 * s), -(int)(8 * s), th, k.fg); break;
    case Mood::CONFUSED: {
      int px = cx - (int)(28 * s), py = my;
      for (int i = 1; i <= 4; i++) {
        int x = cx - (int)(28 * s) + (int)(14 * s) * i;
        int y = my + ((i & 1) ? -(int)(6 * s) : (int)(6 * s));
        for (int q = -th / 2; q <= th / 2; q++) c.drawLine(px, py + q, x, y + q, k.fg);
        px = x; py = y;
      }
      break;
    }
    case Mood::TALK: {
      int mw = (int)(40 * s);
      int mh = max(th, (int)((4 + mouthOpen * 30) * s));
      c.fillRoundRect(cx - mw / 2, my - mh / 2 + (int)(4 * s), mw, mh, max(1, mh / 2), k.fg);
      break;
    }
    case Mood::LISTEN:
      c.fillCircle(cx, my, (int)(9 * s), k.fg);
      if (s >= 0.8f) {
        int pulse = (int)(sinf(now / 150.0f) * 5);
        c.drawCircle(cx, cy, (int)(105 * s) + pulse, k.acc);
        c.drawCircle(cx, cy, (int)(106 * s) + pulse, k.acc);
      }
      break;
    case Mood::THINK:
      for (int i = 0; i < 3; i++) {
        int r = ((now / 300) % 3 == (uint32_t)i) ? (int)(8 * s) : (int)(4 * s);
        c.fillCircle(cx - (int)(24 * s) + (int)(24 * s) * i, my, max(2, r), k.acc);
      }
      break;
  }
}
