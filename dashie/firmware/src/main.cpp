// Dashie - in-car voice buddy for M5Stack Core2 / CoreS3 / StickC Plus2 (M5Unified)
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
#include "layout.h"
#include "shift.h"
#include "bridge_net.h"
#include "wifi_mgr.h"
#include "hub.h"
#ifdef USE_A2DP_OUT
#include "a2dp_out.h"
#endif

#ifdef HAVE_HE_FONT
#include "font_he.h"   // must define: const uint8_t font_he_vlw[] PROGMEM = {...};
#endif

// ----------------------------------------------------------------------------
// Types & globals
// ----------------------------------------------------------------------------
enum class St : uint8_t { IDLE, LISTEN, RECORD, THINK, REPLY, ERROR, CRASH, NOTE, SQ, SWAIT, PALERT, FIND };   // NOTE/SQ/SWAIT: shift mode; PALERT/FIND: hub (pocket device)
#define HUB_SCREEN (HUB_ENABLED && !HUB_ROLE_POCKET)    // car device: extra "Pocket device" status screen
enum Screen : uint8_t {
  SC_FACE, SC_NOISE, SC_G, SC_TRIP,
#if SHIFT_ENABLED
  SC_SHIFT,
#endif
#if HUB_SCREEN
  SC_PEER,
#endif
  SC_SET, SC_COUNT
};
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

struct Settings { uint8_t bright = 1, vol = 2, sens = 1, theme = 0; bool voice = true; bool recipe = false; } cfg;
struct Trip { uint32_t secs = 0, brakes = 0, turns = 0, noiseN = 0; float maxG = 0, noiseSum = 0; } trip;
#ifdef USE_A2DP_OUT
static constexpr int SET_ITEMS = 8;    // + "Button asks" + "Test kit"
#else
static constexpr int SET_ITEMS = 7;    // + "Button asks" (assistant / recipe book)
#endif

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
static bool errInfo = false;                     // true = friendly info (neutral face) instead of an error
static Mood replyMood = Mood::HAPPY;
static std::vector<String> replyLines;
static bool replyHeFont = false; static bool replyRight = false;
static uint32_t replySince = 0;
static bool askRecipes = false;                  // the current question goes to the recipe book (X-Mode: recipes)
static bool pttHeld = false;                     // recipe PTT: recording while the button is held down
static uint32_t pttUpSince = 0, pttVibOff = 0;
static uint32_t thinkTimeoutMs = 20000;          // THINK gives up after this long (shift summary: longer)
static uint32_t replyHoldMs = 8000;              // how long REPLY stays after the audio ended (long summaries: more)
static String shiftReplyText;                    // set by shift mode: text to show instead of job.reply

// playback
static uint8_t* playBuf = nullptr; static size_t playLen = 0, playDataOff = 44, playDataLen = 0;
static uint32_t playRate = 16000, playStart = 0; static float mouthOpen = 0;
#ifdef USE_A2DP_OUT
static bool viaBt = false;                       // current reply is going out over A2DP
static uint8_t* deferFree = nullptr;             // buffer still read by the A2DP task; freed when it ends
static bool testRunning = false, btPicking = false; static a2o::Res testRes = a2o::Res::NONE; static int pickSel = 0;
#endif

// "Is a reply being played (keep the buffer, stay in REPLY)?" vs "is sound coming out right now (mouth)?"
static bool audioActive() {
#ifdef USE_A2DP_OUT
  if (viaBt) return a2o::busy();
#endif
  return M5.Speaker.isPlaying();
}
static bool audioSounding() {
#ifdef USE_A2DP_OUT
  if (viaBt) return a2o::streaming();
#endif
  return M5.Speaker.isPlaying();
}

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
  bool recipes = false;            // ask the recipe book (X-Mode: recipes)
  char bookStatus[16] = {0};       // bridge's recipe-book state: pending / blocked / unknown / unconfigured / error
};
static NetJob job;

static void* bigAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}

static bool fetchWav(const char* urlIn, uint8_t** outBuf, size_t* outLen) {
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
  const String bb = bridge::base();
  const bool begun = http.begin(*cl, url);
  if (begun && bb.length() && url.startsWith(bb)) bridge::addAuth(http);    // same origin as the bridge only
  if (begun && http.GET() == 200) {
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
          *outBuf = buf; *outLen = n; ok = true;
        }
      }
    }
  }
  if (!ok && buf) heap_caps_free(buf);
  http.end();
  delete cl;
  return ok;
}

static bool downloadAudio(const char* urlIn) { return fetchWav(urlIn, &job.audio, &job.audioLen); }

// Shift summary: the bridge speaks it as several WAV chunks (audio_urls). Merge them into ONE 16-bit mono buffer so the
// A2DP kit is connected only once. Chunks that do not fit SHIFT_AUDIO_MAX_BYTES (or fail to download) are left out.
static bool fetchMerged(const std::vector<String>& urls, uint8_t** outBuf, size_t* outLen) {
  const size_t cap = SHIFT_AUDIO_MAX_BYTES;
  uint8_t* m = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!m) return false;
  size_t pos = 44; uint32_t rate = 0;
  for (const String& u : urls) {
    uint8_t* b = nullptr; size_t n = 0;
    if (!fetchWav(u.c_str(), &b, &n)) break;                       // keep what we have
    uint16_t ch = 0, bits = 0; uint32_t sr = 0;
    memcpy(&ch, b + 22, 2); memcpy(&sr, b + 24, 4); memcpy(&bits, b + 34, 2);
    size_t off = 44, dl = n > 44 ? n - 44 : 0;
    for (size_t i = 12; i + 8 < n && i < 200; i++) {
      if (memcmp(b + i, "data", 4) == 0) { uint32_t d; memcpy(&d, b + i + 4, 4); off = i + 8; dl = min<size_t>(d, n - off); break; }
    }
    bool fits = ch == 1 && bits == 16 && sr >= 8000 && sr <= 48000 && (rate == 0 || sr == rate) && pos + dl <= cap;
    if (fits) { memcpy(m + pos, b + off, dl); pos += dl; rate = sr; }
    heap_caps_free(b);
    if (!fits) break;
  }
  if (pos <= 44) { heap_caps_free(m); return false; }
  uint32_t dataBytes = (uint32_t)(pos - 44), riff = dataBytes + 36, byteRate = rate * 2, fmtLen = 16;
  uint16_t fmt = 1, ch1 = 1, align = 2, bits16 = 16;
  memcpy(m, "RIFF", 4); memcpy(m + 4, &riff, 4); memcpy(m + 8, "WAVEfmt ", 8);
  memcpy(m + 16, &fmtLen, 4); memcpy(m + 20, &fmt, 2); memcpy(m + 22, &ch1, 2);
  memcpy(m + 24, &rate, 4); memcpy(m + 28, &byteRate, 4); memcpy(m + 32, &align, 2);
  memcpy(m + 34, &bits16, 2); memcpy(m + 36, "data", 4); memcpy(m + 40, &dataBytes, 4);
  *outBuf = m; *outLen = pos;
  return true;
}

static StrId doRequest() {
  if (strlen(BRIDGE_URL) == 0) return S_ERR_NOBRIDGE;
  if (WiFi.status() != WL_CONNECTED) {
    if (wifimgr::configured() == 0) return S_ERR_NOWIFI;
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
    bridge::addAuth(http);                              // Bearer token (BRIDGE_TOKEN), if configured
    if (hub::deviceId()[0]) http.addHeader("X-Device-Id", hub::deviceId());   // lets the bridge route hub voice commands
    if (job.recipes) http.addHeader("X-Mode", "recipes");                      // force the recipe book (otherwise the bridge guesses from the words)
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
          strlcpy(job.bookStatus, doc["status"] | "", sizeof(job.bookStatus));
          result = S_COUNT;                     // success marker
          if (job.bookStatus[0]) {              // recipe book not ready: show a short local message instead of the bridge's text
            if (!strcmp(job.bookStatus, "pending")) result = S_RCP_WAIT;
            else if (!strcmp(job.bookStatus, "blocked")) result = S_RCP_BLOCKED;
            else if (!strcmp(job.bookStatus, "unknown") || !strcmp(job.bookStatus, "unconfigured")) result = S_RCP_SETUP;
            else if (!strcmp(job.bookStatus, "error")) result = S_RCP_DOWN;
          }
        }
        heap_caps_free(resp);
      }
    }
    http.end();
  }
  delete cl;
#if defined(DASHIE_NO_SPEAKER) && !defined(USE_A2DP_OUT)
  // buzzer-only board without A2DP: nothing could play the audio, so do not download it
#else
  if (result == S_COUNT && job.audioUrl[0] && !job.orphan) downloadAudio(job.audioUrl);   // failure => text only
#endif
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

