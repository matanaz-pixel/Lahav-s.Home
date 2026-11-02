// Shift mode logic: storage, streaming note writer, upload queue, /shift/finish. See shift.h.
#include "config.h"
#if SHIFT_ENABLED

#include "shift.h"
#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <SPI.h>
#include <LittleFS.h>
#include <esp_sntp.h>
#include <esp_heap_caps.h>
#include <time.h>

namespace shift {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Preferences sp;                       // namespace "dshift": survives reboot / battery swap
static bool spOpen = false;

static fs::FS* gfs = nullptr;                // where the notes live (SD or LittleFS)
static Backend gbk = BK_NONE;
static bool lfsMounted = false;

static char sid[28] = "";                    // shift id, empty = no shift
static uint32_t t0 = 0, t1 = 0;              // epoch UTC of start / end request
static bool gActive = false, gFin = false, gAck = false;
static uint16_t seqNext = 1, notesN = 0;
static volatile uint16_t pendingN = 0;
static uint8_t gLang = 0;
static volatile bool gHold = false;
static volatile int16_t gHttp = 0;
static volatile bool gWriteErr = false;
static volatile bool gSummaryReady = false;

static SemaphoreHandle_t busMx = nullptr;

// ---- note writer ----
static constexpr size_t RING_BYTES = 64 * 1024;
static uint8_t* ring = nullptr;
static size_t ringCap = 0;
static volatile uint32_t rHead = 0, rTail = 0;        // monotonic byte counters (producer / consumer)
static File nf;
static char ntmp[72], nfin[72];
static TaskHandle_t wTask = nullptr;
static volatile bool wRun = false, wDone = true, wErr = false, gRec = false;
static volatile uint32_t wBytes = 0, gOverruns = 0;
static uint32_t noteRate = 16000;

// ---- net task / finish ----
static TaskHandle_t netH = nullptr;
static volatile bool finWant = false, finInteractive = false, finDetached = false;
static uint8_t finRound = 0;
static volatile FinState resState = FS_NONE;
static Result res;
static uint32_t nextTry = 0, backoff = 4000;
static uint32_t lastRtcWrite = 0, lastHealth = 0;
static int32_t freeSecCache = -1;

#ifdef DASHIE_BUTTONS_ONLY
static constexpr bool kHasSdBoard = false;
#else
static constexpr bool kHasSdBoard = true;
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t nowEpoch() { time_t t = time(nullptr); return t > 1700000000 ? (uint32_t)t : 0; }

static void* bigAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  return p;
}

void busLock() { if (busMx) xSemaphoreTake(busMx, portMAX_DELAY); }
void busUnlock() { if (busMx) xSemaphoreGive(busMx); }

static void prefsOpen() { if (!spOpen) { sp.begin("dshift", false); spOpen = true; } }

static void persistShift() {
  prefsOpen();
  sp.putUChar("act", gActive ? 1 : 0);
  sp.putUChar("fin", gFin ? 1 : 0);
  sp.putString("id", sid);
  sp.putUInt("t0", t0); sp.putUInt("t1", t1);
  sp.putUShort("seq", seqNext); sp.putUShort("notes", notesN);
  sp.putBool("ack", gAck);
}

static void clearShiftState() {
  gActive = false; gFin = false; gAck = false; sid[0] = 0; t0 = t1 = 0; seqNext = 1; notesN = 0; pendingN = 0;
  finRound = 0; finWant = false; finDetached = false;
  persistShift();
}

static String baseUrl() {
  String b = SHIFT_BRIDGE_BASE;
  if (!b.length()) {
    b = BRIDGE_URL;
    int p = b.indexOf("://");
    int e = (p >= 0) ? b.indexOf('/', p + 3) : -1;
    if (e > 0) b = b.substring(0, e);
  }
  while (b.endsWith("/")) b.remove(b.length() - 1);
  return b;
}

