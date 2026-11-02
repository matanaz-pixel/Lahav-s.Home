// Dashie - in-car voice buddy for M5Stack Core2 / CoreS3 (M5Unified)
// See ../../SPEC.md. All UI strings: strings.h (EN/HE). Hebrew font hook: font_he.h.

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <time.h>
#include <vector>

#include "config.h"
#include "strings.h"
#include "rtl.h"
#include "trigger.h"
#include "face.h"

#ifdef HAVE_HE_FONT
#include "font_he.h"   // must define: const uint8_t font_he_vlw[] PROGMEM = {...};
#endif

// ----------------------------------------------------------------------------
// Types & globals
// ----------------------------------------------------------------------------
enum class St : uint8_t { IDLE, LISTEN, RECORD, THINK, REPLY, ERROR, CRASH };
enum Screen : uint8_t { SC_FACE, SC_NOISE, SC_G, SC_TRIP, SC_SET, SC_COUNT };
enum Align : uint8_t { AL_L, AL_C, AL_R };

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
static const FaceColors THEMES[3] = {
  {rgb(0, 0, 0), rgb(235, 245, 255), rgb(0, 220, 255)},
  {rgb(0, 0, 0), rgb(255, 200, 90), rgb(255, 140, 0)},
  {rgb(0, 0, 0), rgb(200, 255, 210), rgb(0, 230, 100)},
};
static const uint8_t BRIGHT_LV[3] = {64, 160, 255};
static const uint8_t VOL_LV[5] = {0, 64, 128, 200, 255};
static const float SENS_DB[3] = {15.0f, 12.0f, 9.0f};

struct Settings { uint8_t bright = 1, vol = 2, sens = 1, theme = 0; bool voice = true; } cfg;
struct Trip { uint32_t secs = 0, brakes = 0, turns = 0, noiseN = 0; float maxG = 0, noiseSum = 0; } trip;
static constexpr int SET_ITEMS = 6;

static M5Canvas cv(&M5.Display);
static Preferences prefs;
static DoubleShout shout;

static St st = St::IDLE;
static uint32_t stSince = 0;
static Screen screen = SC_FACE;
static int setSel = 0;
static uint32_t lastActivity = 0, lastBtn = 0;
static uint32_t resetArmedAt = 0;
static uint8_t lastBright = 255;

// audio
enum class Aud : uint8_t { NONE, MIC, SPK };
static Aud aud = Aud::NONE;
static constexpr size_t FR = 512;                // 32 ms @16k
static constexpr uint32_t FRAME_MS = FR * 1000 / MIC_RATE;
static int16_t fbuf[3][FR];
static uint8_t fi = 0, fprimed = 0;
static uint32_t micGuardUntil = 0;
static float dbNow = 30, dbSm = 30;
static float wave[64];
static uint8_t waveIdx = 0;

// recording
static constexpr size_t MAX_SAMP = (size_t)MAX_REC_MS * MIC_RATE / 1000;
static uint8_t* recBuf = nullptr;                // 44-byte WAV header + PCM
static size_t recN = 0;
static bool speechSeen = false;
static uint32_t recQuietMs = 0, recMs = 0;

// per-second noise stats
static float secSum = 0; static uint32_t secCnt = 0;
static float hist[60]; static uint8_t histIdx = 0;
static uint32_t quietStreak = 0, bestQuiet = 0;
static uint32_t lastSec = 0, lastSave = 0;
static bool tripDirty = false;

// IMU
static float gX = 0, gY = 0, gZ = 0; static bool gInit = false;
static float latG = 0, fwdG = 0, hfEma = 0; static bool driving = false;
static bool brakeArmed = true, turnArmed = true; static uint32_t brakeRearm = 0, turnRearm = 0;
static uint32_t brakeFlashUntil = 0, crashHighSince = 0, crashRefract = 0, crashStart = 0, lastImu = 0;
static bool crashHigh = false, sosShown = false; static uint32_t lastCrashBeep = 0;
static float trail[24][2]; static uint8_t trailIdx = 0;

// errors / reply
static StrId errId = S_ERR_NOBRIDGE;
static Mood replyMood = Mood::HAPPY;
static std::vector<String> replyLines;
static bool replyHeFont = false; static bool replyRight = false;
static uint32_t replySince = 0;

// playback
static uint8_t* playBuf = nullptr; static size_t playLen = 0, playDataOff = 44, playDataLen = 0;
static uint32_t playRate = 16000, playStart = 0; static float mouthOpen = 0;

// ----------------------------------------------------------------------------
// Network job (runs in its own task so the face keeps animating)
// ----------------------------------------------------------------------------
struct NetJob {
  volatile uint8_t status = 0;     // 0 idle, 1 running, 2 ok, 3 error
  volatile bool orphan = false;
  uint8_t* wav = nullptr; size_t wavLen = 0;
  StrId err = S_ERR_BRIDGE;
  char text[160] = {0}, reply[240] = {0}, action[24] = {0}, audioUrl[200] = {0};
  uint8_t* audio = nullptr; size_t audioLen = 0;
};
static NetJob job;

static void* bigAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}

