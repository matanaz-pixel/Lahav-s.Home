#pragma once
// ---------------------------------------------------------------------------
// Shared helpers for talking to the Dashie bridge (used by shift.cpp; the planned hub module will use the same).
// Header-only so a new module just includes it. Nothing here knows about shift mode.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include "config.h"

namespace bridge {

// Origin of the bridge ("http://host:port"): SHIFT_BRIDGE_BASE if set, else derived from BRIDGE_URL.
inline String base() {
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

// Caller deletes the returned client. https => certificate is NOT verified (same as the /voice client).
inline WiFiClient* newClient(const String& url) {
  if (url.startsWith("https")) { auto* c = new WiFiClientSecure(); c->setInsecure(); return c; }
  return new WiFiClient();
}

inline void addAuth(HTTPClient& h) {
  if (strlen(BRIDGE_TOKEN)) h.addHeader("Authorization", String("Bearer ") + BRIDGE_TOKEN);
}

// ISO 8601 with numeric offset in local time ("2026-11-02T22:15:00+02:00"): what the bridge parses.
// epoch 0 (clock never set) -> "".
inline String isoLocal(uint32_t epoch) {
  if (!epoch) return "";
  time_t t = epoch; struct tm l; localtime_r(&t, &l);
  char buf[40], z[8];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &l);
  strftime(z, sizeof(z), "%z", &l);                       // "+0200"
  String out = buf;
  out += String(z).substring(0, 3) + ":" + String(z).substring(3, 5);
  return out;
}

}  // namespace bridge