static WiFiClient* newClient(const String& url) {
  if (url.startsWith("https")) { auto* c = new WiFiClientSecure(); c->setInsecure(); return c; }
  return new WiFiClient();
}

static void addAuth(HTTPClient& h) {
  if (strlen(SHIFT_BRIDGE_TOKEN)) h.addHeader("Authorization", String("Bearer ") + SHIFT_BRIDGE_TOKEN);
}

static void dirPath(char* out, size_t n) { snprintf(out, n, "/shift/%s", sid); }

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
static bool mountLfs() {
  if (lfsMounted) return true;
  lfsMounted = LittleFS.begin(true);      // formats once on a blank partition (a few seconds)
  return lfsMounted;
}

static bool mountSd() {
  if (!kHasSdBoard) return false;
  int sck = M5.getPin(m5::sd_spi_sclk), mosi = M5.getPin(m5::sd_spi_mosi);
  int miso = M5.getPin(m5::sd_spi_miso), cs = M5.getPin(m5::sd_spi_cs);
  if (sck < 0 || mosi < 0 || miso < 0 || cs < 0) return false;     // pin table has no SD for this board
  busLock();
  SPI.begin(sck, miso, mosi, cs);
  bool ok = SD.begin(cs, SPI, 25000000);
  if (ok && SD.cardType() == CARD_NONE) { SD.end(); ok = false; }
  busUnlock();
  return ok;
}

static bool ensureStorage() {
  if (gfs) return true;
  if (mountSd()) { gfs = &SD; gbk = BK_SD; }
  else if (mountLfs()) { gfs = &LittleFS; gbk = BK_LFS; }
  else return false;
  noteRate = (gbk == BK_SD) ? 16000 : 8000;
  busLock(); gfs->mkdir("/shift"); busUnlock();
  return true;
}

// Parses "n0007_1730000000.wav"; returns false for other names.
static bool parseNote(const char* name, bool tmp, uint16_t* seq, uint32_t* ep) {
  unsigned s = 0; unsigned long e = 0; char ext[8] = {0};
  if (sscanf(name, "n%u_%lu.%7s", &s, &e, ext) != 3) return false;
  if (strcmp(ext, tmp ? "tmp" : "wav") != 0) return false;
  *seq = (uint16_t)s; *ep = (uint32_t)e;
  return true;
}

static void wavHeader(uint8_t* b, uint32_t dataBytes, uint32_t rate) {
  uint32_t riff = 36 + dataBytes, byteRate = rate * 2, fmtLen = 16;
  uint16_t fmt = 1, ch = 1, align = 2, bits = 16;
  memcpy(b, "RIFF", 4); memcpy(b + 4, &riff, 4); memcpy(b + 8, "WAVEfmt ", 8);
  memcpy(b + 16, &fmtLen, 4); memcpy(b + 20, &fmt, 2); memcpy(b + 22, &ch, 2);
  memcpy(b + 24, &rate, 4); memcpy(b + 28, &byteRate, 4); memcpy(b + 32, &align, 2);
  memcpy(b + 34, &bits, 2); memcpy(b + 36, "data", 4); memcpy(b + 40, &dataBytes, 4);
}

// Finds the oldest queued note. Returns false when the queue is empty.
static bool oldestNote(char* path, size_t cap, uint16_t* seq, uint32_t* ep) {
  char d[48]; dirPath(d, sizeof(d));
  busLock();
  File dir = gfs->open(d);
  bool found = false; uint16_t bestSeq = 0xFFFF; uint32_t bestEp = 0; char bestName[48] = {0};
  uint16_t cnt = 0;
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      uint16_t s; uint32_t e;
      if (!f.isDirectory() && parseNote(f.name(), false, &s, &e)) {
        cnt++;
        if (s < bestSeq) { bestSeq = s; bestEp = e; strlcpy(bestName, f.name(), sizeof(bestName)); found = true; }
      }
    }
  }
  if (dir) dir.close();
  busUnlock();
  pendingN = cnt;
  if (found) { snprintf(path, cap, "%s/%s", d, bestName); *seq = bestSeq; *ep = bestEp; }
  return found;
}