static bool downloadAudio(const char* urlIn) {
  String url = urlIn;
  if (url.startsWith("/")) {                       // relative -> bridge origin
    String b = BRIDGE_URL;
    int p = b.indexOf("://");
    int e = (p >= 0) ? b.indexOf('/', p + 3) : -1;
    url = (e > 0 ? b.substring(0, e) : b) + url;
  }
  bool https = url.startsWith("https");
  WiFiClient* cl = https ? new WiFiClientSecure() : new WiFiClient();
  if (https) ((WiFiClientSecure*)cl)->setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(8000);
  bool ok = false;
  uint8_t* buf = nullptr;
  if (http.begin(*cl, url) && http.GET() == 200) {
    int len = http.getSize();
    if (len <= (int)AUDIO_MAX_BYTES && len != 0) {
      size_t cap = len > 0 ? (size_t)len : (size_t)AUDIO_MAX_BYTES;
      buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);   // PSRAM only
      if (buf) {
        WiFiClient* s = http.getStreamPtr();
        size_t n = 0; uint32_t last = millis();
        while (n < cap && (http.connected() || s->available()) && millis() - last < 5000) {
          int a = s->available();
          if (a > 0) { n += s->readBytes(buf + n, min((size_t)a, cap - n)); last = millis(); }
          else vTaskDelay(5 / portTICK_PERIOD_MS);
        }
        bool tooBig = (len < 0 && n >= cap && s->available());
        if (!tooBig && n >= 44 && memcmp(buf, "RIFF", 4) == 0) {
          job.audio = buf; job.audioLen = n; ok = true;
        }
      }
    }
  }
  if (!ok && buf) heap_caps_free(buf);
  http.end();
  delete cl;
  return ok;
}

static StrId doRequest() {
  if (strlen(BRIDGE_URL) == 0) return S_ERR_NOBRIDGE;
  if (WiFi.status() != WL_CONNECTED) {
    if (strlen(WIFI_SSID) == 0) return S_ERR_NOWIFI;
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 6000) vTaskDelay(100 / portTICK_PERIOD_MS);
    if (WiFi.status() != WL_CONNECTED) return S_ERR_NOWIFI;
  }
  bool https = strncmp(BRIDGE_URL, "https", 5) == 0;
  WiFiClient* cl = https ? new WiFiClientSecure() : new WiFiClient();
  if (https) ((WiFiClientSecure*)cl)->setInsecure();
  HTTPClient http;
  http.setTimeout(BRIDGE_TIMEOUT);
  http.setConnectTimeout(8000);
  http.useHTTP10(true);                       // no chunked encoding
  StrId result = S_ERR_BRIDGE;
  if (http.begin(*cl, BRIDGE_URL)) {
    http.addHeader("Content-Type", "audio/wav");
    int code = http.POST(job.wav, job.wavLen);
    if (code == 200) {
      char* resp = (char*)bigAlloc(RESP_MAX_BYTES);
      if (!resp) {
        result = S_ERR_MEM;
      } else {
        WiFiClient* s = http.getStreamPtr();
        size_t n = 0; uint32_t last = millis();
        while (n < RESP_MAX_BYTES - 1 && (http.connected() || s->available()) && millis() - last < 5000) {
          int a = s->available();
          if (a > 0) { n += s->readBytes(resp + n, min((size_t)a, (size_t)(RESP_MAX_BYTES - 1 - n))); last = millis(); }
          else vTaskDelay(5 / portTICK_PERIOD_MS);
        }
        resp[n] = 0;
        JsonDocument doc;
        if (!deserializeJson(doc, resp, n)) {
          strlcpy(job.text, doc["text"] | "", sizeof(job.text));
          strlcpy(job.reply, doc["reply"] | "", sizeof(job.reply));
          strlcpy(job.action, doc["action"] | "", sizeof(job.action));
          strlcpy(job.audioUrl, doc["audio_url"] | "", sizeof(job.audioUrl));
          result = S_COUNT;                     // success marker
        }
        heap_caps_free(resp);
      }
    }
    http.end();
  }
  delete cl;
  if (result == S_COUNT && job.audioUrl[0] && !job.orphan) downloadAudio(job.audioUrl);   // failure => text only
  return result;
}

static void netTask(void*) {
  StrId r = doRequest();
  if (job.wav) { heap_caps_free(job.wav); job.wav = nullptr; }
  if (job.orphan) {
    if (job.audio) { heap_caps_free(job.audio); job.audio = nullptr; }
    job.status = 0;
  } else if (r == S_COUNT) {
    job.status = 2;
  } else {
    job.err = r;
    job.status = 3;
  }
  vTaskDelete(nullptr);
}

// ----------------------------------------------------------------------------
// Audio routing (Core2 mic and speaker share the I2S block)
// ----------------------------------------------------------------------------
static void audMic() {
  if (aud == Aud::MIC) return;
  M5.Speaker.end();
  M5.Mic.begin();
  aud = Aud::MIC; fi = 0; fprimed = 0;
  micGuardUntil = millis() + 250;
}
static void audSpk() {
  if (aud == Aud::SPK) return;
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(VOL_LV[cfg.vol]);
  aud = Aud::SPK;
}
static void beep(int freq, int ms) {
  if (VOL_LV[cfg.vol] == 0) return;
  audSpk();
  M5.Speaker.setVolume(VOL_LV[cfg.vol]);
  M5.Speaker.tone(freq, ms);
  uint32_t t = millis();
  while (M5.Speaker.isPlaying() && millis() - t < (uint32_t)ms + 200) delay(5);
  audMic();
}

// Returns true when a finished frame is available in *out.
static bool pollMic(const int16_t** out) {
  if (aud != Aud::MIC || !M5.Mic.isEnabled()) return false;
  if (!M5.Mic.record(fbuf[fi], FR, MIC_RATE)) { delay(1); return false; }
  fi = (fi + 1) % 3;
  if (fprimed < 3) { fprimed++; return false; }
  *out = fbuf[fi];                                  // oldest = finished buffer
  return true;
}

