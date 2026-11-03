#pragma once
// ---------------------------------------------------------------------------
// Hub: car <-> pocket coordination through the bridge. Contract: bridge/hub.py + bridge/README.md "Hub".
//   POST /hub/hello {device_id, role, ssid, battery, notes_pending, shift_active, fw}
//        -> {poll_s, in_car, commands:[{id,type,...}], peer:{...}|null}      (heartbeat AND poll)
//   POST /hub/ack   {device_id, command_id}    (sent when the command is ACCEPTED, not when it is done; 404 = expired)
//   POST /hub/alert {device_id, type:"crash", g, ts}
// A single worker task does all the network I/O and never touches M5 / display / I2C: the main loop feeds it the
// values it reports (setLocal) and picks up commands (next/accept). Compiled out with -DHUB_ENABLED=0 (stubs below).
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <vector>
#include "config.h"

namespace hub {

enum class CmdType : uint8_t { NONE, READ_SUMMARY, VIBRATE, FIND_ME };

struct Cmd {
  char id[24] = {0};
  CmdType type = CmdType::NONE;
  char pattern[12] = {0};            // vibrate: "crash"
  float g = -1;                      // vibrate: peak g reported by the car (-1 = unknown)
  uint16_t durationS = 30;           // find_me
  uint32_t deadlineMs = 0;           // millis() after which the command is stale (set by the hub)
  String shiftId, spoken;            // read_summary
  std::vector<String> audioUrls;     // read_summary: WAV chunks (signed URLs) in playback order
};

struct Peer {
  bool valid = false;                // the bridge knows a device with the other role
  int8_t battery = -1;               // %, -1 unknown
  int32_t notes = -1;                // notes waiting to upload, -1 unknown
  int8_t shiftActive = -1;           // 1 / 0 / -1 unknown
  uint32_t lastSeenS = 0;            // age on the bridge at the time of the last hello
  uint32_t receivedMs = 0;           // millis() of that hello (so the age keeps counting on screen)
};

#if HUB_ENABLED

void begin();                                   // creates the worker task (idle until Wi-Fi + bridge are configured)
const char* deviceId();                         // "car-AABBCCDDEEFF" / "pkt-AABBCCDDEEFF" (unique per chip)
bool isCar();
// Called from the main loop (about once a second): values for the heartbeat, and "do not poll now".
void setLocal(int battery, int notesPending, bool shiftActive);
void hold(bool h);                              // true = recording a note / A2DP busy: no heartbeat now
bool online();                                  // last hello succeeded < 3 poll intervals ago
bool inCar();                                   // the bridge says: both devices are on the car hotspot
Peer peer();                                    // copy of the latest peer info
// Commands: next() copies the oldest pending command (drops expired ones). Call accept(id) when you start handling
// it: that removes it, remembers the id (a redelivery is not run twice) and sends the ack. If you are busy, do
// nothing: it stays pending, un-acked, and the bridge redelivers (up to 3 times, then it expires after 10 min).
bool next(Cmd& out);
void accept(const char* id);
// Crash alert from the car. ts identifies the event (de-duplication on the bridge); retried for HUB_ALERT_RETRY_S.
void alert(float g);

#else

inline void begin() {}
inline const char* deviceId() { return ""; }
inline bool isCar() { return !HUB_ROLE_POCKET; }
inline void setLocal(int, int, bool) {}
inline void hold(bool) {}
inline bool online() { return false; }
inline bool inCar() { return false; }
inline Peer peer() { return Peer(); }
inline bool next(Cmd&) { return false; }
inline void accept(const char*) {}
inline void alert(float) {}

#endif
}  // namespace hub
