#pragma once
// ---------------------------------------------------------------------------
// Shift mode: voice notes during a work shift -> queue -> bridge -> summary.
// Pure logic (storage, streaming note writer, upload queue, /shift/finish). All UI and audio
// routing stays in main.cpp. Compiled out with -DSHIFT_ENABLED=0 (stubs below keep main.cpp unchanged).
// Contract with the bridge: ../../SPEC_SHIFT.md and README "Shift mode".
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <vector>
#include "config.h"

namespace shift {

enum Backend : uint8_t { BK_NONE, BK_SD, BK_LFS };
enum NoteEnd : uint8_t { NE_OK, NE_TOO_SHORT, NE_ERROR };
enum FinState : uint8_t { FS_NONE, FS_WAIT, FS_DONE, FS_NEED, FS_ERR };

struct Status {
  bool active = false;          // shift running (notes can be recorded)
  bool finishing = false;       // end requested, waiting for uploads / summary
  bool online = false;
  Backend backend = BK_NONE;
  uint32_t startedAt = 0;       // epoch UTC (0 = clock was not set)
  uint16_t notes = 0;           // notes kept in this shift
  uint16_t pending = 0;         // notes not yet acknowledged by the bridge
  int32_t freeSec = -1;         // seconds of audio that still fit (-1 = unknown, e.g. SD card)
  int16_t lastHttp = 0;         // last HTTP code / error of the uploader (0 = none)
  bool writeErr = false;        // a note could not be written (storage full / card error)
  uint32_t overruns = 0;        // audio frames dropped because the writer could not keep up
  bool summaryReady = false;    // a finished summary arrived while the UI was not waiting for it
};

struct Result {
  FinState state = FS_NONE;
  String spoken, summary, emailInfo;
  std::vector<String> audioUrls;     // spoken summary, WAV chunks in playback order
  std::vector<String> questions;     // FS_NEED: follow-up questions
  int16_t http = 0;
};

#if SHIFT_ENABLED

void begin();                          // after configTzTime(): RTC -> system clock, restore a running shift
void loop(uint32_t now);               // cheap, call every loop(): RTC write-back, health flags
bool active();
bool finishing();
bool recording();
bool start();                          // false = no storage
Status status();
void hold(bool h);                     // true = do not upload now (recording / assistant / A2DP busy)

bool noteBegin(bool answer = false);   // answer = reply to a follow-up question (X-Kind: answer)
void noteFeed(const int16_t* pcm16k, size_t n);   // 16 kHz mono; decimated to 8 kHz on LittleFS boards
NoteEnd noteEnd(uint32_t* durMs);      // flushes + closes the file; the note is queued for upload

// End of shift. interactive = the UI waits for the answer (follow-up questions allowed).
bool finishBegin(bool interactive);
void finishDetach();                   // UI stops waiting: remaining rounds run with final=true
FinState finishState();
bool takeResult(Result& out);          // FS_DONE / FS_NEED / FS_ERR: copies and resets to FS_NONE

bool hasLastSummary();
bool lastSummary(String& id, String& spoken, std::vector<String>& audioUrls);
bool questionUrl(int n, char* out, size_t cap);   // "<base>/shift/question/<n>.wav?lang=.." ("" base = no bridge)
void setLang(uint8_t lang);            // 0 EN, 1 HE (sent to the bridge)

void busLock();                        // display / SD share one SPI bus on Core2: take it around pushSprite
void busUnlock();

#else  // ---- compiled out: no-op stubs ----

inline void begin() {}
inline void loop(uint32_t) {}
inline bool active() { return false; }
inline bool finishing() { return false; }
inline bool recording() { return false; }
inline bool start() { return false; }
inline Status status() { return Status(); }
inline void hold(bool) {}
inline bool noteBegin(bool = false) { return false; }
inline void noteFeed(const int16_t*, size_t) {}
inline NoteEnd noteEnd(uint32_t* d) { if (d) *d = 0; return NE_ERROR; }
inline bool finishBegin(bool) { return false; }
inline void finishDetach() {}
inline FinState finishState() { return FS_NONE; }
inline bool takeResult(Result&) { return false; }
inline bool hasLastSummary() { return false; }
inline bool lastSummary(String&, String&, std::vector<String>&) { return false; }
inline bool questionUrl(int, char*, size_t) { return false; }
inline void setLang(uint8_t) {}
inline void busLock() {}
inline void busUnlock() {}

#endif
}  // namespace shift