// ----------------------------------------------------------------------------
// Text helpers (Latin bitmap font or Hebrew VLW + rtlVisual)
// ----------------------------------------------------------------------------
static bool heFontLoaded = false;

static void setupFont(bool he, int sz) {
  if (he) {
#ifdef HAVE_HE_FONT
    if (!heFontLoaded) { cv.loadFont(font_he_vlw); heFontLoaded = true; }
    cv.setTextSize(sz >= 2 ? 1.0f : 0.8f);
    return;
#endif
  }
  if (heFontLoaded) { cv.unloadFont(); heFontLoaded = false; }
  cv.setFont(&fonts::Font0);
  cv.setTextSize(sz);
}

// Draws a logical string. UI-language HE or a Hebrew string => Hebrew font + rtlVisual.
static void say(const String& logical, int x, int y, Align al, int sz, uint16_t col, bool forceLatin = false) {
  bool he = !forceLatin && (heActive() || rtl::hasHebrew(logical.c_str()));
#ifndef HAVE_HE_FONT
  he = false;
#endif
  setupFont(he, sz);
  String v = (he && rtl::hasHebrew(logical.c_str())) ? rtlVisual(logical) : logical;
  cv.setTextColor(col);
  int w = cv.textWidth(v.c_str());
  int tx = (al == AL_L) ? x : (al == AL_C) ? x - w / 2 : x - w;
  cv.drawString(v.c_str(), tx, y);
}

static void bigNum(const char* s, int x, int y, uint16_t col) {
  if (heFontLoaded) { cv.unloadFont(); heFontLoaded = false; }
  cv.setFont(&fonts::Font7);
  cv.setTextSize(1);
  cv.setTextColor(col);
  cv.drawCenterString(s, x, y);
}

// Label/value row: EN label left + value right, HE mirrored.
static void row(int y, const String& label, const String& value, uint16_t colL, uint16_t colV) {
  if (heActive()) { say(label, 310, y, AL_R, 2, colL); say(value, 10, y, AL_L, 2, colV); }
  else            { say(label, 10, y, AL_L, 2, colL);  say(value, 310, y, AL_R, 2, colV); }
}

static String asciiOnly(const char* in) {
  String out; bool lastQ = false;
  for (const uint8_t* p = (const uint8_t*)in; *p; ++p) {
    if (*p >= 0x80) { if ((*p & 0xC0) != 0x80 && !lastQ) { out += '?'; lastQ = true; } continue; }
    lastQ = false;
    out += (*p < 32) ? ' ' : (char)*p;
  }
  return out;
}

// Word-wrap a logical string into lines (converted to visual order for Hebrew).
static void wrapText(const String& text, bool he, int maxW, int sz) {
  replyLines.clear();
  setupFont(he, sz);
  String line, word;
  auto flushWord = [&]() {
    if (!word.length()) return;
    String cand = line.length() ? line + " " + word : word;
    String probe = he ? rtlVisual(cand) : cand;
    if (cv.textWidth(probe.c_str()) > maxW && line.length()) {
      replyLines.push_back(he ? rtlVisual(line) : line);
      line = word;
    } else line = cand;
    word = "";
  };
  for (size_t i = 0; i < text.length(); i++) {
    char ch = text[i];
    if (ch == ' ' || ch == '\n') flushWord(); else word += ch;
  }
  flushWord();
  if (line.length()) replyLines.push_back(he ? rtlVisual(line) : line);
}

// ----------------------------------------------------------------------------
// Settings / trip persistence
// ----------------------------------------------------------------------------
static void saveSettings() {
  prefs.putUChar("br", cfg.bright); prefs.putUChar("vol", cfg.vol); prefs.putUChar("sens", cfg.sens);
  prefs.putUChar("theme", cfg.theme); prefs.putBool("vt", cfg.voice); prefs.putUChar("lang", g_lang);
}
static void saveTrip() {
  prefs.putUInt("ts", trip.secs); prefs.putUInt("brk", trip.brakes); prefs.putUInt("trn", trip.turns);
  prefs.putFloat("mg", trip.maxG); prefs.putFloat("ns", trip.noiseSum); prefs.putUInt("nn", trip.noiseN);
  tripDirty = false;
}
static void loadPrefs() {
  prefs.begin("dashie", false);
  cfg.bright = min<uint8_t>(prefs.getUChar("br", 1), 2);
  cfg.vol = min<uint8_t>(prefs.getUChar("vol", 2), 4);
  cfg.sens = min<uint8_t>(prefs.getUChar("sens", 1), 2);
  cfg.theme = min<uint8_t>(prefs.getUChar("theme", 0), 2);
  cfg.voice = prefs.getBool("vt", true);
  g_lang = min<uint8_t>(prefs.getUChar("lang", LANG_EN), 1);
  trip.secs = prefs.getUInt("ts", 0); trip.brakes = prefs.getUInt("brk", 0); trip.turns = prefs.getUInt("trn", 0);
  trip.maxG = prefs.getFloat("mg", 0); trip.noiseSum = prefs.getFloat("ns", 0); trip.noiseN = prefs.getUInt("nn", 0);
}

// ----------------------------------------------------------------------------
// State machine helpers
// ----------------------------------------------------------------------------
static void setSt(St s, uint32_t now) { st = s; stSince = now; }

static void stopPlayback() {
  if (M5.Speaker.isEnabled()) M5.Speaker.stop();
  if (playBuf) { heap_caps_free(playBuf); playBuf = nullptr; }
  mouthOpen = 0;
}

