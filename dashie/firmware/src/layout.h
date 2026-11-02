#pragma once
// ---------------------------------------------------------------------------
// Screen layout. All UI code positions things through `L` (never raw pixel numbers),
// so the same screens run on 320x240 (Core2 / CoreS3) and 240x135 (StickC Plus2).
//
// L.init(w, h) is called once after M5.begin() (and after any rotation) with the real
// display size. Two profiles are derived from it:
//   normal  (H >= 200): the original 320x240 design; every value below equals the
//                       number that used to be hard-coded, so Core2/CoreS3 look unchanged.
//   compact (H <  200): 240x135 landscape, no touch, 16 px hint bar at the bottom.
// X positions are relative to W (right edge = W - margin); Y positions are per profile.
// ---------------------------------------------------------------------------
#include <Arduino.h>

struct Layout {
  int W = 320, H = 240;
  bool compact = false;

  // bottom hint bar (labels for the three physical buttons / touch zones)
  int barY = 214, barTextY = 219;
  // hero face (idle) and overlay faces
  int faceCy = 100, listenCy = 96, thinkCy = 96, errCy = 90;
  float faceS = 1.0f, errS = 0.8f;
  // face screen decorations
  int clockSz = 2, clockY = 6, wifiY = 6, dotX = 12, dotY = 26, dotR = 4;
  // one-line status under the face (Listening / Thinking / Recording ...), error text, "Whoa!"
  int statusY = 196, errY = 190, whoaY = 186;
  // noise screen
  int noiseTitleX = 8, noiseTitleY = 6, noiseFaceX = 62, noiseFaceY = 100; float noiseFaceS = 0.42f;
  int bigNumX = 205, bigNumY = 38, dbX = 296, dbY = 78, quietX = 160, quietY = 104;
  int histX = 10, histBase = 205, histStep = 5, histW = 4, histMaxH = 90;
  // G-force screen: circle + three stat blocks. Stats are given for English; Hebrew mirrors x and alignment.
  int gCx = 160, gCy = 108, gR = 84, gBall = 9;
  int gStatX[3] = {8, 312, 312};          // 0 = Max G, 1 = Hard brakes, 2 = Sharp turns
  uint8_t gStatAlign[3] = {0, 2, 2};      // 0 left, 2 right (same values as enum Align)
  int gStatLabelY[3] = {6, 6, 44};
  int gStatValueY[3] = {18, 18, 56};
  // trip screen
  int rowY0 = 14, rowPitch = 32, rowMargin = 10, pressY = 196;
  // settings screen
  int setY0 = 10, setPitch6 = 33, setPitch7 = 28, setVisible = 7, setRectDy = 5;
  int pickTitleY = 6, pickY0 = 34, pickPitch = 22, pickRectH = 21, pickVisible = 8;
  // recording overlay
  int recFaceY = 52; float recFaceS = 0.5f;
  int waveX = 8, waveStep = 5, waveW = 3, waveBase = 150, waveMaxH = 70;
  // reply overlay
  int replyFaceX = 160, replyFaceY = 46; float replyFaceS = 0.5f;
  int replyTextY = 100, replyBottom = 210, replyLineLatin = 18, replyLineHe = 24, wrapW = 300, margin = 8;
  // crash screen
  int crashY1 = 20, crashY2 = 56, crashNumY = 96, crashHintY = 190, crashSosY = 100;

  int barH() const { return H - barY; }
  int cx() const { return W / 2; }
  int right() const { return W - margin; }
  int mirror(int x) const { return W - x; }

  void init(int w, int h) {
    W = w; H = h;
    compact = (h < 200);
    if (!compact) { *this = Layout(); W = w; H = h; return; }   // normal: original 320x240 numbers
    // ---- compact 240x135 ----
    barY = H - 17; barTextY = barY + 1;
    faceCy = listenCy = thinkCy = errCy = 44; faceS = 0.75f; errS = 0.7f;
    clockSz = 1; clockY = 2; wifiY = 2; dotX = 8; dotY = 20; dotR = 3;
    statusY = H - 20; errY = H - 22; whoaY = barY + 1;
    noiseTitleX = 4; noiseTitleY = 2; noiseFaceX = 30; noiseFaceY = 64; noiseFaceS = 0.3f;
    bigNumX = 130; bigNumY = 14; dbX = 184; dbY = 44; quietX = W / 2; quietY = 66;
    histX = (W - 60 * 3) / 2; histBase = barY - 4; histStep = 3; histW = 2; histMaxH = 36;
    gCx = 62; gCy = 60; gR = 50; gBall = 6;
    for (int i = 0; i < 3; i++) { gStatX[i] = W - 4; gStatAlign[i] = 2; }
    gStatLabelY[0] = 3;  gStatValueY[0] = 12;
    gStatLabelY[1] = 38; gStatValueY[1] = 47;
    gStatLabelY[2] = 72; gStatValueY[2] = 81;
    rowY0 = 2; rowPitch = 17; rowMargin = 6; pressY = barY - 12;
    setY0 = 7; setPitch6 = setPitch7 = 28; setVisible = 4; setRectDy = 5;
    pickTitleY = 2; pickY0 = 18; pickPitch = 14; pickRectH = 13; pickVisible = (barY - 18) / 14;
    recFaceY = 22; recFaceS = 0.3f;
    waveX = (W - 64 * 3) / 2; waveStep = 3; waveW = 2; waveBase = 78; waveMaxH = 44;
    replyFaceX = W / 2; replyFaceY = 18; replyFaceS = 0.28f;
    replyTextY = 44; replyBottom = H - 2; replyLineLatin = 17; replyLineHe = 22; wrapW = W - 16; margin = 8;
    crashY1 = 4; crashY2 = 24; crashNumY = 44; crashHintY = H - 22; crashSosY = 50;
  }
};

static Layout L;
