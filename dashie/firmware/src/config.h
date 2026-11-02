#pragma once
// ---------------------------------------------------------------------------
// Dashie configuration. Edit before flashing.
// ---------------------------------------------------------------------------

// Phone hotspot credentials (leave SSID empty to run fully offline).
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// Voice bridge endpoint, e.g. "http://192.168.43.10:8787/voice" (empty = no assistant).
// https:// URLs are accepted (certificate is NOT verified; fine for a LAN bridge).
#ifndef BRIDGE_URL
#define BRIDGE_URL ""
#endif

// POSIX TZ string used for the clock (NTP over Wi-Fi). Default: Israel.
#ifndef TZ_STRING
#define TZ_STRING "IST-2IDT,M3.4.4/26,M10.5.0"
#endif

// Maps 20*log10(rms+1) to a rough "dB SPL" number on the Noise screen.
// The mic is NOT calibrated: tune this against a phone dB meter.
#ifndef DB_OFFSET
#define DB_OFFSET 10.0f
#endif

// ---- A2DP output to the car hands-free kit (env core2_a2dp only; needs Bluetooth Classic) ----
// Bluetooth name of the kit as seen in a scan. Unknown? Put the kit in pairing mode, open
// Settings > Test kit; if the name is not found Dashie lists the devices it saw and you pick one.
// The picked name is stored in Preferences and overrides this value.
#ifndef A2DP_TARGET_NAME
#define A2DP_TARGET_NAME "MyCarKit"
#endif
#ifndef A2DP_CONNECT_TIMEOUT_MS
#define A2DP_CONNECT_TIMEOUT_MS 6000
#endif
#ifndef A2DP_SCAN_MS                       // discovery time of a Test-kit run (fills the pick list)
#define A2DP_SCAN_MS 13000
#endif
#ifndef A2DP_STREAM_START_TIMEOUT_MS       // after "connected": wait for the media stream to start
#define A2DP_STREAM_START_TIMEOUT_MS 8000
#endif
#ifndef A2DP_MIN_FREE_HEAP                 // internal RAM needed before bringing up Bluetooth (bytes).
#define A2DP_MIN_FREE_HEAP 90000           // A guess: read the serial log ("a2dp: free internal") and tune.
#endif
#ifndef A2DP_GAIN_Q15                      // output gain (Q15, 22000 ~ 0.67) = headroom against clipping
#define A2DP_GAIN_Q15 22000
#endif
#ifndef A2DP_RELEASE_BLE                   // 1 = esp_bt_controller_mem_release(BLE) before first start.
#define A2DP_RELEASE_BLE 0                 // OFF: the prebuilt Arduino core runs BTDM, unverified on hardware
#endif

// Uncomment to enable an Edge Impulse keyword model (see trigger.h).
// #define USE_EI_KWS

// Timing / limits
#define MIC_RATE        16000
#define MAX_REC_MS      6000
#define END_SILENCE_MS  900
#define IDLE_DIM_MS     20000
#define BRIDGE_TIMEOUT  12000
#define RESP_MAX_BYTES  (16 * 1024)    // JSON reply from bridge
#ifndef AUDIO_MAX_BYTES
#define AUDIO_MAX_BYTES (600 * 1024)   // max WAV downloaded from audio_url (PSRAM)
#endif

// ---- Button-only boards (M5StickC Plus2: env plus2 / plus2_a2dp) ----
// DASHIE_BUTTONS_ONLY : no touch screen; input = BtnA (front), BtnB (side), BtnPWR (side). See README.
// DASHIE_NO_SPEAKER   : only a buzzer. Replies are spoken through A2DP (plus2_a2dp) or shown as text.
// DASHIE_ROTATION     : landscape rotation for the 135x240 panel (1 or 3). Flip if the buttons end up on the wrong side.
#ifndef DASHIE_ROTATION
#define DASHIE_ROTATION 1
#endif
// IMU mounting: which sensor axis (gravity already removed: lx, ly, lz in g) is "lateral" and "forward"
// in the vehicle. Core2/CoreS3 default (upright, screen to the driver): lateral = lx, forward = -lz.
// Plus2 (env sets DASHIE_IMU_LAT_EXPR=ly) is a best guess: verify on the G-Force screen and flip the sign if needed.
#ifndef DASHIE_IMU_LAT_EXPR
#define DASHIE_IMU_LAT_EXPR (lx)
#endif
#ifndef DASHIE_IMU_FWD_EXPR
#define DASHIE_IMU_FWD_EXPR (-lz)
#endif