static void freeRec() { if (recBuf) { heap_caps_free(recBuf); recBuf = nullptr; } recN = 0; }

static void toIdle(uint32_t now) {
  stopPlayback();
  freeRec();
  if (aud != Aud::MIC) audMic();
  shout.holdoff(now, 1200);
  setSt(St::IDLE, now);
  lastActivity = now;
}

static void toError(StrId e, uint32_t now) {
  errId = e; freeRec();
  setSt(St::ERROR, now);
}

static void startListen(uint32_t now) {
  lastActivity = now;
  beep(1400, 50);
  now = millis();
  shout.holdoff(now, 3000);
  setSt(St::LISTEN, now);
}

static void writeWavHeader(uint8_t* b, uint32_t samples) {
  uint32_t dataBytes = samples * 2, riff = 36 + dataBytes, rate = MIC_RATE, byteRate = MIC_RATE * 2;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16; uint32_t fmtLen = 16;
  memcpy(b, "RIFF", 4); memcpy(b + 4, &riff, 4); memcpy(b + 8, "WAVEfmt ", 8);
  memcpy(b + 16, &fmtLen, 4); memcpy(b + 20, &fmt, 2); memcpy(b + 22, &ch, 2);
  memcpy(b + 24, &rate, 4); memcpy(b + 28, &byteRate, 4); memcpy(b + 32, &align, 2);
  memcpy(b + 34, &bits, 2); memcpy(b + 36, "data", 4); memcpy(b + 40, &dataBytes, 4);
}

static void startRecord(uint32_t now) {
  freeRec();
  recBuf = (uint8_t*)bigAlloc(44 + MAX_SAMP * 2);
  if (!recBuf) { toError(S_ERR_MEM, now); return; }
  recN = 0; speechSeen = false; recQuietMs = 0; recMs = 0;
  memset(wave, 0, sizeof(wave));
  setSt(St::RECORD, now);
}

static void startNet(uint32_t now) {
  if (job.status != 0) { toError(S_ERR_BUSY, now); return; }
  writeWavHeader(recBuf, recN);
  job.wav = recBuf; job.wavLen = 44 + recN * 2; recBuf = nullptr; recN = 0;
  job.orphan = false; job.audio = nullptr; job.audioLen = 0;
  job.text[0] = job.reply[0] = job.action[0] = job.audioUrl[0] = 0;
  job.status = 1;
  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
  setSt(St::THINK, now);
}

static void startReply(uint32_t now) {
  String r = job.reply[0] ? String(job.reply) : String(job.text);
  replyMood = Mood::HAPPY;
  if (strcmp(job.action, "face:angry") == 0) replyMood = Mood::ANGRY;
  else if (strcmp(job.action, "face:confused") == 0) replyMood = Mood::CONFUSED;
  else if (strcmp(job.action, "face:worried") == 0) replyMood = Mood::WORRIED;
  else if (strcmp(job.action, "face:happy") == 0) replyMood = Mood::HAPPY;
  bool hasHe = rtl::hasHebrew(r.c_str());
#ifdef HAVE_HE_FONT
  replyHeFont = hasHe;
#else
  replyHeFont = false;
  if (hasHe) { String a = asciiOnly(r.c_str()); a.replace("?", ""); a.trim(); r = a.length() ? a : String(ui(S_NONLATIN)); }
#endif
  if (!r.length()) r = ui(S_NOREPLY);
  replyRight = replyHeFont;
  wrapText(r, replyHeFont, 300, 2);
  replySince = now;

  // take ownership of downloaded audio and play it
  if (job.audio) {
    playBuf = job.audio; playLen = job.audioLen; job.audio = nullptr; job.audioLen = 0;
    playDataOff = 44; playDataLen = playLen > 44 ? playLen - 44 : 0; playRate = 16000;
    for (size_t i = 12; i + 8 < playLen && i < 200; i++) {            // find "data" chunk
      if (memcmp(playBuf + i, "data", 4) == 0) {
        uint32_t dl; memcpy(&dl, playBuf + i + 4, 4);
        playDataOff = i + 8; playDataLen = min<size_t>(dl, playLen - playDataOff); break;
      }
    }
    uint32_t sr; memcpy(&sr, playBuf + 24, 4); if (sr >= 8000 && sr <= 48000) playRate = sr;
    audSpk();
    M5.Speaker.setVolume(VOL_LV[cfg.vol]);
    M5.Speaker.playWav(playBuf, playLen);
    playStart = millis();
  }
  if (strcmp(job.action, "beep") == 0) beep(1800, 120);
  job.status = 0;
  setSt(St::REPLY, now);
}

// ----------------------------------------------------------------------------
// Input
// ----------------------------------------------------------------------------
static void changeSetting(uint32_t now) {
  switch (setSel) {
    case 0: cfg.bright = (cfg.bright + 1) % 3; lastBright = 255; break;
    case 1: cfg.vol = (cfg.vol + 1) % 5; break;
    case 2: cfg.sens = (cfg.sens + 1) % 3; break;
    case 3: cfg.theme = (cfg.theme + 1) % 3; break;
    case 4: cfg.voice = !cfg.voice; break;
    case 5: g_lang = (g_lang == LANG_EN) ? LANG_HE : LANG_EN; break;
  }
  saveSettings();
}

static void cancelCrash(uint32_t now) {
  crashRefract = now + 8000;
  toIdle(now);
}