static int textW(const String& logical, int sz) {      // width `say` would draw (same font / RTL logic)
  bool he = heActive() || rtl::hasHebrew(logical.c_str());
#ifndef HAVE_HE_FONT
  he = false;
#endif
  setupFont(he, sz);
  String v = (he && rtl::hasHebrew(logical.c_str())) ? rtlVisual(logical) : logical;
  return cv.textWidth(v.c_str());
}

// Label/value row: EN label left + value right, HE mirrored. On the compact screen a row that
// would overflow is drawn in the small font instead.
static void row(int y, const String& label, const String& value, uint16_t colL, uint16_t colV) {
  int sz = 2, xl = L.rowMargin, xr = L.W - L.rowMargin;
  if (L.compact && textW(label, 2) + textW(value, 2) + 8 > xr - xl) { sz = 1; y += 4; }
  if (heActive()) { say(label, xr, y, AL_R, sz, colL); say(value, xl, y, AL_L, sz, colV); }
  else            { say(label, xl, y, AL_L, sz, colL);  say(value, xr, y, AL_R, sz, colV); }
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
  prefs.putUChar("theme", cfg.theme); prefs.putBool("vt", cfg.voice); prefs.putUChar("lang", g_lang); prefs.putBool("rcp", cfg.recipe);
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
  cfg.recipe = prefs.getBool("rcp", false);
  g_lang = min<uint8_t>(prefs.getUChar("lang", LANG_EN), 1);
  trip.secs = prefs.getUInt("ts", 0); trip.brakes = prefs.getUInt("brk", 0); trip.turns = prefs.getUInt("trn", 0);
  trip.maxG = prefs.getFloat("mg", 0); trip.noiseSum = prefs.getFloat("ns", 0); trip.noiseN = prefs.getUInt("nn", 0);
}

// ----------------------------------------------------------------------------
// State machine helpers
// ----------------------------------------------------------------------------
static void setSt(St s, uint32_t now) { st = s; stSince = now; }

static void stopPlayback() {
#ifdef USE_A2DP_OUT
  if (viaBt || a2o::busy()) {                    // the A2DP task still reads playBuf: free it after it ends
    a2o::abort(); viaBt = false;
    if (playBuf) { if (deferFree) heap_caps_free(deferFree); deferFree = playBuf; playBuf = nullptr; }
    mouthOpen = 0;
    return;
  }
#endif
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
  errId = e; errInfo = false; freeRec();
  setSt(St::ERROR, now);
}
static void toInfo(StrId e, uint32_t now) {      // same screen as an error, but a calm face
  errId = e; errInfo = true; freeRec();
  setSt(St::ERROR, now);
}