// Driving-event thresholds (in g)
#define BRAKE_G         0.45f
#define TURN_G          0.45f
#define CRASH_G         4.0f
#define CRASH_COUNTDOWN_S 15

// ---------------------------------------------------------------------------
// Shift mode (end-of-shift summary). See ../../SPEC_SHIFT.md and README "Shift mode".
// Build with -DSHIFT_ENABLED=0 to compile it out completely (behaviour is then identical to before).
// ---------------------------------------------------------------------------
#ifndef SHIFT_ENABLED
#define SHIFT_ENABLED 1
#endif
// Bearer token sent to the bridge on every /shift/* call (must equal BRIDGE_TOKEN on the server; empty = none).
// Generic name: later bridge features (hub) use the same token.
#ifndef BRIDGE_TOKEN
#define BRIDGE_TOKEN ""
#endif
#ifndef SHIFT_BRIDGE_TOKEN                 // kept for older configs
#define SHIFT_BRIDGE_TOKEN BRIDGE_TOKEN
#endif
// Bridge origin for /shift/* (e.g. "http://192.168.43.10:8787"). Empty = derived from BRIDGE_URL.
#ifndef SHIFT_BRIDGE_BASE
#define SHIFT_BRIDGE_BASE ""
#endif
#ifndef SHIFT_NOTE_MAX_S                   // a note stops by itself after this long
#define SHIFT_NOTE_MAX_S 90
#endif
#ifndef SHIFT_NOTE_MIN_MS                  // shorter presses are discarded (accidents / "peek" double press)
#define SHIFT_NOTE_MIN_MS 1000
#endif
#ifndef SHIFT_POCKET_MS                    // no input for this long during a shift => screen off, touch ignored
#define SHIFT_POCKET_MS 10000
#endif
#ifndef SHIFT_LOW_BATT_PCT
#define SHIFT_LOW_BATT_PCT 20
#endif
#ifndef SHIFT_FINISH_WAIT_MS               // end of shift: how long the UI waits for the summary before detaching.
#define SHIFT_FINISH_WAIT_MS 130000        // The bridge needs up to ~90 s (LLM + TTS + e-mail): keep this above that.
#endif
#ifndef SHIFT_FINISH_HTTP_MS               // HTTP timeout of POST /shift/finish. HTTPClient stores it in a uint16_t (max 65535 ms),
#define SHIFT_FINISH_HTTP_MS 65000         // so a slower bridge answer is handled by re-sending the request (see shift.cpp)
#endif
#if SHIFT_FINISH_HTTP_MS > 65535
#error "SHIFT_FINISH_HTTP_MS must be <= 65535 (HTTPClient::setTimeout takes uint16_t)"
#endif
// The spoken summary arrives as several WAV chunks (audio_urls, ~13 s each). They are merged into ONE buffer so
// the A2DP kit connects once. Budget in PSRAM; chunks beyond it are not spoken (the text is still shown).
#ifndef SHIFT_AUDIO_MAX_BYTES
#ifdef DASHIE_BUTTONS_ONLY
#define SHIFT_AUDIO_MAX_BYTES (1000 * 1024)   // Plus2: 2 MB PSRAM (~30 s)
#else
#define SHIFT_AUDIO_MAX_BYTES (2500 * 1024)   // Core2 / CoreS3: 8 MB PSRAM (~75 s)
#endif
#endif
// 1 = read the 4 wrap-up questions aloud from GET <base>/shift/question/<n>.wav?lang=he|en. The current bridge has
// no such endpoint, so this is off: the questions are shown on screen with a vibration.
#ifndef SHIFT_SPOKEN_QUESTIONS
#define SHIFT_SPOKEN_QUESTIONS 0
#endif
// Plus2 has no SD slot: notes are stored in LittleFS (custom partition table, see platformio.ini) at 8 kHz.
// Core2 / CoreS3 store 16 kHz notes on the SD card (LittleFS fallback when no card is inserted).