static void onAction(char which, uint32_t now) {
  if (now - lastBtn < 150) return;
  lastBtn = now; lastActivity = now;
  switch (st) {
    case St::CRASH: cancelCrash(now); return;
    case St::ERROR: toIdle(now); return;
    case St::LISTEN: case St::RECORD: if (which == 'A' || which == 'T') toIdle(now); return;
    case St::REPLY: toIdle(now); return;
    case St::THINK: return;
    case St::IDLE: break;
  }
  if (which == 'B') { screen = (Screen)((screen + 1) % SC_COUNT); return; }
  if (which == 'A' || which == 'T') {
    if (screen == SC_SET) { if (which == 'A') setSel = (setSel + 1) % SET_ITEMS; return; }
    if (which == 'T' && screen != SC_FACE) return;
    startListen(now);
    return;
  }
  if (which == 'C') {
    switch (screen) {
      case SC_SET: changeSetting(now); break;
      case SC_NOISE: bestQuiet = 0; quietStreak = 0; memset(hist, 0, sizeof(hist)); break;
      case SC_G: trip.maxG = 0; break;
      case SC_TRIP:
        if (resetArmedAt && now - resetArmedAt < 3000) { trip = Trip(); saveTrip(); resetArmedAt = 0; }
        else resetArmedAt = now;
        break;
      default: break;
    }
  }
}

static void handleInput(uint32_t now) {
  if (M5.BtnA.wasPressed()) onAction('A', now);
  else if (M5.BtnB.wasPressed()) onAction('B', now);
  else if (M5.BtnC.wasPressed()) onAction('C', now);
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (t.y < 200) onAction('T', now);
    else if (t.y < 240) onAction(t.x < 107 ? 'A' : (t.x < 214 ? 'B' : 'C'), now);   // CoreS3 / safety
  }
}

// ----------------------------------------------------------------------------
// Audio frame processing
// ----------------------------------------------------------------------------
static void processFrame(const int16_t* f, uint32_t now) {
  if (now < micGuardUntil) return;
  int64_t sum = 0;
  for (size_t i = 0; i < FR; i++) sum += f[i];
  float mean = (float)sum / FR;
  double sq = 0;
  for (size_t i = 0; i < FR; i++) { float d = f[i] - mean; sq += d * d; }
  float rms = sqrtf((float)(sq / FR));
  dbNow = 20.0f * log10f(rms + 1.0f) + DB_OFFSET;
  dbSm += (dbNow - dbSm) * 0.25f;
  secSum += dbNow; secCnt++;

  float norm = constrain((dbNow - 30.0f) / 50.0f, 0.0f, 1.0f);
  wave[waveIdx] = norm; waveIdx = (waveIdx + 1) % 64;

  if (st == St::IDLE) {
    bool fire = false;
    if (cfg.voice) {
      float margin = SENS_DB[cfg.sens] + (driving ? 4.0f : 0.0f);
      fire = shout.update(dbNow, now, margin);
#ifdef USE_EI_KWS
      if (!fire && kwsInfer(f, FR)) fire = true;
#endif
    }
    if (fire) startListen(now);
  } else if (st == St::RECORD) {
    size_t n = min(FR, MAX_SAMP - recN);
    memcpy(((int16_t*)(recBuf + 44)) + recN, f, n * 2);
    recN += n; recMs += FRAME_MS;
    if (dbNow > shout.floorDb() + 8.0f) { speechSeen = true; recQuietMs = 0; }
    else recQuietMs += FRAME_MS;
    if (recN >= MAX_SAMP || (speechSeen && recQuietMs >= END_SILENCE_MS && recMs >= 500)) {
      startNet(now);
    } else if (!speechSeen && recMs >= 2500) {
      toError(S_ERR_NOCATCH, now);
    }
  }
}

// ----------------------------------------------------------------------------
// IMU
// ----------------------------------------------------------------------------
static void startCrash(uint32_t now) {
  stopPlayback(); freeRec();
  if (st == St::THINK) job.orphan = true;
  if (aud != Aud::MIC) audMic();
  crashStart = now; sosShown = false; lastCrashBeep = 0; lastActivity = now;
  setSt(St::CRASH, now);
}