static void startListen(uint32_t now) {
  lastActivity = now;
  askRecipes = cfg.recipe;
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

// Recipe PTT: hold the front button (Core2: BtnA touch zone, Plus2: BtnA) while asking, release to send.
// No beep (the speaker would take the mic away): a short vibration on Core2, the screen changes on Plus2.
static void startPtt(uint32_t now) {
  lastActivity = now;
  askRecipes = true; pttHeld = true; pttUpSince = 0;
  shout.holdoff(now, 3000);
  if (M5.getBoard() == m5::board_t::board_M5StackCore2) { M5.Power.setVibration(180); pttVibOff = now + 70; }
  startRecord(now);
  if (st != St::RECORD) pttHeld = false;         // allocation failed: startRecord already showed the error
}

static void startNet(uint32_t now) {
  if (job.status != 0) { toError(S_ERR_BUSY, now); return; }
  writeWavHeader(recBuf, recN);
  job.wav = recBuf; job.wavLen = 44 + recN * 2; recBuf = nullptr; recN = 0;
  job.orphan = false; job.audio = nullptr; job.audioLen = 0;
  job.text[0] = job.reply[0] = job.action[0] = job.audioUrl[0] = job.bookStatus[0] = 0;
  job.recipes = askRecipes;
  thinkTimeoutMs = 20000;
  job.status = 1;
  xTaskCreatePinnedToCore(netTask, "net", 20480, nullptr, 1, nullptr, 0);
  setSt(St::THINK, now);
}

// Takes ownership of a downloaded WAV (16-bit mono) and plays it: A2DP kit first, internal speaker as fallback.
static void startPlayback(uint8_t* buf, size_t len, bool allowBt = true) {
  playBuf = buf; playLen = len;
  playDataOff = 44; playDataLen = playLen > 44 ? playLen - 44 : 0; playRate = 16000;
  for (size_t i = 12; i + 8 < playLen && i < 200; i++) {            // find "data" chunk
    if (memcmp(playBuf + i, "data", 4) == 0) {
      uint32_t dl; memcpy(&dl, playBuf + i + 4, 4);
      playDataOff = i + 8; playDataLen = min<size_t>(dl, playLen - playDataOff); break;
    }
  }
  uint32_t sr; memcpy(&sr, playBuf + 24, 4); if (sr >= 8000 && sr <= 48000) playRate = sr;
  bool onBt = false;
#ifdef USE_A2DP_OUT
  uint16_t ch, bits; memcpy(&ch, playBuf + 22, 2); memcpy(&bits, playBuf + 34, 2);
  if (allowBt && ch == 1 && bits == 16 && playDataLen >= 3200 && !a2o::busy())
    onBt = viaBt = a2o::play((const int16_t*)(playBuf + playDataOff), playDataLen / 2, playRate);
#endif
  if (!onBt) {
#ifdef DASHIE_NO_SPEAKER
    heap_caps_free(playBuf); playBuf = nullptr;       // buzzer only: text-only reply
#else
    audSpk();
    M5.Speaker.setVolume(VOL_LV[cfg.vol]);
    M5.Speaker.playWav(playBuf, playLen);
    playStart = millis();
#endif
  }
}

static void startReply(uint32_t now) {
  String r = shiftReplyText.length() ? shiftReplyText : (job.reply[0] ? String(job.reply) : String(job.text));
  const bool longText = shiftReplyText.length() > 0 || job.recipes; shiftReplyText = "";   // long answers scroll: keep them up longer
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
  wrapText(r, replyHeFont, L.wrapW, 2);
  replySince = now;
  replyHoldMs = 8000;
  if (longText) {                                  // shift summary: keep it on screen while it scrolls
    int lh = (replyHeFont || heActive()) ? L.replyLineHe : L.replyLineLatin;
    int extra = (int)replyLines.size() - (L.replyBottom - L.replyTextY) / lh;
    if (extra > 0) replyHoldMs = 8000 + (uint32_t)extra * 1800;
  }

  // take ownership of downloaded audio and play it
  if (job.audio) {
    uint8_t* a = job.audio; size_t an = job.audioLen; job.audio = nullptr; job.audioLen = 0;
    startPlayback(a, an);
  }
  if (strcmp(job.action, "beep") == 0) beep(1800, 120);
  job.status = 0;
  setSt(St::REPLY, now);
}

// ----------------------------------------------------------------------------
// Shift mode UI (logic, storage and uploads: shift.cpp). Compiled out with SHIFT_ENABLED=0.
// Input scheme: 'N' = note button click, 'E' = note button hold (end shift / skip), 'H' = hold on the Shift screen (start).
//   Core2/CoreS3: the physical side button (BtnPWR). Plus2: BtnA. While a shift runs and the screen is dark
//   ("pocket mode") the touch zones are ignored, so only the physical button works.
// ----------------------------------------------------------------------------
#if SHIFT_ENABLED
enum class SqMode : uint8_t { CONFIRM, QUESTIONS };
static SqMode sqMode = SqMode::CONFIRM;
static std::vector<String> sqQ;
static int sqIdx = 0;
static bool sqFollow = false;                    // true = follow-up questions from the bridge (need_info)
static St noteReturn = St::IDLE;
static uint32_t swaitSince = 0, confirmSince = 0;
static bool haveVib = false, warnedWrite = false, warnedSum = false;
static String hapSeq; static uint8_t hapIdx = 0; static uint32_t hapNext = 0, vibOff = 0;
static int battPct = -1; static uint32_t lastBattPoll = 0, lastWarn = 0;
#if SHIFT_SPOKEN_QUESTIONS
static volatile uint8_t qState = 0;              // question audio: 0 none, 1 fetching, 2 ready, 3 failed
static uint8_t* qBuf = nullptr; static size_t qLen = 0; static char qUrl[200]; static int qFor = -1;
#endif
static std::vector<String> sumUrls;              // spoken summary chunks (audio_urls) of the summary being fetched

static bool shiftPocket(uint32_t now) {
  return shift::active() && st != St::CRASH && st != St::PALERT && st != St::FIND && now - lastActivity > SHIFT_POCKET_MS;
}

// Feedback that works in a pocket: vibration on Core2, short beeps on buzzer boards ('s' short, 'l' long, '.' pause).
static void haptic(const char* pat) {
  if (haveVib) { hapSeq = pat; hapIdx = 0; hapNext = 0; return; }
  for (const char* q = pat; *q; ++q) {
    if (*q == 's') beep(2200, 70); else if (*q == 'l') beep(1500, 260); else delay(120);
  }
}
static void hapticTick(uint32_t now) {
  if (vibOff && now >= vibOff) { M5.Power.setVibration(0); vibOff = 0; }
  if (!haveVib || hapIdx >= hapSeq.length() || now < hapNext || vibOff) return;
  char c = hapSeq[hapIdx++];
  if (c == 's' || c == 'l') { M5.Power.setVibration(200); vibOff = now + (c == 's' ? 90 : 320); hapNext = vibOff + 120; }
  else hapNext = now + 160;
}

static String safeText(const String& in, bool& he) {   // Hebrew without a font => ASCII part only
  String r = in;
  bool hasHe = rtl::hasHebrew(r.c_str());
#ifdef HAVE_HE_FONT
  he = hasHe;
#else
  he = false;
  if (hasHe) { String a = asciiOnly(r.c_str()); a.replace("?", ""); a.trim(); r = a.length() ? a : String(ui(S_NONLATIN)); }
#endif
  return r;
}

static void noteStart(uint32_t now, St ret) {
  if (playBuf) stopPlayback();
  if (aud != Aud::MIC) audMic();
  haptic("s");
  if (!shift::noteBegin(ret == St::SQ && sqFollow)) { haptic("lll"); toError(S_ERR_NOSTORE, now); return; }   // follow-up answers carry X-Kind: answer
  noteReturn = ret;
  micGuardUntil = millis() + 300;                // skip the vibration / beep noise at the start of the note
  memset(wave, 0, sizeof(wave));
  setSt(St::NOTE, millis());
}

static void sqNext(uint32_t now);
static void sqShow(uint32_t now);

static void noteStop(uint32_t now, bool autoStop) {
  uint32_t dur = 0;
  shift::NoteEnd r = shift::noteEnd(&dur);
  haptic(r == shift::NE_OK ? (autoStop ? "sss" : "ss") : (r == shift::NE_ERROR ? "lll" : ""));
  St ret = noteReturn; noteReturn = St::IDLE;
  if (ret == St::SQ) { setSt(St::SQ, now); if (r == shift::NE_OK) sqNext(now); }
  else setSt(St::IDLE, now);
}

#if SHIFT_SPOKEN_QUESTIONS
static void qTask(void*) {
  uint8_t* b = nullptr; size_t n = 0;
  bool ok = fetchWav(qUrl, &b, &n);
  if (ok) { qBuf = b; qLen = n; qState = 2; } else qState = 3;
  vTaskDelete(nullptr);
}
#endif

static void sqShow(uint32_t now) {
  shift::setLang(g_lang);
  sqMode = SqMode::QUESTIONS;
  bool he = false;
  String t = safeText(sqQ[sqIdx], he);
  replyHeFont = he; replyRight = he;
  wrapText(t, he, L.wrapW, 2);
  setSt(St::SQ, now);
#if SHIFT_SPOKEN_QUESTIONS
  if (!sqFollow && qState == 0 && shift::questionUrl(sqIdx + 1, qUrl, sizeof(qUrl))) {   // best effort: spoken question
    qFor = sqIdx; qState = 1;
    xTaskCreatePinnedToCore(qTask, "qfetch", 12288, nullptr, 1, nullptr, 0);
  }
#endif
  haptic("s");                                      // new question (the bridge has no audio for it)
}

static void swaitStart(uint32_t now) {
  shift::setLang(g_lang);
  if (!shift::finishBegin(true)) { toError(S_ERR_BRIDGE, now); return; }
  swaitSince = now;
  setSt(St::SWAIT, now);
}

static void sqNext(uint32_t now) {
  sqIdx++;
  if (sqIdx < (int)sqQ.size()) sqShow(now); else swaitStart(now);
}

static void startQuestions(uint32_t now) {
  sqQ.clear();
  sqQ.push_back(ui(S_Q1)); sqQ.push_back(ui(S_Q2)); sqQ.push_back(ui(S_Q3)); sqQ.push_back(ui(S_Q4));
  sqIdx = 0; sqFollow = false;
  sqShow(now);
}

static void startSummary(const String& text, const std::vector<String>& urls, uint32_t now) {
  if (job.status != 0) { toError(S_ERR_BUSY, now); return; }
  shiftReplyText = text.length() ? text : String(ui(S_NOREPLY));
  job.text[0] = job.reply[0] = 0; strlcpy(job.action, "face:happy", sizeof(job.action));
  job.audioUrl[0] = 0;
  job.orphan = false; job.audio = nullptr; job.audioLen = 0;
  sumUrls = urls;
  thinkTimeoutMs = 70000;                           // up to ~8 chunks to download
#if defined(DASHIE_NO_SPEAKER) && !defined(USE_A2DP_OUT)
  job.status = 2;                                   // nothing could play the audio: text only
#else
  if (!sumUrls.empty()) {
    job.status = 1;
    xTaskCreatePinnedToCore([](void*) {
      uint8_t* b = nullptr; size_t n = 0;
      if (fetchMerged(sumUrls, &b, &n)) { job.audio = b; job.audioLen = n; }   // failure => text only
      if (job.orphan) { if (job.audio) { heap_caps_free(job.audio); job.audio = nullptr; } job.status = 0; }
      else job.status = 2;
      vTaskDelete(nullptr);
    }, "sumfetch", 20480, nullptr, 1, nullptr, 0);
  } else job.status = 2;
#endif
  setSt(St::THINK, now);
}

static void startReadLast(uint32_t now) {
  String id, spoken; std::vector<String> urls;
  if (!shift::lastSummary(id, spoken, urls)) { toInfo(S_NO_SUMMARY, now); return; }
  startSummary(spoken, urls, now);
}

// Returns true when the key was consumed by shift mode.
static bool shiftAction(char which, uint32_t now) {
  if (st == St::CRASH || st == St::PALERT || st == St::FIND) return false;
  const bool on = shift::active();
  if (st == St::IDLE && screen == SC_SHIFT) {        // touch zones / B-hold on the Shift screen
    if (which == 'A') { if (on) which = 'N'; else return true; }
    else if (which == 'C') { if (on) which = 'E'; else { startReadLast(now); return true; } }
  }
  switch (which) {
    case 'N':
      if (st == St::NOTE) noteStop(now, false);
      else if (st == St::SQ) { if (sqMode == SqMode::CONFIRM) startQuestions(now); else noteStart(now, St::SQ); }
      else if (st == St::IDLE && on) noteStart(now, St::IDLE);
      return true;
    case 'E':
      if (st == St::IDLE && on) { confirmSince = now; sqMode = SqMode::CONFIRM; setSt(St::SQ, now); haptic("ls"); }
      else if (st == St::SQ) {
        if (sqMode == SqMode::QUESTIONS) { if (playBuf) { stopPlayback(); audMic(); } sqNext(now); }
        else setSt(St::IDLE, now);
      }
      return true;
    case 'H':
      if (st == St::IDLE && !on && !shift::finishing()) {
        if (shift::start()) haptic("l"); else toError(S_ERR_NOSTORE, now);
      }
      return true;
    default: break;
  }
  if (st == St::NOTE) return true;                   // recording: ignore everything else
  if (st == St::SWAIT) { shift::finishDetach(); toInfo(S_SAVED_OFFLINE, now); return true; }
  if (st == St::SQ) { if (playBuf) { stopPlayback(); audMic(); } setSt(St::IDLE, now); return true; }   // cancel
  return false;
}

// Runs every loop(): haptics, upload gating, warnings and the shift state timers.
static void shiftLoop(uint32_t now) {
  shift::loop(now);
  hapticTick(now);
  bool busy = st == St::LISTEN || st == St::RECORD || st == St::THINK || st == St::REPLY || st == St::CRASH;
#ifdef USE_A2DP_OUT
  busy = busy || a2o::busy() || testRunning;
#endif
  shift::hold(busy);
#ifdef DASHIE_BUTTONS_ONLY
  {                                                    // Plus2: a long press must be deliberate while A is the note button
    static bool longHold = false;
    bool want = shift::active() || st == St::SQ || st == St::NOTE || st == St::SWAIT;
    if (want != longHold) { M5.BtnA.setHoldThresh(want ? 1500 : 500); longHold = want; }
  }
#endif
  if (now - lastBattPoll > 10000) { lastBattPoll = now; battPct = (int)M5.Power.getBatteryLevel(); }

  shift::Status ss = shift::status();
  if (shift::active() && !shift::recording() && st == St::IDLE) {     // warnings never interrupt a recording
    if (ss.writeErr && !warnedWrite) { warnedWrite = true; haptic("lll"); }
    if (now - lastWarn > 300000) {
      bool low = (battPct >= 0 && battPct < SHIFT_LOW_BATT_PCT && M5.Power.isCharging() != m5::Power_Class::is_charging)
                 || (ss.freeSec >= 0 && ss.freeSec < 120);
      if (low) { lastWarn = now; haptic("lsl"); }
    }
  }
  if (!shift::active()) warnedWrite = false;
  if (ss.summaryReady && !warnedSum && st == St::IDLE) { warnedSum = true; haptic("ssl"); }
  if (!ss.summaryReady) warnedSum = false;

  switch (st) {
    case St::NOTE:
      if (now - stSince >= (uint32_t)SHIFT_NOTE_MAX_S * 1000UL) noteStop(now, true);
      break;
    case St::SQ:
      if (sqMode == SqMode::CONFIRM && now - confirmSince > 6000) setSt(St::IDLE, now);
      else if (sqMode == SqMode::QUESTIONS) {
#if SHIFT_SPOKEN_QUESTIONS
        if (qState == 2) {
          qState = 0;
          if (qFor == sqIdx && !playBuf && !shift::recording()) startPlayback(qBuf, qLen, false);   // questions: own speaker, never the kit
          else { heap_caps_free(qBuf); }
          qBuf = nullptr;
        } else if (qState == 3) qState = 0;
#endif
        if (playBuf && !audioActive()) { stopPlayback(); audMic(); }
        if (now - lastActivity > 180000) { if (playBuf) { stopPlayback(); audMic(); } setSt(St::IDLE, now); }
      }
      break;
    case St::SWAIT: {
      shift::FinState fs = shift::finishState();
      if (fs == shift::FS_DONE || fs == shift::FS_NEED || fs == shift::FS_ERR) {
        shift::Result r; shift::takeResult(r);
        if (fs == shift::FS_DONE) startSummary(r.spoken.length() ? r.spoken : r.summary.substring(0, 600), r.audioUrls, now);
        else if (fs == shift::FS_NEED) { sqQ = r.questions; sqIdx = 0; sqFollow = true; haptic("ss"); sqShow(now); }
        else { shift::finishDetach(); toError(S_ERR_BRIDGE, now); }
      } else if (now - swaitSince > SHIFT_FINISH_WAIT_MS) {
        shift::finishDetach(); toInfo(S_SAVED_OFFLINE, now);
      }
      break;
    }
    default: break;
  }
}
#else
static inline bool shiftPocket(uint32_t) { return false; }
#endif

// ----------------------------------------------------------------------------
// Hub (car <-> pocket): UI side. Network I/O (heartbeat, commands, acks, alert) is in hub.cpp. HUB_ENABLED=0 compiles it out.
//   car    : READ_SUMMARY -> speaks the finished shift summary (same path as "Read last summary").
//   pocket : VIBRATE (crash alert from the car) / FIND_ME -> strong vibration / beeps until a button is pressed.
// A command is ACKED when it is accepted. If the device is busy the command stays pending (un-acked) and is retried
// on the next loop pass; the bridge also redelivers it (up to 3 times, 10 min TTL).
// ----------------------------------------------------------------------------
#if HUB_ENABLED
static bool hubVib = false;                      // Core2 has a vibration motor; the Plus2 only a buzzer
static uint32_t hubVibOff = 0, hubPulseAt = 0, hubUntil = 0, hubBattAt = 0, hubSecAt = 0;
static int hubBatt = -1;
static float palertG = -1;

static void alertBeep(int freq, int ms) {        // ignores the volume setting: an alert must be heard
  audSpk();
  M5.Speaker.setVolume(255);
  M5.Speaker.tone(freq, ms);
  uint32_t t = millis();
  while (M5.Speaker.isPlaying() && millis() - t < (uint32_t)ms + 200) delay(5);
  audMic();
}

static void hubPulse(uint32_t now, bool find) {
  if (hubVib) { M5.Power.setVibration(255); hubVibOff = now + (find ? 300 : 500); }
  if (find) alertBeep(2600, 150); else alertBeep(2400, 250);
}

// Same preparation as a local crash: save a running note, stop audio, forget a pending request.
static void hubInterrupt(uint32_t now) {
#if SHIFT_ENABLED
  if (st == St::NOTE) noteStop(now, true);
  if (st == St::SWAIT) shift::finishDetach();
#endif
  stopPlayback(); freeRec();
  if (st == St::THINK) job.orphan = true;
  if (aud != Aud::MIC) audMic();
  lastActivity = now;
}

static void startPalert(float g, uint32_t now) {
  hubInterrupt(now);
  palertG = g; hubPulseAt = 0; hubUntil = now + (uint32_t)HUB_PALERT_MAX_S * 1000UL;
  setSt(St::PALERT, now);
}

static void startFind(uint16_t durS, uint32_t now) {
  hubInterrupt(now);
  hubPulseAt = 0; hubUntil = now + (uint32_t)durS * 1000UL;
  setSt(St::FIND, now);
}

// Every loop(): feed the hub task, run the alert states, take at most one command.
static void hubLoop(uint32_t now) {
  if (hubVibOff && now >= hubVibOff) { M5.Power.setVibration(0); hubVibOff = 0; }
  // heartbeat inputs: the battery is read here, the I2C bus belongs to the main loop (the hub task never touches M5)
  if (!hubBattAt || now - hubBattAt > 10000) { hubBattAt = now; hubBatt = (int)M5.Power.getBatteryLevel(); }
  if (now - hubSecAt >= 1000) { hubSecAt = now; hub::setLocal(hubBatt, (int)shift::status().pending, shift::active()); }
  bool busy = st == St::NOTE;                    // never poll over a running recording
#ifdef USE_A2DP_OUT
  busy = busy || a2o::busy() || testRunning;
#endif
  hub::hold(busy);

  if (st == St::PALERT || st == St::FIND) {
    if ((int32_t)(now - hubUntil) >= 0) { toIdle(now); }
    else if (now >= hubPulseAt) { hubPulse(now, st == St::FIND); hubPulseAt = millis() + (st == St::FIND ? 700 : 1100); }
  }

  hub::Cmd c;
  if (!hub::next(c)) return;
  switch (c.type) {
    case hub::CmdType::READ_SUMMARY: {
#if SHIFT_ENABLED
      bool free = st == St::IDLE && job.status == 0 && !playBuf && !shift::recording();
#ifdef USE_A2DP_OUT
      free = free && !a2o::busy() && !testRunning && !btPicking;
#endif
      if (!free) return;                         // busy: stay pending and un-acked
      hub::accept(c.id);
      lastActivity = now;
      startSummary(c.spoken, c.audioUrls, now);
#else
      hub::accept(c.id);                         // no shift module in this build: nothing to read
#endif
      break;
    }
    case hub::CmdType::VIBRATE:
      if (st == St::CRASH) return;               // a local crash screen is already shouting
      hub::accept(c.id); startPalert(c.g, now);
      break;
    case hub::CmdType::FIND_ME:
      if (st == St::CRASH) return;
      hub::accept(c.id); startFind(c.durationS, now);
      break;
    default: hub::accept(c.id); break;
  }
}
#endif

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
    case 6: cfg.recipe = !cfg.recipe; break;
#ifdef USE_A2DP_OUT
    case 7: if (a2o::test()) { testRunning = true; testRes = a2o::Res::NONE; } break;
#endif
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
#if SHIFT_ENABLED
  if (shiftAction(which, now)) return;
#endif
  switch (st) {
    case St::NOTE: case St::SQ: case St::SWAIT: return;        // shift mode owns these states
    case St::PALERT: case St::FIND: toIdle(now); return;       // hub alert / "find me": any button silences it
    case St::CRASH: cancelCrash(now); return;
    case St::ERROR: toIdle(now); return;
    case St::LISTEN: case St::RECORD: if (which == 'A' || which == 'T') toIdle(now); return;
    case St::REPLY: toIdle(now); return;
    case St::THINK: return;
    case St::IDLE: break;
  }
#ifdef USE_A2DP_OUT
  if (screen == SC_SET && btPicking) {          // scan list: A next, B back, C pick
    int n = a2o::foundCount();
    if (which == 'A' && n) pickSel = (pickSel + 1) % n;
    else if (which == 'B' || which == 'S') btPicking = false;
    else if (which == 'C' && n) {
      a2o::selectFound(pickSel); btPicking = false;
      if (a2o::test()) { testRunning = true; testRes = a2o::Res::NONE; }
    }
    return;
  }
#endif
  if (which == 'S') {                           // button-only boards: PWR toggles Settings, A-hold leaves it
    screen = (screen == SC_SET) ? SC_FACE : SC_SET; setSel = 0; resetArmedAt = 0;
    return;
  }
#ifdef DASHIE_BUTTONS_ONLY
  if (which == 'B') { screen = (Screen)((screen + 1) % (SC_COUNT - 1)); return; }   // Settings is reached with PWR
#else
  if (which == 'B') { screen = (Screen)((screen + 1) % SC_COUNT); return; }
#endif
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

// One input scheme for all boards: everything funnels into onAction(which):
//   'A' = primary (talk / next settings row)   'B' = next screen   'C' = option (reset / change value)
//   'T' = tap on the face (touch only)         'S' = toggle Settings / back (button-only)
// Touch boards (Core2/CoreS3): BtnA/B/C or the touch zones under the screen.
// Button-only boards (Plus2): A click = 'A'; B click = 'B' (in Settings: 'C' = change value);
// B hold = 'C' (reset); PWR click = 'S'; A hold in Settings = 'S' (back).
static void handleInput(uint32_t now) {
  // ---- recipe PTT (Settings: "Button asks: recipe book"): hold BtnA to talk, release to send. Not during a shift.
  if (pttHeld) {
    if (st != St::RECORD) { pttHeld = false; }                    // cancelled by something else (crash, hub alert)
    else {
      if (M5.BtnA.isPressed()) pttUpSince = 0;
      else if (!pttUpSince) pttUpSince = now;
      if (pttUpSince && now - pttUpSince >= 120) {               // released (debounced: touch zones flicker)
        pttHeld = false;
        if (recMs >= 400) startNet(now); else toIdle(now);       // a tap is not a question
      }
      return;                                                     // swallow every other input while talking
    }
  }
  #if SHIFT_ENABLED
  const bool onShiftScr = (screen == SC_SHIFT);
#else
  const bool onShiftScr = false;
#endif
  if (cfg.recipe && st == St::IDLE && screen != SC_SET && !onShiftScr && !shift::active() && M5.BtnA.wasPressed()) {
    startPtt(now);
    return;
  }
#ifdef DASHIE_BUTTONS_ONLY
  const bool inSet = (screen == SC_SET) && st == St::IDLE;
#if SHIFT_ENABLED
  const bool shiftOn = shift::active() || st == St::SQ || st == St::NOTE || st == St::SWAIT;
  if (shiftOn) {                                    // during a shift A is the note button (hold = end / skip)
    if (M5.BtnA.wasHold()) onAction('E', now);
    else if (M5.BtnA.wasClicked()) onAction('N', now);
  } else if (screen == SC_SHIFT && st == St::IDLE && M5.BtnA.wasHold()) {
    onAction('H', now);
  } else
#endif
  {
    if (M5.BtnA.wasHold()) onAction(inSet ? 'S' : 'A', now);
    else if (M5.BtnA.wasClicked()) onAction('A', now);
  }
  if (M5.BtnB.wasHold()) onAction('C', now);
  else if (M5.BtnB.wasClicked()) onAction(inSet ? 'C' : 'B', now);
  if (M5.BtnPWR.wasClicked()) onAction('S', now);
#else
#if SHIFT_ENABLED
  {
    const bool sh = shift::active() || st == St::SQ || st == St::NOTE;
    if (sh && M5.BtnPWR.wasClicked()) onAction('N', now);              // physical side button
    if (sh && M5.BtnPWR.wasHold()) onAction('E', now);
    if (screen == SC_SHIFT && st == St::IDLE && !shift::active() && !shift::finishing() && M5.BtnA.wasHold()) onAction('H', now);
    if (shiftPocket(now)) return;                                      // screen off: touch zones are ignored
  }
#endif
  if (M5.BtnA.wasPressed()) onAction('A', now);
  else if (M5.BtnB.wasPressed()) onAction('B', now);
  else if (M5.BtnC.wasPressed()) onAction('C', now);
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (t.y < 200) onAction('T', now);
    else if (t.y < 240) onAction(t.x < 107 ? 'A' : (t.x < 214 ? 'B' : 'C'), now);   // CoreS3 / safety
  }
#endif
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
    if (cfg.voice && !shift::active()) {          // no voice trigger during a shift (kitchen noise, pocket)
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
    if (pttHeld) {                                    // recipe PTT: the button decides, not the silence detector
      if (recN >= MAX_SAMP) { pttHeld = false; startNet(now); }
    } else if (recN >= MAX_SAMP || (speechSeen && recQuietMs >= END_SILENCE_MS && recMs >= 500)) {
      startNet(now);
    } else if (!speechSeen && recMs >= 2500) {
      toError(S_ERR_NOCATCH, now);
    }
  } else if (st == St::NOTE) {
    shift::noteFeed(f, FR);                           // streamed to storage by the writer task
  }
}

