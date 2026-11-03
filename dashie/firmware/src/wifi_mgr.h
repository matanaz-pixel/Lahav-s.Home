#pragma once
// ---------------------------------------------------------------------------
// Wi-Fi manager: up to 4 networks (config.h: WIFI_SSID[_2.._4]). Scans when disconnected, joins the STRONGEST
// configured network, retries with backoff (3 s ... 60 s). Meant for a pocket device that sits on the kitchen
// Wi-Fi at work and on the phone hotspot in the car. One network = the old behaviour (join it, retry with backoff).
// Header-only; everything runs from the Arduino loop() (no extra task). Scans are paused with hold(true)
// (e.g. while an A2DP session runs: scanning would disturb the Bluetooth stream).
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"

// WIFIMGR_SCAN=1: with several networks, scan and join the strongest one. WIFIMGR_SCAN=0: try the entries in list order.
// A2DP builds default to 0: linking the Wi-Fi scan API pulls ~2 KB more Wi-Fi driver code into IRAM, and the A2DP
// builds (Bluetooth controller + Wi-Fi) are already ~0 bytes from the IRAM limit (link error "iram0_0_seg overflowed").
#ifndef WIFIMGR_SCAN
#ifdef USE_A2DP_OUT
#define WIFIMGR_SCAN 0
#else
#define WIFIMGR_SCAN 1
#endif
#endif

namespace wifimgr {

struct Net { const char* ssid; const char* pass; };
static const Net kNets[4] = {
  {WIFI_SSID, WIFI_PASS}, {WIFI_SSID_2, WIFI_PASS_2}, {WIFI_SSID_3, WIFI_PASS_3}, {WIFI_SSID_4, WIFI_PASS_4},
};

enum class S : uint8_t { OFF, IDLE, SCANNING, CONNECTING };
static S state = S::OFF;
static uint8_t count = 0, tries = 0;
static int8_t tryIdx = -1;                   // WIFIMGR_SCAN=0: entry of kNets tried last (round robin)
static uint8_t attempts = 0;                 // WIFIMGR_SCAN=0: entries tried since the last backoff
static uint32_t nextTry = 0, since = 0;
static bool held = false;
static int8_t lastIdx = -1;                  // which entry of kNets the current / last connection uses

inline uint8_t configured() { return count; }
inline bool enabled() { return state != S::OFF; }
inline void hold(bool h) { held = h; }
inline const char* currentSsid() { return lastIdx >= 0 ? kNets[lastIdx].ssid : ""; }

inline uint32_t backoffMs() {
  uint32_t b = 3000UL << (tries > 4 ? 4 : tries);        // 3, 6, 12, 24, 48 s ...
  return b > 60000UL ? 60000UL : b;
}
inline void fail(uint32_t now) {
  if (tries < 8) tries++;
  nextTry = now + backoffMs();
  state = S::IDLE;
}

inline void begin() {
  count = 0;
  for (auto& n : kNets) if (n.ssid && n.ssid[0]) count++;
  if (!count) { state = S::OFF; return; }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);              // this manager reconnects (and may switch networks)
  //                      // modem sleep between beacons: matters on a 200 mAh pocket device
  state = S::IDLE; nextTry = 0; tries = 0;
}

inline void startConnect(int idx, uint32_t now) {
  lastIdx = idx;
  WiFi.begin(kNets[idx].ssid, kNets[idx].pass);
  state = S::CONNECTING; since = now;
}

inline void loop(uint32_t now) {
  if (state == S::OFF) return;
  if (WiFi.status() == WL_CONNECTED) {       // connected: nothing to do until it drops
    if (state != S::IDLE || tries || attempts) { state = S::IDLE; tries = 0; attempts = 0; nextTry = 0; }
    if (lastIdx < 0) {                       // joined by someone else (should not happen): find which entry it is
      String s = "";
      for (int i = 0; i < 4; i++) if (kNets[i].ssid[0] && s == kNets[i].ssid) { lastIdx = i; break; }
    }
    return;
  }
  if (held) return;
  switch (state) {
    case S::IDLE:
      if ((int32_t)(now - nextTry) < 0) return;
      
#if WIFIMGR_SCAN
      if (count > 1) {
        if (WiFi.scanNetworks(true /*async*/, false /*hidden*/) == WIFI_SCAN_FAILED) { fail(now); return; }
        state = S::SCANNING; since = now;
        break;
      }
#endif
      {                                      // one network, or no scan: next configured entry in list order
        int idx = tryIdx;
        for (int k = 0; k < 4; k++) { idx = (idx + 1) % 4; if (kNets[idx].ssid[0]) break; }
        tryIdx = idx;
        startConnect(idx, now);
      }
      break;
#if WIFIMGR_SCAN
    case S::SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) { if (now - since > 12000) { WiFi.scanDelete(); fail(now); } return; }
      if (n < 0) { WiFi.scanDelete(); fail(now); return; }
      int best = -1, bestRssi = -1000;
      for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        for (int j = 0; j < 4; j++) {
          if (kNets[j].ssid[0] && s == kNets[j].ssid && WiFi.RSSI(i) > bestRssi) { best = j; bestRssi = WiFi.RSSI(i); }
        }
      }
      WiFi.scanDelete();
      if (best < 0) { fail(now); return; }   // none of our networks is in range: try again later
      startConnect(best, now);
      break;
    }
#endif
    case S::CONNECTING:
      if (now - since > (WIFIMGR_SCAN || count == 1 ? 12000UL : 8000UL)) {
        
        if (!WIFIMGR_SCAN && count > 1 && ++attempts < count) { state = S::IDLE; nextTry = now; }   // next entry right away
        else { attempts = 0; fail(now); }                                                           // whole list failed: back off
      }
      break;
    default: break;
  }
}

}  // namespace wifimgr