// After a crash / battery pull a note may still be a ".tmp" file: give it a valid header and queue it.
static void recoverTmpFiles() {
  char d[48]; dirPath(d, sizeof(d));
  busLock();
  File dir = gfs->open(d);
  std::vector<String> names;
  if (dir && dir.isDirectory())
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      uint16_t s; uint32_t e;
      if (!f.isDirectory() && parseNote(f.name(), true, &s, &e)) names.push_back(f.name());
    }
  if (dir) dir.close();
  for (auto& n : names) {
    String tp = String(d) + "/" + n;
    File f = gfs->open(tp.c_str(), "r+");
    size_t sz = f ? f.size() : 0;
    uint32_t rate = noteRate;
    if (f && sz > 44 + rate) {                       // more than ~0.5 s of audio
      uint8_t h[44]; wavHeader(h, (uint32_t)(sz - 44), rate);
      f.seek(0); f.write(h, 44); f.close();
      String fin = tp.substring(0, tp.length() - 3) + "wav";
      gfs->rename(tp.c_str(), fin.c_str());
    } else { if (f) f.close(); gfs->remove(tp.c_str()); }
  }
  busUnlock();
}

// ---------------------------------------------------------------------------
// Note writer task: ring buffer -> file (so SD latency never stalls the microphone loop)
// ---------------------------------------------------------------------------
static void writerTask(void*) {
  for (;;) {
    uint32_t h = rHead, t = rTail;
    uint32_t avail = h - t;
    if (avail >= 4096 || (avail > 0 && !wRun)) {
      size_t off = t % ringCap;
      size_t chunk = min<size_t>(min<size_t>(avail, 8192), ringCap - off);
      busLock();
      size_t n = nf.write(ring + off, chunk);
      busUnlock();
      if (n != chunk) wErr = true;
      rTail = t + (uint32_t)chunk;
      wBytes += (uint32_t)n;
    } else if (!wRun && avail == 0) {
      break;
    } else {
      vTaskDelay(10 / portTICK_PERIOD_MS);
    }
  }
  busLock();
  if (nf) {
    uint8_t h[44]; wavHeader(h, wBytes, noteRate);
    nf.seek(0);
    if (nf.write(h, 44) != 44) wErr = true;
    nf.flush();
    nf.close();
  }
  busUnlock();
  wDone = true;
  wTask = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Network task: shift start ack -> note uploads -> finish. One task, serialized.
// ---------------------------------------------------------------------------
class HoldFileStream : public Stream {          // aborts the upload as soon as the user starts recording
 public:
  explicit HoldFileStream(File& f) : f_(f) {}
  int available() override { return (gHold || gRec) ? -1 : f_.available(); }
  int read() override { return f_.read(); }
  int peek() override { return f_.peek(); }
  size_t readBytes(char* b, size_t n) override { return f_.readBytes(b, n); }
  size_t write(uint8_t) override { return 0; }
  void flush() override {}
 private:
  File& f_;
};

static bool postStart() {
  String url = baseUrl() + "/shift/start";
  WiFiClient* cl = newClient(url);
  HTTPClient http; http.setTimeout(8000); http.setConnectTimeout(6000); http.useHTTP10(true);
  int code = -1;
  if (http.begin(*cl, url)) {
    http.addHeader("Content-Type", "application/json"); addAuth(http);
    char body[120]; snprintf(body, sizeof(body), "{\"shift_id\":\"%s\",\"started_at\":%lu}", sid, (unsigned long)t0);
    code = http.POST((uint8_t*)body, strlen(body));
    http.end();
  }
  delete cl;
  gHttp = (int16_t)code;
  return code >= 200 && code < 300;
}

static bool uploadNote(const char* path, uint16_t seq, uint32_t ep) {
  String url = baseUrl() + "/shift/note";
  busLock(); File f = gfs->open(path, "r"); busUnlock();
  if (!f) return false;
  size_t sz = f.size();
  WiFiClient* cl = newClient(url);
  HTTPClient http; http.setTimeout(20000); http.setConnectTimeout(6000); http.useHTTP10(true);
  int code = -1;
  if (http.begin(*cl, url)) {
    http.addHeader("Content-Type", "audio/wav"); addAuth(http);
    http.addHeader("X-Shift-Id", sid);
    http.addHeader("X-Seq", String(seq));
    http.addHeader("X-Time", String(ep));
    HoldFileStream hs(f);
    code = http.sendRequest("POST", &hs, sz);
    http.end();
  }
  delete cl;
  f.close();
  gHttp = (int16_t)code;
  bool ok = (code >= 200 && code < 300) || code == 409;        // 409 = bridge already has this seq
  if (ok) { busLock(); gfs->remove(path); busUnlock(); }
  return ok;
}

static void saveLast(const String& id, const String& spoken, const String& url, const String& md) {
  if (!mountLfs()) return;
  JsonDocument d; d["id"] = id; d["spoken"] = spoken; d["audio_url"] = url;
  busLock();
  File f = LittleFS.open("/dshift_last.json", "w");
  if (f) { serializeJson(d, f); f.close(); }
  File m = LittleFS.open("/dshift_last.md", "w");
  if (m) { m.print(md); m.close(); }
  busUnlock();
  prefsOpen(); sp.putBool("last", true);
}

static void removeShiftDir() {
  char d[48]; dirPath(d, sizeof(d));
  busLock(); gfs->rmdir(d); busUnlock();                // only succeeds when empty
}

static bool doFinish() {
  String url = baseUrl() + "/shift/finish";
  bool finalRound = !finInteractive || finRound >= 2;
  WiFiClient* cl = newClient(url);
  HTTPClient http; http.setTimeout(75000); http.setConnectTimeout(6000); http.useHTTP10(true);
  int code = -1;
  char* buf = (char*)bigAlloc(24 * 1024);
  size_t n = 0;
  if (buf && http.begin(*cl, url)) {
    http.addHeader("Content-Type", "application/json"); addAuth(http);
    char body[200];
    snprintf(body, sizeof(body),
             "{\"shift_id\":\"%s\",\"ended_at\":%lu,\"notes\":%u,\"final\":%s,\"round\":%u,\"lang\":\"%s\"}",
             sid, (unsigned long)t1, (unsigned)notesN, finalRound ? "true" : "false", (unsigned)(finRound + 1),
             gLang ? "he" : "en");
    code = http.POST((uint8_t*)body, strlen(body));
    if (code == 200) {
      WiFiClient* s = http.getStreamPtr();
      uint32_t last = millis();
      while (n < 24 * 1024 - 1 && (http.connected() || s->available()) && millis() - last < 8000) {
        int a = s->available();
        if (a > 0) { n += s->readBytes(buf + n, min((size_t)a, (size_t)(24 * 1024 - 1 - n))); last = millis(); }
        else vTaskDelay(5 / portTICK_PERIOD_MS);
      }
      buf[n] = 0;
    }
    http.end();
  }
  delete cl;
  gHttp = (int16_t)code;
  bool handled = false;
  if (code == 200 && n) {
    JsonDocument doc;
    if (!deserializeJson(doc, buf, n)) {
      String status = doc["status"] | "";
      if (status == "done") {
        res = Result();
        res.summary = String(doc["summary_md"] | "");
        res.spoken = String(doc["spoken"] | "");
        res.audioUrl = String(doc["audio_url"] | "");
        bool em = doc["delivered"]["email"] | false;
        String docs = String(doc["delivered"]["docs"] | "");
        res.emailInfo = String(em ? "email " : "") + (docs.length() ? "docs" : "");
        if (res.audioUrl.startsWith("/")) res.audioUrl = baseUrl() + res.audioUrl;
        saveLast(String(sid), res.spoken, res.audioUrl, res.summary);
        bool wasDetached = finDetached;
        finWant = false;
        removeShiftDir();
        clearShiftState();
        if (wasDetached) gSummaryReady = true;
        res.state = FS_DONE; resState = FS_DONE;
        handled = true;
      } else if (status == "need_info" && !finalRound) {
        res = Result();
        JsonArray qs = doc["questions"].as<JsonArray>();
        for (JsonVariant q : qs) { if (res.questions.size() < 3) res.questions.push_back(String(q.as<const char*>())); }
        finWant = false; finRound++;
        res.state = res.questions.empty() ? FS_ERR : FS_NEED; resState = res.state;
        handled = true;
      }
    }
  }
  if (buf) heap_caps_free(buf);
  if (!handled && code >= 400 && code < 500 && code != 408 && code != 429) {   // wrong token / bad request: tell the UI
    res = Result(); res.http = (int16_t)code; res.state = FS_ERR; resState = FS_ERR;
  }
  return handled;
}

static void netTask(void*) {
  for (;;) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    if (!(gActive || gFin)) break;
    if (gHold || gRec) continue;
    if (WiFi.status() != WL_CONNECTED) continue;
    if (baseUrl().length() == 0) continue;
    if (millis() < nextTry) continue;
    bool ok = true;
    if (!gAck) {
      if (postStart()) { gAck = true; persistShift(); backoff = 4000; }
      else ok = false;
    }
    if (ok) {
      char path[96]; uint16_t seq; uint32_t ep;
      bool wantFinish = gFin && finWant;
      if (pendingN > 0 || wantFinish) {                 // scan the card only when there is something to do
        if (oldestNote(path, sizeof(path), &seq, &ep)) {
          if (uploadNote(path, seq, ep)) { if (pendingN) pendingN--; backoff = 4000; }
          else ok = false;
        } else if (wantFinish) {
          if (doFinish()) backoff = 4000;
          else ok = false;
        }
      }
    }
    if (!ok) { nextTry = millis() + backoff; backoff = min<uint32_t>(backoff * 2, 300000); }
  }
  netH = nullptr;
  vTaskDelete(nullptr);
}