// ----------------------------------------------------------------------------
// IMU
// ----------------------------------------------------------------------------
static float crashPeak = 0;                      // highest |a| (g) seen during the current impact
#if HUB_ENABLED
static bool crashAlertSent = false;              // HUB_ALERT_ON_TIMEOUT_ONLY: the late alert goes out once
#endif
static void startCrash(uint32_t now) {
#if HUB_ENABLED
  crashAlertSent = false;
  if (hub::isCar() && !HUB_ALERT_ON_TIMEOUT_ONLY) { hub::alert(crashPeak); crashAlertSent = true; }   // tell the pocket device
#endif
#if SHIFT_ENABLED
  if (st == St::NOTE) noteStop(now, true);
  if (st == St::SWAIT) shift::finishDetach();
#endif
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
  latG = DASHIE_IMU_LAT_EXPR; fwdG = DASHIE_IMU_FWD_EXPR;
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
  const bool noCrashHere = HUB_ENABLED && HUB_ROLE_POCKET && HUB_POCKET_NO_CRASH && shift::active();   // a pocket knocks around
  if (mag > CRASH_G) {
    if (mag > crashPeak) crashPeak = mag;
    if (!crashHigh) { crashHigh = true; crashHighSince = now; }
    else if (now - crashHighSince >= 30 && st != St::CRASH && now > crashRefract && !noCrashHere) startCrash(now);
  } else { crashHigh = false; crashPeak = 0; }
}