static void pollImu(uint32_t now) {
  if (!M5.Imu.isEnabled() || now - lastImu < 15) return;
  lastImu = now;
  float ax = 0, ay = 0, az = 0;
  M5.Imu.getAccel(&ax, &ay, &az);
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  if (!gInit) { gX = ax; gY = ay; gZ = az; gInit = true; }
  gX += (ax - gX) * 0.002f; gY += (ay - gY) * 0.002f; gZ += (az - gZ) * 0.002f;   // gravity (tau ~7 s)
  float lx = ax - gX, ly = ay - gY, lz = az - gZ;
  // Assumed mounting: screen facing the driver, upright: X = right, Y = up, Z = towards driver (= backwards).
  latG = lx; fwdG = -lz;
  float lin = sqrtf(lx * lx + ly * ly + lz * lz);
  hfEma += (lin - hfEma) * 0.05f;
  if (!driving && hfEma > 0.04f) driving = true; else if (driving && hfEma < 0.025f) driving = false;

  trail[trailIdx][0] = latG; trail[trailIdx][1] = fwdG; trailIdx = (trailIdx + 1) % 24;
  float g2 = sqrtf(latG * latG + fwdG * fwdG);
  if (g2 > trip.maxG && g2 < 3.5f) { trip.maxG = g2; tripDirty = true; }

  if (!brakeArmed && fabsf(fwdG) < 0.2f && now > brakeRearm) brakeArmed = true;
  if (fabsf(fwdG) >= 0.2f) brakeRearm = now + 1000;
  if (!turnArmed && fabsf(latG) < 0.2f && now > turnRearm) turnArmed = true;
  if (fabsf(latG) >= 0.2f) turnRearm = now + 1000;
  if (brakeArmed && fwdG < -BRAKE_G) { brakeArmed = false; trip.brakes++; tripDirty = true; brakeFlashUntil = now + 2000; lastActivity = now; }
  if (turnArmed && fabsf(latG) > TURN_G) { turnArmed = false; trip.turns++; tripDirty = true; lastActivity = now; }

  // crash: |a| > 4 g for >= 30 ms
  if (mag > CRASH_G) {
    if (!crashHigh) { crashHigh = true; crashHighSince = now; }
    else if (now - crashHighSince >= 30 && st != St::CRASH && now > crashRefract) startCrash(now);
  } else crashHigh = false;
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------
static const FaceColors& col() { return THEMES[cfg.theme]; }

static void bottomBar(StrId a, StrId b, StrId c) {
  const FaceColors& k = col();
  cv.drawFastHLine(0, 214, 320, k.acc);
  say(ui(a), 53, 219, AL_C, 2, k.fg);
  say(ui(b), 160, 219, AL_C, 2, k.fg);
  say(ui(c), 267, 219, AL_C, 2, k.fg);
}

static Mood noiseMood() {
  if (dbSm > 75) return Mood::ANGRY;
  if (dbSm > 60) return Mood::WORRIED;
  if (quietStreak > 30) return Mood::HAPPY;
  return Mood::NEUTRAL;
}

static void clockStr(char* out, size_t n) {
  time_t t = time(nullptr);
  if (t > 1700000000) { struct tm tmv; localtime_r(&t, &tmv); snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min); }
  else snprintf(out, n, "--:--");
}

static void renderScreen(uint32_t now) {
  const FaceColors& k = col();
  char b[48];
  switch (screen) {
    case SC_FACE: {
      Mood m = (now < brakeFlashUntil) ? Mood::WORRIED : noiseMood();
      drawFace(cv, 160, 100, 1.0f, m, now, k);
      clockStr(b, sizeof(b));
      say(b, 312, 6, AL_R, 2, k.fg, true);
      say(WiFi.status() == WL_CONNECTED ? "Wi-Fi" : "-", 8, 6, AL_L, 1, WiFi.status() == WL_CONNECTED ? k.acc : k.fg, true);
      if (cfg.voice) cv.fillCircle(12, 26, 4, k.acc); else cv.drawCircle(12, 26, 4, k.fg);
      if (now < brakeFlashUntil) say(ui(S_WHOA), 160, 186, AL_C, 2, k.acc);
      bottomBar(S_TALK, S_NEXT, S_OPT);
      break;
    }
    case SC_NOISE: {
      say(ui(S_NOISE), heActive() ? 312 : 8, 6, heActive() ? AL_R : AL_L, 2, k.fg);
      drawFace(cv, 62, 100, 0.42f, noiseMood(), now, k);
      snprintf(b, sizeof(b), "%d", (int)(dbSm + 0.5f));
      bigNum(b, 205, 38, dbSm > 75 ? rgb(255, 80, 60) : (dbSm > 60 ? rgb(255, 200, 0) : k.fg));
      say("dB", 296, 78, AL_L, 2, k.acc, true);
      snprintf(b, sizeof(b), "%s %lus  (%lus)", ui(S_QUIET), (unsigned long)quietStreak, (unsigned long)bestQuiet);
      say(b, 160, 104, AL_C, 1, k.fg);
      int base = 205;
      cv.drawFastHLine(10, base, 300, k.acc);
      for (int i = 0; i < 60; i++) {
        float v = hist[(histIdx + i) % 60];
        int h = constrain((int)((v - 30.0f) * 1.3f), 0, 90);
        uint16_t c2 = v > 75 ? rgb(255, 80, 60) : (v > 60 ? rgb(255, 200, 0) : k.acc);
        if (v > 1) cv.fillRect(10 + i * 5, base - h, 4, h, c2);
      }
      bottomBar(S_TALK, S_NEXT, S_RESET);
      break;
    }
    case SC_G: {
      int cx = 160, cy = 108, R = 84;
      cv.drawCircle(cx, cy, R, k.acc); cv.drawCircle(cx, cy, R / 2, k.fg);
      cv.drawFastHLine(cx - R, cy, 2 * R, k.fg); cv.drawFastVLine(cx, cy - R, 2 * R, k.fg);
      for (int i = 0; i < 24; i++) {
        int idx = (trailIdx + i) % 24;
        int tx = cx + (int)(constrain(trail[idx][0], -1.0f, 1.0f) * R);
        int ty = cy - (int)(constrain(trail[idx][1], -1.0f, 1.0f) * R);
        cv.fillCircle(tx, ty, 1 + i / 8, k.fg);
      }
      int bx = cx + (int)(constrain(latG, -1.0f, 1.0f) * R), by = cy - (int)(constrain(fwdG, -1.0f, 1.0f) * R);
      uint16_t bc = (now < brakeFlashUntil) ? rgb(255, 60, 40) : k.acc;
      cv.fillCircle(bx, by, 9, bc);
      snprintf(b, sizeof(b), "%.2fg", trip.maxG);
      say(ui(S_MAXG), heActive() ? 312 : 8, 6, heActive() ? AL_R : AL_L, 1, k.fg);
      say(b, heActive() ? 312 : 8, 18, heActive() ? AL_R : AL_L, 2, k.acc, true);
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.brakes);
      say(ui(S_BRAKES), heActive() ? 8 : 312, 6, heActive() ? AL_L : AL_R, 1, k.fg);
      say(b, heActive() ? 8 : 312, 18, heActive() ? AL_L : AL_R, 2, k.acc, true);
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.turns);
      say(ui(S_TURNS), heActive() ? 8 : 312, 44, heActive() ? AL_L : AL_R, 1, k.fg);
      say(b, heActive() ? 8 : 312, 56, heActive() ? AL_L : AL_R, 2, k.acc, true);
      bottomBar(S_TALK, S_NEXT, S_RESET);
      break;
    }
    case SC_TRIP: {
      uint32_t s = trip.secs;
      snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
      row(14, ui(S_TIME), b, k.fg, k.acc);
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.brakes);
      row(46, ui(S_BRAKES), b, k.fg, k.acc);
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.turns);
      row(78, ui(S_TURNS), b, k.fg, k.acc);
      snprintf(b, sizeof(b), "%.2f g", trip.maxG);
      row(110, ui(S_MAXG), b, k.fg, k.acc);
      snprintf(b, sizeof(b), "%d dB", trip.noiseN ? (int)(trip.noiseSum / trip.noiseN + 0.5f) : 0);
      row(142, ui(S_AVGNOISE), b, k.fg, k.acc);
      snprintf(b, sizeof(b), "%lu s", (unsigned long)bestQuiet);
      row(174, ui(S_BESTQUIET), b, k.fg, k.acc);
      if (resetArmedAt && now - resetArmedAt < 3000) say(ui(S_PRESS_AGAIN), 160, 196, AL_C, 1, rgb(255, 120, 80));
      bottomBar(S_TALK, S_NEXT, S_RESET);
      break;
    }
    case SC_SET: {
      static const StrId labels[SET_ITEMS] = {S_BRIGHT, S_VOLUME, S_SENS, S_THEME, S_VOICE, S_LANG};
      for (int i = 0; i < SET_ITEMS; i++) {
        int y = 10 + i * 33;
        if (i == setSel) cv.drawRoundRect(2, y - 5, 316, 30, 6, k.acc);
        String v;
        switch (i) {
          case 0: v = String(BRIGHT_LV[cfg.bright] * 100 / 255) + "%"; break;
          case 1: v = String(VOL_LV[cfg.vol] * 100 / 255) + "%"; break;
          case 2: v = ui((StrId)(S_LOW + cfg.sens)); break;
          case 3: v = ui((StrId)(S_CYAN + cfg.theme)); break;
          case 4: v = ui(cfg.voice ? S_ON : S_OFF); break;
          case 5:
#ifdef HAVE_HE_FONT
            v = ui(g_lang == LANG_HE ? S_LANG_HE : S_LANG_EN);
#else
            v = g_lang == LANG_HE ? kStrings[S_LANG_HE_NOFONT].en : kStrings[S_LANG_EN].en;
#endif
            break;
        }
        row(y, ui(labels[i]), v, i == setSel ? k.acc : k.fg, k.fg);
      }
      bottomBar(S_NEXT, S_NEXT, S_OPT);
      break;
    }
    default: break;
  }
}

