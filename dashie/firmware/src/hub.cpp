// Hub worker: heartbeat/poll, commands, acks, crash alert. See hub.h.
#include "config.h"
#if HUB_ENABLED

#include "hub.h"
#include "bridge_net.h"
#include "wifi_mgr.h"
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <time.h>

namespace hub {

static constexpr size_t RESP_CAP = 10 * 1024;          // read_summary carries up to ~8 signed URLs + the spoken text
static constexpr int HANDLED_N = 16;
static constexpr int ACKQ_N = 6;
static constexpr int CMDQ_N = 4;
static constexpr uint32_t ACK_RETRY_MS = 3000;

static SemaphoreHandle_t mx = nullptr;
static char devId[28] = "";

// ---- written by the main loop (setLocal / hold), read by the task ----
static volatile int gBatt = -1, gNotes = -1;
static volatile bool gShift = false, gHold = false;

// ---- shared state (under mx) ----
static std::vector<Cmd> cmdq;                           // pending, in arrival order
static char handled[HANDLED_N][24];                     // ring of accepted command ids
static uint8_t handledIdx = 0;
struct PendingAck { char id[24]; uint8_t tries; uint32_t due; };
static PendingAck ackq[ACKQ_N]; static uint8_t ackN = 0;
static Peer gPeer;
static volatile bool gInCar = false;
static volatile uint32_t lastOkMs = 0, gPollMs = 0;

// ---- crash alert ----
static volatile bool alertWant = false;
static char alertTs[40] = ""; static float alertG = -1;
static uint32_t alertFirst = 0, alertNext = 0; static uint16_t alertTries = 0;

struct Lock { Lock() { xSemaphoreTake(mx, portMAX_DELAY); } ~Lock() { xSemaphoreGive(mx); } };

const char* deviceId() { return devId; }
bool isCar() { return !HUB_ROLE_POCKET; }
void setLocal(int battery, int notesPending, bool shiftActive) { gBatt = battery; gNotes = notesPending; gShift = shiftActive; }
void hold(bool h) { gHold = h; }
bool inCar() { return gInCar; }
bool online() {
  uint32_t ok = lastOkMs;
  return ok && (millis() - ok) < 3UL * (gPollMs ? (uint32_t)gPollMs : (uint32_t)HUB_POLL_FAST_S * 1000UL) + 5000UL;
}
Peer peer() { Lock l; return gPeer; }

static bool wasHandled(const char* id) {               // caller holds the lock
  for (int i = 0; i < HANDLED_N; i++) if (handled[i][0] && strcmp(handled[i], id) == 0) return true;
  return false;
}
static void queueAck(const char* id) {                 // caller holds the lock
  for (int i = 0; i < ackN; i++) if (strcmp(ackq[i].id, id) == 0) return;
  if (ackN >= ACKQ_N) { memmove(&ackq[0], &ackq[1], sizeof(PendingAck) * (ACKQ_N - 1)); ackN = ACKQ_N - 1; }
  PendingAck& a = ackq[ackN++];
  strlcpy(a.id, id, sizeof(a.id)); a.tries = 0; a.due = 0;
}

bool next(Cmd& out) {
  Lock l;
  uint32_t now = millis();
  for (size_t i = 0; i < cmdq.size();) {               // drop stale commands (bridge TTL is 10 min)
    if ((int32_t)(now - cmdq[i].deadlineMs) >= 0) cmdq.erase(cmdq.begin() + i); else ++i;
  }
  if (cmdq.empty()) return false;
  out = cmdq.front();
  return true;
}

void accept(const char* id) {
  Lock l;
  for (size_t i = 0; i < cmdq.size(); i++) {
    if (strcmp(cmdq[i].id, id) == 0) { cmdq.erase(cmdq.begin() + i); break; }
  }
  if (!wasHandled(id)) { strlcpy(handled[handledIdx], id, sizeof(handled[0])); handledIdx = (handledIdx + 1) % HANDLED_N; }
  queueAck(id);                                        // ack at the START of handling, not at the end
}

void alert(float g) {
  time_t t = time(nullptr);
  String ts = (t > 1700000000) ? bridge::isoLocal((uint32_t)t) : String("m") + String(millis() / 1000);
  strlcpy(alertTs, ts.c_str(), sizeof(alertTs));
  alertG = g; alertFirst = millis(); alertNext = 0; alertTries = 0;
  alertWant = true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
// POST json. Returns the HTTP code (-1 = no connection). If resp != nullptr the body is read into it (<= cap-1 bytes).
static int postJson(const String& path, const String& body, char* resp, size_t cap, size_t* respLen) {
  String url = bridge::base() + path;
  WiFiClient* cl = bridge::newClient(url);
  HTTPClient http;
  http.setTimeout(8000); http.setConnectTimeout(5000); http.useHTTP10(true);
  int code = -1;
  if (http.begin(*cl, url)) {
    http.addHeader("Content-Type", "application/json");
    bridge::addAuth(http);
    code = http.POST((uint8_t*)body.c_str(), body.length());
    if (resp && code > 0) {
      WiFiClient* s = http.getStreamPtr();
      size_t n = 0; uint32_t last = millis();
      while (n < cap - 1 && (http.connected() || s->available()) && millis() - last < 4000) {
        int a = s->available();
        if (a > 0) { n += s->readBytes(resp + n, min((size_t)a, cap - 1 - n)); last = millis(); }
        else vTaskDelay(5 / portTICK_PERIOD_MS);
      }
      resp[n] = 0;
      if (respLen) *respLen = n;
    }
    http.end();
  }
  delete cl;
  return code;
}

static CmdType typeOf(const char* s) {
  if (!strcmp(s, "read_summary")) return CmdType::READ_SUMMARY;
  if (!strcmp(s, "vibrate")) return CmdType::VIBRATE;
  if (!strcmp(s, "find_me")) return CmdType::FIND_ME;
  return CmdType::NONE;
}

static bool hello() {
  char* resp = (char*)heap_caps_malloc(RESP_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!resp) resp = (char*)heap_caps_malloc(RESP_CAP, MALLOC_CAP_8BIT);
  if (!resp) return false;
  JsonDocument d;
  d["device_id"] = devId;
  d["role"] = isCar() ? "car" : "pocket";
  d["ssid"] = wifimgr::currentSsid();
  if (gBatt >= 0) d["battery"] = (int)gBatt;
  if (gNotes >= 0) d["notes_pending"] = (int)gNotes;
  d["shift_active"] = (bool)gShift;
  d["fw"] = HUB_FW;
  String body; serializeJson(d, body);

  size_t n = 0;
  int code = postJson("/hub/hello", body, resp, RESP_CAP, &n);
  bool ok = false;
  if (code == 200 && n) {
    JsonDocument r;
    if (!deserializeJson(r, resp, n)) {
      uint32_t now = millis();
      int ps = r["poll_s"] | 0;
      if (ps >= 3) gPollMs = (uint32_t)min<int>(ps, 300) * 1000UL;
      gInCar = r["in_car"] | false;
      Lock l;
      JsonObject pr = r["peer"];
      if (!pr.isNull()) {
        gPeer.valid = true;
        gPeer.battery = pr["battery"].is<int>() ? (int8_t)constrain((int)pr["battery"], 0, 100) : -1;
        gPeer.notes = pr["notes_pending"].is<int>() ? (int32_t)(int)pr["notes_pending"] : -1;
        gPeer.shiftActive = pr["shift_active"].is<bool>() ? (pr["shift_active"].as<bool>() ? 1 : 0) : -1;
        gPeer.lastSeenS = pr["last_seen_s"] | 0;
        gPeer.receivedMs = now;
      } else gPeer = Peer();
      for (JsonObject c : r["commands"].as<JsonArray>()) {
        const char* id = c["id"] | "";
        CmdType ty = typeOf(c["type"] | "");
        if (!id[0] || strlen(id) >= sizeof(Cmd::id)) continue;
        if (wasHandled(id)) { queueAck(id); continue; }               // our ack got lost: confirm again, do not rerun
        bool dup = false;
        for (auto& q : cmdq) if (strcmp(q.id, id) == 0) { dup = true; break; }
        if (dup) continue;
        const bool mine = (ty == CmdType::READ_SUMMARY && isCar()) || ((ty == CmdType::VIBRATE || ty == CmdType::FIND_ME) && !isCar());
        if (!mine) {                                                  // unknown type / wrong role: confirm and ignore
          if (!wasHandled(id)) { strlcpy(handled[handledIdx], id, sizeof(handled[0])); handledIdx = (handledIdx + 1) % HANDLED_N; }
          queueAck(id);
          continue;
        }
        if ((int)cmdq.size() >= CMDQ_N) continue;
        Cmd cm; strlcpy(cm.id, id, sizeof(cm.id)); cm.type = ty;
        int exp = c["expires_in_s"] | 600;
        cm.deadlineMs = now + (uint32_t)max<int>(exp - 2, 5) * 1000UL;
        if (ty == CmdType::READ_SUMMARY) {
          cm.shiftId = String((const char*)(c["shift_id"] | ""));
          cm.spoken = String((const char*)(c["spoken"] | ""));
          for (JsonVariant u : c["audio_urls"].as<JsonArray>()) { const char* us = u | ""; if (us[0]) cm.audioUrls.push_back(String(us)); }
        } else if (ty == CmdType::VIBRATE) {
          strlcpy(cm.pattern, c["pattern"] | "crash", sizeof(cm.pattern));
          cm.g = c["g"].is<float>() ? (float)c["g"] : -1.0f;
        } else {
          cm.durationS = (uint16_t)constrain((int)(c["duration_s"] | 30), 3, (int)HUB_FIND_MAX_S);
        }
        cmdq.push_back(cm);
      }
      lastOkMs = now;
      ok = true;
    }
  }
  heap_caps_free(resp);
  return ok;
}

static void doAcks(uint32_t now) {
  PendingAck a; bool have = false;
  { Lock l; for (int i = 0; i < ackN; i++) if ((int32_t)(now - ackq[i].due) >= 0) { a = ackq[i]; have = true; break; } }
  if (!have) return;
  String body = String("{\"device_id\":\"") + devId + "\",\"command_id\":\"" + a.id + "\"}";
  int code = postJson("/hub/ack", body, nullptr, 0, nullptr);
  Lock l;
  for (int i = 0; i < ackN; i++) {
    if (strcmp(ackq[i].id, a.id) != 0) continue;
    // 200 ok, 404 = expired / unknown (stop trying), 400/401 = will never succeed
    bool done = code == 200 || code == 404 || code == 400 || code == 401 || ackq[i].tries >= 5;
    if (done) { memmove(&ackq[i], &ackq[i + 1], sizeof(PendingAck) * (ackN - i - 1)); ackN--; }
    else { ackq[i].tries++; ackq[i].due = now + ACK_RETRY_MS; }
    break;
  }
}

static void sendAlertOnce(uint32_t now) {
  JsonDocument d;
  d["device_id"] = devId; d["type"] = "crash"; d["ts"] = alertTs;
  if (alertG >= 0) d["g"] = alertG;
  String body; serializeJson(d, body);
  int code = postJson("/hub/alert", body, nullptr, 0, nullptr);
  alertTries++;
  if (code == 200 || (code >= 400 && code < 500 && code != 408 && code != 429)) { alertWant = false; return; }   // sent / never will
  if (now - alertFirst > (uint32_t)HUB_ALERT_RETRY_S * 1000UL) { alertWant = false; return; }                    // give up
  alertNext = now + (alertTries < 5 ? 2000 : 5000);
}

static void task(void*) {
  bool wasConn = false; uint32_t nextHello = 0, fails = 0;
  for (;;) {
    vTaskDelay(200 / portTICK_PERIOD_MS);
    uint32_t now = millis();
    if (WiFi.status() != WL_CONNECTED) { wasConn = false; continue; }     // wifimgr (main loop) reconnects
    if (!wasConn) { wasConn = true; nextHello = now; }
    if (!bridge::base().length()) continue;
    if (alertWant && (int32_t)(now - alertNext) >= 0) sendAlertOnce(now); // alerts ignore hold()
    doAcks(now);
    if (!gHold && (int32_t)(now - nextHello) >= 0) {
      bool ok = hello();
      uint32_t interval = gPollMs ? (uint32_t)gPollMs : (uint32_t)HUB_POLL_FAST_S * 1000UL;
      if (!isCar()) {                                                    // pocket away from the car hotspot: save battery
        bool onCar = HUB_CAR_SSID[0] && strcmp(wifimgr::currentSsid(), HUB_CAR_SSID) == 0;
        if (!onCar) interval = max<uint32_t>(interval, (uint32_t)HUB_POLL_SLOW_S * 1000UL);
      }
      if (ok) fails = 0; else if (fails < 4) fails++;
      nextHello = millis() + (ok ? interval : min<uint32_t>(interval << fails, 60000UL));
    }
  }
}

void begin() {
  mx = xSemaphoreCreateMutex();
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(devId, sizeof(devId), "%s-%02X%02X%02X%02X%02X%02X", HUB_ROLE_POCKET ? "pkt" : "car", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  xTaskCreatePinnedToCore(task, "hub", 10240, nullptr, 1, nullptr, 0);
}

}  // namespace hub
#endif