// ----------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------
static const FaceColors& col() { return THEMES[cfg.theme]; }

// Hint-bar labels. Touch boards: A/B/C = three zones. Button-only boards: A = front, B = side, PWR.
#ifdef DASHIE_BUTTONS_ONLY
#  define BAR_FACE   S_TALK, S_NEXT, S_SET
#  define BAR_RESET  S_TALK, S_NEXT, S_SET
#  define BAR_SET    S_NEXT, S_CHANGE, S_BACK
#  define BAR_PICK   S_NEXT, S_PICK, S_BACK
#else
#  define BAR_FACE   S_TALK, S_NEXT, S_OPT
#  define BAR_RESET  S_TALK, S_NEXT, S_RESET
#  define BAR_SET    S_NEXT, S_NEXT, S_OPT
#  define BAR_PICK   S_NEXT, S_BACK, S_PICK
#endif

#define BAR_SHIFT_IDLE S_START, S_NEXT, S_READ       // Shift screen, no shift running
#define BAR_SHIFT_ON   S_NOTE, S_NEXT, S_END         // Shift screen, shift running

static void bottomBar(StrId a, StrId b, StrId c) {
  const FaceColors& k = col();
  cv.drawFastHLine(0, L.barY, L.W, k.acc);
  say(ui(a), L.W / 6, L.barTextY, AL_C, 2, k.fg);
  say(ui(b), L.W / 2, L.barTextY, AL_C, 2, k.fg);
  say(ui(c), (L.W * 5 + 3) / 6, L.barTextY, AL_C, 2, k.fg);
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

static const char* cancelHint() {
#ifdef DASHIE_BUTTONS_ONLY
  return ui(S_PRESSCANCEL);
#else
  return ui(S_TAPCANCEL);
#endif
}

static void renderScreen(uint32_t now) {
  const FaceColors& k = col();
  char b[48];
  const bool he = heActive();
  switch (screen) {
    case SC_FACE: {
      bool whoa = now < brakeFlashUntil;
      Mood m = whoa ? Mood::WORRIED : noiseMood();
      drawFace(cv, L.cx(), L.faceCy, L.faceS, m, now, k);
      clockStr(b, sizeof(b));
      say(b, L.right(), L.clockY, AL_R, L.clockSz, k.fg, true);
      say(WiFi.status() == WL_CONNECTED ? "Wi-Fi" : "-", 8, L.wifiY, AL_L, 1, WiFi.status() == WL_CONNECTED ? k.acc : k.fg, true);
      if (cfg.voice) cv.fillCircle(L.dotX, L.dotY, L.dotR, k.acc); else cv.drawCircle(L.dotX, L.dotY, L.dotR, k.fg);
      if (whoa) say(ui(S_WHOA), L.cx(), L.whoaY, AL_C, 2, k.acc);
      if (!(whoa && L.compact)) bottomBar(BAR_FACE);     // compact: "Whoa!" takes the bar's place
      break;
    }
    case SC_NOISE: {
      say(ui(S_NOISE), he ? L.mirror(L.noiseTitleX) : L.noiseTitleX, L.noiseTitleY, he ? AL_R : AL_L, 2, k.fg);
      drawFace(cv, L.noiseFaceX, L.noiseFaceY, L.noiseFaceS, noiseMood(), now, k);
      snprintf(b, sizeof(b), "%d", (int)(dbSm + 0.5f));
      bigNum(b, L.bigNumX, L.bigNumY, dbSm > 75 ? rgb(255, 80, 60) : (dbSm > 60 ? rgb(255, 200, 0) : k.fg));
      say("dB", L.dbX, L.dbY, AL_L, 2, k.acc, true);
      snprintf(b, sizeof(b), "%s %lus  (%lus)", ui(S_QUIET), (unsigned long)quietStreak, (unsigned long)bestQuiet);
      say(b, L.quietX, L.quietY, AL_C, 1, k.fg);
      int base = L.histBase;
      cv.drawFastHLine(L.histX, base, 60 * L.histStep, k.acc);
      for (int i = 0; i < 60; i++) {
        float v = hist[(histIdx + i) % 60];
        int h = constrain((int)((v - 30.0f) * 1.3f), 0, L.histMaxH);
        uint16_t c2 = v > 75 ? rgb(255, 80, 60) : (v > 60 ? rgb(255, 200, 0) : k.acc);
        if (v > 1) cv.fillRect(L.histX + i * L.histStep, base - h, L.histW, h, c2);
      }
      bottomBar(BAR_RESET);
      break;
    }
    case SC_G: {
      int cx = he ? L.mirror(L.gCx) : L.gCx, cy = L.gCy, R = L.gR;
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
      cv.fillCircle(bx, by, L.gBall, bc);
      // three stat blocks (EN positions in L; Hebrew mirrors x and alignment)
      auto stat = [&](int i, StrId lab, const char* val) {
        int x = he ? L.mirror(L.gStatX[i]) : L.gStatX[i];
        Align a = (Align)L.gStatAlign[i];
        if (he) a = (a == AL_L) ? AL_R : AL_L;
        say(ui(lab), x, L.gStatLabelY[i], a, 1, k.fg);
        say(val, x, L.gStatValueY[i], a, 2, k.acc, true);
      };
      snprintf(b, sizeof(b), "%.2fg", trip.maxG);        stat(0, S_MAXG, b);
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.brakes); stat(1, S_BRAKES, b);
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.turns);  stat(2, S_TURNS, b);
      bottomBar(BAR_RESET);
      break;
    }
    case SC_TRIP: {
      uint32_t s = trip.secs;
      int y = L.rowY0;
      snprintf(b, sizeof(b), "%02lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60), (unsigned long)(s % 60));
      row(y, ui(S_TIME), b, k.fg, k.acc); y += L.rowPitch;
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.brakes);
      row(y, ui(S_BRAKES), b, k.fg, k.acc); y += L.rowPitch;
      snprintf(b, sizeof(b), "%lu", (unsigned long)trip.turns);
      row(y, ui(S_TURNS), b, k.fg, k.acc); y += L.rowPitch;
      snprintf(b, sizeof(b), "%.2f g", trip.maxG);
      row(y, ui(S_MAXG), b, k.fg, k.acc); y += L.rowPitch;
      snprintf(b, sizeof(b), "%d dB", trip.noiseN ? (int)(trip.noiseSum / trip.noiseN + 0.5f) : 0);
      row(y, ui(S_AVGNOISE), b, k.fg, k.acc); y += L.rowPitch;
      snprintf(b, sizeof(b), "%lu s", (unsigned long)bestQuiet);
      row(y, ui(S_BESTQUIET), b, k.fg, k.acc);
      if (resetArmedAt && now - resetArmedAt < 3000) say(ui(S_PRESS_AGAIN), L.cx(), L.pressY, AL_C, 1, rgb(255, 120, 80));
      bottomBar(BAR_RESET);
      break;
    }
#if SHIFT_ENABLED
    case SC_SHIFT: {
      shift::Status ss = shift::status();
      const bool on = ss.active || ss.finishing;
      const uint16_t amber = rgb(255, 170, 40), red = rgb(255, 80, 60);
#ifdef DASHIE_BUTTONS_ONLY
      const char* noteHint = ui(S_SH_HINT_STICK);
#else
      const char* noteHint = ui(S_SH_HINT_CORE);
#endif
      int y = L.rowY0;
      if (on) {
        char tb[16] = "--:--:--";
        time_t tn = time(nullptr);
        if (ss.startedAt && tn > (time_t)ss.startedAt) {
          unsigned long e = (unsigned long)(tn - ss.startedAt);
          snprintf(tb, sizeof(tb), "%02lu:%02lu:%02lu", e / 3600, (e / 60) % 60, e % 60);
        }
        row(y, ui(S_TIME), tb, k.fg, k.acc); y += L.rowPitch;
        row(y, ui(S_SH_NOTES), String(ss.notes), k.fg, k.acc); y += L.rowPitch;
        row(y, ui(S_SH_QUEUE), String(ss.pending) + ((ss.pending && !ss.online) ? String(" ") + ui(S_SH_OFFLINE) : String("")),
            k.fg, (ss.pending && !ss.online) ? amber : k.acc); y += L.rowPitch;
        row(y, ui(S_SH_BATT), battPct >= 0 ? String(battPct) + "%" : String("--"), k.fg,
            (battPct >= 0 && battPct < SHIFT_LOW_BATT_PCT) ? red : k.acc); y += L.rowPitch;
        if (!L.compact) {
          String st2 = ss.backend == shift::BK_SD ? String("SD") : (ss.backend == shift::BK_LFS ? String("Flash") : String("-"));
          if (ss.freeSec >= 0) st2 += String(" ") + String(ss.freeSec / 60) + " min";
          row(y, ui(S_SH_STORE), st2, k.fg, (ss.freeSec >= 0 && ss.freeSec < 120) ? red : k.acc);
        }
        const char* warn = nullptr; uint16_t wc = red;
        if (ss.writeErr) warn = ui(S_SH_WRITEERR);
        else if (ss.freeSec >= 0 && ss.freeSec < 120) warn = ui(S_SH_LOWSTORE);
        else if (battPct >= 0 && battPct < SHIFT_LOW_BATT_PCT) warn = ui(S_SH_LOWBATT);
        else if (ss.finishing) { warn = ui(S_SENDING); wc = k.acc; }
        else if (ss.summaryReady) { warn = ui(S_SH_SUMREADY); wc = k.acc; }
        if (warn) {
          say(warn, L.cx(), L.shHintY, AL_C, 1, wc);
          if (L.shHint2Y) say(noteHint, L.cx(), L.shHint2Y, AL_C, 1, k.fg);
        } else say(noteHint, L.cx(), L.shHintY, AL_C, 1, k.fg);
        bottomBar(BAR_SHIFT_ON);
      } else {
        row(y, ui(S_SHIFT), ui(S_SH_NONE), k.fg, k.acc); y += L.rowPitch;
        row(y, ui(S_SH_LAST), ui(shift::hasLastSummary() ? S_SH_READY : S_SH_NOLAST), k.fg, k.acc); y += L.rowPitch;
        if (ss.pending) row(y, ui(S_SH_QUEUE), String(ss.pending), k.fg, amber);
        say(ui(S_SH_HOLDA), L.cx(), L.shHintY, AL_C, 1, k.fg);
        if (ss.summaryReady && L.shHint2Y) say(ui(S_SH_SUMREADY), L.cx(), L.shHint2Y, AL_C, 1, k.acc);
        bottomBar(BAR_SHIFT_IDLE);
      }
      break;
    }
#endif
#if HUB_SCREEN
    case SC_PEER: {
      hub::Peer pr = hub::peer();
      const uint16_t amber = rgb(255, 170, 40), red = rgb(255, 80, 60);
      int y = L.rowY0;
      row(y, ui(S_LINK), ui(hub::online() ? S_ONLINE : S_SH_OFFLINE), k.fg, hub::online() ? k.acc : amber); y += L.rowPitch;
      uint32_t age = pr.valid ? pr.lastSeenS + (now - pr.receivedMs) / 1000 : 0;
      String ps = !pr.valid ? String(ui(S_PEER_NONE)) : (age <= 90 ? String(ui(S_ONLINE)) : String(ui(S_SH_OFFLINE)) + " " + String(age < 3600 ? (int)(age / 60) : 99) + "m");
      row(y, ui(S_PEER), ps, k.fg, (pr.valid && age <= 90) ? k.acc : amber); y += L.rowPitch;
      row(y, ui(S_SH_BATT), pr.valid && pr.battery >= 0 ? String((int)pr.battery) + "%" : String("--"), k.fg,
          (pr.valid && pr.battery >= 0 && pr.battery < SHIFT_LOW_BATT_PCT) ? red : k.acc); y += L.rowPitch;
      row(y, ui(S_SH_QUEUE), pr.valid && pr.notes >= 0 ? String((long)pr.notes) : String("--"), k.fg, (pr.valid && pr.notes > 0) ? amber : k.acc); y += L.rowPitch;
      row(y, ui(S_SHIFT), pr.valid && pr.shiftActive >= 0 ? ui(pr.shiftActive ? S_ACTIVE : S_IDLE_W) : ui(S_DASH), k.fg, k.acc); y += L.rowPitch;
      row(y, ui(S_INCAR), ui(hub::inCar() ? S_YES : S_NO), k.fg, hub::inCar() ? k.acc : k.fg);
      bottomBar(S_TALK, S_NEXT, S_DASH);
      break;
    }
#endif
    case SC_SET: {
#ifdef USE_A2DP_OUT
      if (btPicking) {                            // scan results: pick the kit
        say(ui(S_BT_PICK), L.cx(), L.pickTitleY, AL_C, 2, k.acc);
        int n = a2o::foundCount();
        if (!n) say(ui(S_BT_NODEV), L.cx(), L.H * 5 / 12, AL_C, L.compact ? 1 : 2, k.fg);
        static int pickFirst = 0;
        int vis = min(L.pickVisible, 8);
        if (pickSel < pickFirst) pickFirst = pickSel;
        if (pickSel >= pickFirst + vis) pickFirst = pickSel - vis + 1;
        if (pickFirst < 0 || pickFirst >= max(n, 1)) pickFirst = 0;
        for (int r = 0; r < vis && pickFirst + r < n; r++) {
          int i = pickFirst + r;
          int y = L.pickY0 + r * L.pickPitch;
          if (i == pickSel) cv.drawRoundRect(2, y - 3, L.W - 4, L.pickRectH, 5, k.acc);
          say(a2o::foundName(i), 10, y, AL_L, 1, i == pickSel ? k.acc : k.fg);
        }
        bottomBar(BAR_PICK);
        break;
      }
#endif
      static const StrId labels[SET_ITEMS] = {S_BRIGHT, S_VOLUME, S_SENS, S_THEME, S_VOICE, S_LANG, S_PTT
#ifdef USE_A2DP_OUT
        , S_TESTKIT
#endif
      };
      const int pitch = SET_ITEMS > 6 ? L.setPitch7 : L.setPitch6;
      const int vis = min(L.setVisible, (int)SET_ITEMS);   // compact: scrolls so the selected row stays visible
      static int setFirst = 0;
      if (setSel < setFirst) setFirst = setSel;
      if (setSel >= setFirst + vis) setFirst = setSel - vis + 1;
      for (int r = 0; r < vis; r++) {
        int i = setFirst + r;
        int y = L.setY0 + r * pitch;
        if (i == setSel) cv.drawRoundRect(2, y - L.setRectDy, L.W - 4, pitch - 3, 6, k.acc);
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
          case 6: v = ui(cfg.recipe ? S_PTT_RECIPES : S_PTT_ASSIST); break;
#ifdef USE_A2DP_OUT
          case 7: {
            StrId id =
#ifdef DASHIE_BUTTONS_ONLY
                S_BT_IDLE_BTN;
#else
                S_BT_IDLE;
#endif
            if (testRunning) id = S_BT_CONN;
            else switch (testRes) {
              case a2o::Res::OK: id = S_BT_OK; break;
              case a2o::Res::NOT_FOUND: id = S_BT_NF; break;
              case a2o::Res::TIMEOUT: id = S_BT_TO; break;
              case a2o::Res::NO_MEM: id = S_BT_MEM; break;
              case a2o::Res::FAIL: id = S_BT_FAIL; break;
              default: break;
            }
            v = ui(id);
            break;
          }
#endif
        }
        row(y, ui(labels[i]), v, i == setSel ? k.acc : k.fg, k.fg);
      }
      bottomBar(BAR_SET);
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
      drawFace(cv, L.cx(), L.listenCy, L.faceS, Mood::LISTEN, now, k);
      say(ui(S_LISTENING), L.cx(), L.statusY, AL_C, 2, k.acc);
      break;
    case St::RECORD: {
      drawFace(cv, L.cx(), L.recFaceY, L.recFaceS, Mood::LISTEN, now, k);
      for (int i = 0; i < 64; i++) {
        float v = wave[(waveIdx + i) % 64];
        int h = 2 + (int)(v * L.waveMaxH);
        cv.fillRect(L.waveX + i * L.waveStep, L.waveBase - h / 2, L.waveW, h, speechSeen ? k.acc : k.fg);
      }
      snprintf(b, sizeof(b), "%.1fs", recMs / 1000.0f);
      say(ui(pttHeld ? S_RCP_HOLD : S_RECORDING), L.margin, L.statusY, AL_L, 2, k.fg);
      say(b, L.right(), L.statusY, AL_R, 2, k.acc, true);
      break;
    }
    case St::THINK:
      drawFace(cv, L.cx(), L.thinkCy, L.faceS, Mood::THINK, now, k);
      say(ui(S_THINKING), L.cx(), L.statusY, AL_C, 2, k.acc);
      break;
    case St::REPLY: {
      bool speaking = playBuf && audioSounding();
      drawFace(cv, L.replyFaceX, L.replyFaceY, L.replyFaceS, speaking ? Mood::TALK : replyMood, now, k, mouthOpen);
      int lineH = replyHeFont || heActive() ? L.replyLineHe : L.replyLineLatin;
      int vis = (L.replyBottom - L.replyTextY) / lineH;
      int total = (int)replyLines.size();
      int scroll = 0;
      if (total > vis) scroll = constrain((int)((now - replySince - 1500) / 1800), 0, total - vis);
      for (int i = 0; i < vis && scroll + i < total; i++) {
        bool he = replyHeFont;
        setupFont(he, 2);
        cv.setTextColor(k.fg);
        const String& ln = replyLines[scroll + i];
        int x = replyRight ? L.right() - cv.textWidth(ln.c_str()) : L.margin;
        cv.drawString(ln.c_str(), x, L.replyTextY + i * lineH);
      }
      break;
    }
#if SHIFT_ENABLED
    case St::NOTE: {
      drawFace(cv, L.cx(), L.recFaceY, L.recFaceS, Mood::LISTEN, now, k);
      for (int i = 0; i < 64; i++) {
        float v = wave[(waveIdx + i) % 64];
        int h = 2 + (int)(v * L.waveMaxH);
        cv.fillRect(L.waveX + i * L.waveStep, L.waveBase - h / 2, L.waveW, h, rgb(255, 90, 70));
      }
      if ((now / 500) % 2) cv.fillCircle(L.margin + 4, L.statusY + 7, 5, rgb(255, 60, 40));
      say(ui(S_REC_NOTE), L.margin + 14, L.statusY, AL_L, 2, k.fg);
      snprintf(b, sizeof(b), "%lus", (unsigned long)((now - stSince) / 1000));
      say(b, L.right(), L.statusY, AL_R, 2, k.acc, true);
      break;
    }
    case St::SQ: {
#ifdef DASHIE_BUTTONS_ONLY
      const char* confirmHint = ui(S_ENDQ_STICK); const char* qHint = ui(S_Q_HINT_STICK);
#else
      const char* confirmHint = ui(S_ENDQ_CORE); const char* qHint = ui(S_Q_HINT_CORE);
#endif
      if (sqMode == SqMode::CONFIRM) {
        say(ui(S_ENDQ), L.cx(), L.H * 2 / 5 - 8, AL_C, 2, k.acc);
        say(confirmHint, L.cx(), L.H * 3 / 5 - 4, AL_C, L.compact ? 1 : 2, k.fg);
      } else {
        snprintf(b, sizeof(b), "%d/%d", sqIdx + 1, (int)sqQ.size());
        say(b, L.right(), L.sqTitleY, AL_R, 2, k.acc, true);
        int lineH = replyHeFont ? L.replyLineHe : L.replyLineLatin;
        int vis = (L.sqHintY - L.sqTextY - 4) / lineH;
        for (int i = 0; i < vis && i < (int)replyLines.size(); i++) {
          setupFont(replyHeFont, 2);
          cv.setTextColor(k.fg);
          const String& ln = replyLines[i];
          int x = replyRight ? L.right() - cv.textWidth(ln.c_str()) : L.margin;
          cv.drawString(ln.c_str(), x, L.sqTextY + i * lineH);
        }
        say(qHint, L.cx(), L.sqHintY, AL_C, 1, k.acc);
      }
      break;
    }
    case St::SWAIT: {
      drawFace(cv, L.cx(), L.thinkCy, L.faceS, Mood::THINK, now, k);
      shift::Status ss = shift::status();
      String msg = ss.pending ? String(ui(S_SENDING)) + " " + String(ss.pending) : String(ui(S_SUMMARIZING));
      say(msg, L.cx(), L.statusY, AL_C, 2, k.acc);
      break;
    }
#endif
#if HUB_ENABLED
    case St::PALERT: {
      bool flash = (now / 400) % 2;
      cv.fillScreen(flash ? rgb(200, 0, 0) : rgb(110, 0, 0));
      uint16_t w = rgb(255, 255, 255);
      say(ui(S_CAR_ALERT), L.cx(), L.crashY1, AL_C, L.compact ? 1 : 2, w);
      if (palertG >= 0) { snprintf(b, sizeof(b), "%.1f g", palertG); say(b, L.cx(), L.crashY2, AL_C, 2, w, true); }
      say(cancelHint(), L.cx(), L.crashHintY, AL_C, 2, w);
      break;
    }
    case St::FIND:
      drawFace(cv, L.cx(), L.thinkCy, L.faceS, Mood::HAPPY, now, k);
      say(ui(S_FINDME), L.cx(), L.statusY, AL_C, 2, k.acc);
      break;
#endif
    case St::ERROR:
      drawFace(cv, L.cx(), L.errCy, L.errS, errInfo ? Mood::NEUTRAL : Mood::CONFUSED, now, k);
      {
        String m = ui(errId); const uint16_t ec = errInfo ? k.acc : rgb(255, 160, 80);
        setupFont(heActive(), 2);
        if (cv.textWidth(m.c_str()) > L.W - 12 && !heActive()) {     // too wide for one line: split at the space nearest the middle
          int best = -1;
          for (int i = 1; i < (int)m.length() - 1; i++) if (m[i] == ' ' && (best < 0 || abs(i - (int)m.length() / 2) < abs(best - (int)m.length() / 2))) best = i;
          if (best > 0) {
            int lh = cv.fontHeight() + 2;
            say(m.substring(0, best), L.cx(), L.errY - lh / 2, AL_C, 2, ec);
            say(m.substring(best + 1), L.cx(), L.errY + lh / 2, AL_C, 2, ec);
            break;
          }
        }
        say(m, L.cx(), L.errY, AL_C, 2, ec);
      }
      break;
    default: break;
  }
}