static void renderOverlay(uint32_t now) {
  const FaceColors& k = col();
  char b[48];
  switch (st) {
    case St::LISTEN:
      drawFace(cv, 160, 96, 1.0f, Mood::LISTEN, now, k);
      say(ui(S_LISTENING), 160, 196, AL_C, 2, k.acc);
      break;
    case St::RECORD: {
      drawFace(cv, 160, 52, 0.5f, Mood::LISTEN, now, k);
      for (int i = 0; i < 64; i++) {
        float v = wave[(waveIdx + i) % 64];
        int h = 2 + (int)(v * 70);
        cv.fillRect(8 + i * 5, 150 - h / 2, 3, h, speechSeen ? k.acc : k.fg);
      }
      snprintf(b, sizeof(b), "%.1fs", recMs / 1000.0f);
      say(ui(S_RECORDING), 8, 196, AL_L, 2, k.fg);
      say(b, 312, 196, AL_R, 2, k.acc, true);
      break;
    }
    case St::THINK:
      drawFace(cv, 160, 96, 1.0f, Mood::THINK, now, k);
      say(ui(S_THINKING), 160, 196, AL_C, 2, k.acc);
      break;
    case St::REPLY: {
      bool speaking = playBuf && M5.Speaker.isPlaying();
      drawFace(cv, 160, 46, 0.5f, speaking ? Mood::TALK : replyMood, now, k, mouthOpen);
      int lineH = replyHeFont || heActive() ? 24 : 18;
      int vis = (210 - 100) / lineH;
      int total = (int)replyLines.size();
      int scroll = 0;
      if (total > vis) scroll = constrain((int)((now - replySince - 1500) / 1800), 0, total - vis);
      for (int i = 0; i < vis && scroll + i < total; i++) {
        bool he = replyHeFont;
        setupFont(he, 2);
        cv.setTextColor(k.fg);
        const String& ln = replyLines[scroll + i];
        int x = replyRight ? 312 - cv.textWidth(ln.c_str()) : 8;
        cv.drawString(ln.c_str(), x, 100 + i * lineH);
      }
      break;
    }
    case St::ERROR:
      drawFace(cv, 160, 90, 0.8f, Mood::CONFUSED, now, k);
      say(ui(errId), 160, 190, AL_C, 2, rgb(255, 160, 80));
      break;
    default: break;
  }
}