static void ensureNetTask() {
  if (netH) return;
  xTaskCreatePinnedToCore(netTask, "shnet", 12288, nullptr, 1, &netH, 0);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void setLang(uint8_t lang) { gLang = lang; }

void begin() {
  if (!busMx) busMx = xSemaphoreCreateMutex();
  prefsOpen();
  // Offline in a kitchen: seed the system clock from the battery-backed RTC (stored as UTC).
  if (nowEpoch() == 0) {
    m5::rtc_datetime_t dt;
    if (M5.Rtc.getDateTime(&dt) && dt.date.year >= 2024) M5.Rtc.setSystemTimeFromRtc();
  }
  gActive = sp.getUChar("act", 0) == 1;
  gFin = sp.getUChar("fin", 0) == 1;
  if (gActive || gFin) {
    String id = sp.getString("id", "");
    strlcpy(sid, id.c_str(), sizeof(sid));
    t0 = sp.getUInt("t0", 0); t1 = sp.getUInt("t1", 0);
    seqNext = sp.getUShort("seq", 1); notesN = sp.getUShort("notes", 0);
    gAck = sp.getBool("ack", false);
    if (!sid[0]) { gActive = gFin = false; }
    else if (ensureStorage()) {
      recoverTmpFiles();
      char p[96]; uint16_t s; uint32_t e; oldestNote(p, sizeof(p), &s, &e);
      if (gFin) { finWant = true; finInteractive = false; finDetached = true; }
      ensureNetTask();
    }
  }
}

bool active() { return gActive; }
bool finishing() { return gFin; }
bool recording() { return gRec; }

bool start() {
  if (gActive || gFin) return false;
  if (!ensureStorage()) return false;
  uint32_t ep = nowEpoch();
  char stamp[20] = "";
  if (ep) { time_t tt = ep; struct tm l; localtime_r(&tt, &l); strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M", &l); }
  else snprintf(stamp, sizeof(stamp), "T%lu", (unsigned long)(millis() / 1000));
  snprintf(sid, sizeof(sid), "%s-%02x", stamp, (unsigned)(esp_random() & 0xFF));
  t0 = ep; t1 = 0; seqNext = 1; notesN = 0; pendingN = 0; gAck = false; finRound = 0;
  finWant = false; finDetached = false; gSummaryReady = false; gWriteErr = false; gOverruns = 0;
  gActive = true; gFin = false;
  char d[48]; dirPath(d, sizeof(d));
  busLock(); gfs->mkdir(d); busUnlock();
  persistShift();
  ensureNetTask();
  return true;
}

Status status() {
  Status s;
  s.active = gActive; s.finishing = gFin; s.online = WiFi.status() == WL_CONNECTED;
  s.backend = gbk; s.startedAt = t0; s.notes = notesN; s.pending = pendingN;
  s.freeSec = freeSecCache; s.lastHttp = gHttp; s.writeErr = gWriteErr; s.overruns = gOverruns;
  s.summaryReady = gSummaryReady;
  return s;
}

void hold(bool h) { gHold = h; }

bool noteBegin() {
  if (!sid[0] || gRec || !wDone) return false;
  if (!ensureStorage()) { gWriteErr = true; return false; }
  if (!ring) {
    ring = (uint8_t*)bigAlloc(RING_BYTES); ringCap = ring ? RING_BYTES : 0;
    if (!ring) { gWriteErr = true; return false; }
  }
  rHead = rTail = 0; wBytes = 0; wErr = false;
  uint32_t ep = nowEpoch();
  char d[48]; dirPath(d, sizeof(d));
  snprintf(ntmp, sizeof(ntmp), "%s/n%04u_%lu.tmp", d, (unsigned)seqNext, (unsigned long)ep);
  snprintf(nfin, sizeof(nfin), "%s/n%04u_%lu.wav", d, (unsigned)seqNext, (unsigned long)ep);
  busLock();
  gfs->mkdir(d);
  nf = gfs->open(ntmp, "w");
  uint8_t h[44]; wavHeader(h, 0xFFFFFFFFu - 36, noteRate);            // size unknown until the note ends
  bool ok = nf && nf.write(h, 44) == 44;
  busUnlock();
  if (!ok) { if (nf) nf.close(); gWriteErr = true; return false; }
  wRun = true; wDone = false; gRec = true;
  if (xTaskCreatePinnedToCore(writerTask, "shw", 6144, nullptr, 2, &wTask, 0) != pdPASS) {
    gRec = false; wRun = false; wDone = true; busLock(); nf.close(); gfs->remove(ntmp); busUnlock();
    gWriteErr = true; return false;
  }
  return true;
}

void noteFeed(const int16_t* pcm, size_t n) {
  if (!gRec || !ring) return;
  int16_t tmp[512];
  const size_t step = (noteRate == 16000) ? 1 : 2;
  size_t outN = n / step;
  if (outN > 512) outN = 512;
  if (step == 1) memcpy(tmp, pcm, outN * 2);
  else for (size_t i = 0; i < outN; i++) tmp[i] = (int16_t)(((int)pcm[2 * i] + (int)pcm[2 * i + 1]) / 2);
  size_t bytes = outN * 2;
  uint32_t h = rHead, t = rTail;
  if ((size_t)(h - t) + bytes > ringCap) { gOverruns++; return; }     // writer is behind: drop this frame
  size_t off = h % ringCap, first = min(bytes, ringCap - off);
  memcpy(ring + off, tmp, first);
  if (first < bytes) memcpy(ring, ((uint8_t*)tmp) + first, bytes - first);
  rHead = h + (uint32_t)bytes;
}

NoteEnd noteEnd(uint32_t* durMs) {
  if (durMs) *durMs = 0;
  if (!gRec) return NE_ERROR;
  gRec = false; wRun = false;
  uint32_t t = millis();
  while (!wDone && millis() - t < 5000) vTaskDelay(5 / portTICK_PERIOD_MS);
  if (!wDone) { gWriteErr = true; return NE_ERROR; }
  uint32_t dur = (uint32_t)((uint64_t)wBytes * 1000 / (noteRate * 2));
  if (durMs) *durMs = dur;
  if (wErr) {
    busLock(); gfs->remove(ntmp); busUnlock();
    gWriteErr = true; return NE_ERROR;
  }
  if (dur < SHIFT_NOTE_MIN_MS) { busLock(); gfs->remove(ntmp); busUnlock(); return NE_TOO_SHORT; }
  busLock(); bool ok = gfs->rename(ntmp, nfin); busUnlock();
  if (!ok) { gWriteErr = true; return NE_ERROR; }
  seqNext++; notesN++; pendingN++;
  persistShift();
  return NE_OK;
}

bool finishBegin(bool interactive) {
  if (!sid[0]) return false;
  if (gActive) { gActive = false; gFin = true; t1 = nowEpoch(); finRound = 0; }
  else if (!gFin) return false;
  finInteractive = interactive; finDetached = !interactive;
  resState = FS_WAIT; res = Result(); res.state = FS_WAIT;
  finWant = true;
  persistShift();
  nextTry = 0;
  ensureNetTask();
  return true;
}

void finishDetach() { finInteractive = false; finDetached = true; }
FinState finishState() { return resState; }

bool takeResult(Result& out) {
  FinState s = resState;
  if (s != FS_DONE && s != FS_NEED && s != FS_ERR) return false;
  out = res;
  resState = FS_NONE;
  if (s == FS_DONE) gSummaryReady = false;
  return true;
}

bool hasLastSummary() { prefsOpen(); return sp.getBool("last", false); }   // flag only: no filesystem mount

bool lastSummary(String& id, String& spoken, String& audioUrl) {
  if (!mountLfs()) return false;
  busLock();
  File f = LittleFS.open("/dshift_last.json", "r");
  JsonDocument d; bool ok = f && !deserializeJson(d, f);
  if (f) f.close();
  busUnlock();
  if (!ok) return false;
  id = String(d["id"] | ""); spoken = String(d["spoken"] | ""); audioUrl = String(d["audio_url"] | "");
  gSummaryReady = false;
  return true;
}

bool questionUrl(int n, char* out, size_t cap) {
  String b = baseUrl();
  if (!b.length()) { if (cap) out[0] = 0; return false; }
  snprintf(out, cap, "%s/shift/question/%d.wav?lang=%s", b.c_str(), n, gLang ? "he" : "en");
  return true;
}

void loop(uint32_t now) {
  // keep the RTC (UTC) in step with NTP when the clock came from the network
  if (now - lastRtcWrite > 6UL * 3600UL * 1000UL || lastRtcWrite == 0) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED && nowEpoch()) {
      time_t t = time(nullptr); struct tm u; gmtime_r(&t, &u);
      M5.Rtc.setDateTime(&u);
      lastRtcWrite = now ? now : 1;
    }
  }
  if (now - lastHealth > 10000) {
    lastHealth = now;
    if (gbk == BK_LFS && lfsMounted) {
      busLock(); size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes(); busUnlock();
      freeSecCache = (int32_t)((total - used) / (noteRate * 2));
    } else freeSecCache = -1;
  }
}

}  // namespace shift
#endif