static void renderCrash(uint32_t now) {
  bool flash = (now / 400) % 2;
  cv.fillScreen(flash ? rgb(200, 0, 0) : rgb(110, 0, 0));
  uint16_t w = rgb(255, 255, 255);
  int left = CRASH_COUNTDOWN_S - (int)((now - crashStart) / 1000);
  say(ui(S_IMPACT), L.cx(), L.crashY1, AL_C, 2, w);
  if (left > 0) {
    say(ui(S_AREYOUOK), L.cx(), L.crashY2, AL_C, 2, w);
    char b[8]; snprintf(b, sizeof(b), "%d", left);
    bigNum(b, L.cx(), L.crashNumY, w);
    say(cancelHint(), L.cx(), L.crashHintY, AL_C, 2, w);
  } else {
    say(ui(S_SOS), L.cx(), L.crashSosY, AL_C, 2, w);
    say(cancelHint(), L.cx(), L.crashHintY, AL_C, 2, w);
  }
}

static void applyBrightness(uint32_t now) {
  uint8_t want = BRIGHT_LV[cfg.bright];
  if (st == St::IDLE && now - lastActivity > IDLE_DIM_MS) want = min<uint8_t>(want, 24);
  if (shiftPocket(now)) want = 0;                   // shift running + no input: screen off ("pocket mode")
  if (want != lastBright) { M5.Display.setBrightness(want); lastBright = want; }
}