static void renderCrash(uint32_t now) {
  bool flash = (now / 400) % 2;
  cv.fillScreen(flash ? rgb(200, 0, 0) : rgb(110, 0, 0));
  uint16_t w = rgb(255, 255, 255);
  int left = CRASH_COUNTDOWN_S - (int)((now - crashStart) / 1000);
  say(ui(S_IMPACT), 160, 20, AL_C, 2, w);
  if (left > 0) {
    say(ui(S_AREYOUOK), 160, 56, AL_C, 2, w);
    char b[8]; snprintf(b, sizeof(b), "%d", left);
    bigNum(b, 160, 96, w);
    say(ui(S_TAPCANCEL), 160, 190, AL_C, 2, w);
  } else {
    say(ui(S_SOS), 160, 100, AL_C, 2, w);
    say(ui(S_TAPCANCEL), 160, 190, AL_C, 2, w);
  }
}

static void applyBrightness(uint32_t now) {
  uint8_t want = BRIGHT_LV[cfg.bright];
  if (st == St::IDLE && now - lastActivity > IDLE_DIM_MS) want = min<uint8_t>(want, 24);
  if (want != lastBright) { M5.Display.setBrightness(want); lastBright = want; }
}

// ----------------------------------------------------------------------------
// Setup / loop
// ----------------------------------------------------------------------------
void setup() {
  auto mcfg = M5.config();
  M5.begin(mcfg);
  loadPrefs();
  M5.Display.setBrightness(BRIGHT_LV[cfg.bright]);

  cv.setPsram(true);
  cv.setColorDepth(16);
  if (!cv.createSprite(M5.Display.width(), M5.Display.height())) {
    cv.setPsram(false);
    cv.createSprite(M5.Display.width(), M5.Display.height());
  }
  memset(hist, 0, sizeof(hist));
  memset(trail, 0, sizeof(trail));
  randomSeed(esp_random());

  if (strlen(WIFI_SSID)) { WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.begin(WIFI_SSID, WIFI_PASS); }
  configTzTime(TZ_STRING, "pool.ntp.org");

  M5.Speaker.end();
  audMic();
  uint32_t now = millis();
  shout.holdoff(now, 2000);
  lastActivity = now; lastSec = now; lastSave = now;
}

void loop() {
  M5.update();
  uint32_t now = millis();
  handleInput(now);

  const int16_t* f = nullptr;
  bool got = pollMic(&f);
  if (got) processFrame(f, now);
  pollImu(now);

  // ---- per-state timers
  switch (st) {
    case St::LISTEN: if (now - stSince >= 450) startRecord(now); break;
    case St::THINK:
      if (job.status == 2) startReply(now);
      else if (job.status == 3) { StrId e = job.err; job.status = 0; toError(e, now); }
      else if (now - stSince > 20000) { job.orphan = true; toError(S_ERR_BRIDGE, now); }
      break;
    case St::REPLY: {
      bool speaking = playBuf && M5.Speaker.isPlaying();
      if (speaking) {
        // mouth follows the loudness of the audio being played
        size_t idx = (size_t)(now - playStart) * playRate / 1000;
        size_t total = playDataLen / 2;
        const int16_t* pcm = (const int16_t*)(playBuf + playDataOff);
        double s2 = 0; int n = 0;
        for (size_t i = idx; i < idx + 240 && i < total; i++) { s2 += (double)pcm[i] * pcm[i]; n++; }
        float rmsv = n ? sqrtf((float)(s2 / n)) : 0;
        float target = constrain(rmsv / 5000.0f, 0.0f, 1.0f);
        mouthOpen += (target - mouthOpen) * 0.5f;
      } else {
        mouthOpen = 0;
        if (playBuf) { stopPlayback(); audMic(); }
        if (now - stSince >= 8000) toIdle(now);
      }
      break;
    }
    case St::ERROR: if (now - stSince >= 3000) toIdle(now); break;
    case St::CRASH: {
      int left = CRASH_COUNTDOWN_S - (int)((now - crashStart) / 1000);
      if (now - lastCrashBeep >= 1000) {
        lastCrashBeep = now;
        beep(left > 0 ? 1500 : 2200, left > 0 ? 80 : 300);
      }
      break;
    }
    default: break;
  }

  // ---- once per second
  if (now - lastSec >= 1000) {
    lastSec += 1000;
    if (now - lastSec > 2000) lastSec = now;
    trip.secs++; tripDirty = true;
    if (secCnt) {
      float avg = secSum / secCnt;
      hist[histIdx] = avg; histIdx = (histIdx + 1) % 60;
      trip.noiseSum += avg; trip.noiseN++;
      if (avg < 60.0f) { quietStreak++; if (quietStreak > bestQuiet) bestQuiet = quietStreak; } else quietStreak = 0;
    }
    secSum = 0; secCnt = 0;
  }
  if (tripDirty && now - lastSave >= 30000) { saveTrip(); lastSave = now; }

  // ---- render (rate-limited)
  static uint32_t lastDraw = 0;
  uint32_t interval = (st == St::RECORD) ? 60 : 33;
  if (now - lastDraw >= interval) {
    lastDraw = now;
    applyBrightness(now);
    cv.fillScreen(col().bg);
    if (st == St::CRASH) renderCrash(now);
    else if (st == St::IDLE) renderScreen(now);
    else renderOverlay(now);
    cv.pushSprite(0, 0);
  }

  if (!got) delay(1);
}