// ----------------------------------------------------------------------------
// Setup / loop
// ----------------------------------------------------------------------------
void setup() {
  auto mcfg = M5.config();
  M5.begin(mcfg);
#ifdef DASHIE_BUTTONS_ONLY
  M5.Display.setRotation(DASHIE_ROTATION);          // 135x240 portrait panel -> 240x135 landscape
#endif
  L.init(M5.Display.width(), M5.Display.height());
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

  wifimgr::begin();                                 // up to 4 networks (config.h), joins the strongest; no-op if none is set
  configTzTime(TZ_STRING, "pool.ntp.org");
#if SHIFT_ENABLED
  haveVib = (M5.getBoard() == m5::board_t::board_M5StackCore2);
#endif
  shift::setLang(g_lang);
  shift::begin();                                   // RTC -> clock, resume a shift that was running before a reboot
#if HUB_ENABLED
  hubVib = (M5.getBoard() == m5::board_t::board_M5StackCore2);
  hub::begin();                                     // worker task: heartbeat / commands / alert (idle without Wi-Fi + bridge)
#endif

  M5.Speaker.end();
  audMic();
  uint32_t now = millis();
  shout.holdoff(now, 2000);
  lastActivity = now; lastSec = now; lastSave = now;
}

void loop() {
  M5.update();
  uint32_t now = millis();
#ifdef USE_A2DP_OUT
  if (deferFree && !a2o::busy()) { heap_caps_free(deferFree); deferFree = nullptr; }
  if (testRunning && !a2o::busy()) {
    testRunning = false; testRes = a2o::result();
    if (testRes == a2o::Res::NOT_FOUND && screen == SC_SET) { btPicking = true; pickSel = 0; }   // show what the scan saw
  }
#endif
  if (pttVibOff && now >= pttVibOff) { M5.Power.setVibration(0); pttVibOff = 0; }
  handleInput(now);
#if SHIFT_ENABLED
  shiftLoop(now);
#endif
  {
    bool wbusy = false;                             // scanning for a Wi-Fi network would disturb a running A2DP stream
#ifdef USE_A2DP_OUT
    wbusy = a2o::busy() || testRunning;
#endif
    wifimgr::hold(wbusy);
    wifimgr::loop(now);
  }
#if HUB_ENABLED
  hubLoop(now);
#endif

  const int16_t* f = nullptr;
  bool got = pollMic(&f);
  if (got) processFrame(f, now);
  pollImu(now);

  // ---- per-state timers
  switch (st) {
    case St::LISTEN: if (now - stSince >= 450) startRecord(now); break;
    case St::THINK:
      if (job.status == 2) startReply(now);
      else if (job.status == 3) {
        StrId e = job.err; job.status = 0;
        if (e >= S_RCP_WAIT && e <= S_RCP_DOWN) toInfo(e, now); else toError(e, now);   // recipe book not ready: calm screen
      }
      else if (now - stSince > thinkTimeoutMs) { job.orphan = true; toError(S_ERR_BRIDGE, now); }
      break;
    case St::REPLY: {
#ifdef USE_A2DP_OUT
      if (viaBt && !a2o::busy()) {                 // A2DP session ended
        viaBt = false;
#ifndef DASHIE_NO_SPEAKER
        if (a2o::result() != a2o::Res::OK && playBuf) {   // kit not reachable: use the internal speaker
          audSpk();
          M5.Speaker.setVolume(VOL_LV[cfg.vol]);
          M5.Speaker.playWav(playBuf, playLen);
          playStart = now;
        }
#endif
      }
#endif
      bool speaking = playBuf && audioActive();
      if (speaking) {
        // mouth follows the loudness of the audio being played
        size_t idx = (size_t)(now - playStart) * playRate / 1000;
#ifdef USE_A2DP_OUT
        if (viaBt) idx = a2o::played();
#endif
        size_t total = playDataLen / 2;
        const int16_t* pcm = (const int16_t*)(playBuf + playDataOff);
        double s2 = 0; int n = 0;
        for (size_t i = idx; i < idx + 240 && i < total; i++) { s2 += (double)pcm[i] * pcm[i]; n++; }
        float rmsv = n ? sqrtf((float)(s2 / n)) : 0;
        float target = audioSounding() ? constrain(rmsv / 5000.0f, 0.0f, 1.0f) : 0.0f;
        mouthOpen += (target - mouthOpen) * 0.5f;
      } else {
        mouthOpen = 0;
        if (playBuf) { stopPlayback(); audMic(); }
        if (now - stSince >= replyHoldMs) toIdle(now);
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
#if HUB_ENABLED
      if (left <= 0 && !crashAlertSent && hub::isCar()) { crashAlertSent = true; hub::alert(-1); }   // nobody answered "Are you OK?"
#endif
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
    if (!shiftPocket(now)) {                        // pocket mode: nothing to draw
      cv.fillScreen(col().bg);
      if (st == St::CRASH) renderCrash(now);
      else if (st == St::IDLE) renderScreen(now);
      else renderOverlay(now);
      shift::busLock();                             // display and SD card share the SPI bus on Core2
      cv.pushSprite(0, 0);
      shift::busUnlock();
    }
  }

  if (!got) delay(1);
}
