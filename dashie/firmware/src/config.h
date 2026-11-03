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

// Up to 3 more networks (e.g. kitchen Wi-Fi at work + phone hotspot in the car). Dashie scans and joins the STRONGEST
// network of the list (WIFI_SSID is entry 1). Leave empty to skip. Case-sensitive, 2.4 GHz only.
#ifndef WIFI_SSID_2
#define WIFI_SSID_2 ""
#endif
#ifndef WIFI_PASS_2
#define WIFI_PASS_2 ""
#endif
#ifndef WIFI_SSID_3
#define WIFI_SSID_3 ""
#endif
#ifndef WIFI_PASS_3
#define WIFI_PASS_3 ""
#endif
#ifndef WIFI_SSID_4
#define WIFI_SSID_4 ""
#endif
#ifndef WIFI_PASS_4
#define WIFI_PASS_4 ""
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
// 1 = read the 4 wrap-up questions aloud from GET <base>/shift/question/<n>.wav?lang=he|en (needs ELEVENLABS_API_KEY +
// ELEVENLABS_VOICE_ID on the bridge, otherwise the bridge answers 404 and the question stays text + vibration).
// Only boards that can speak (Core2/CoreS3 speaker, or Plus2 with A2DP) use it; the code falls back to text itself.
#ifndef SHIFT_SPOKEN_QUESTIONS
#define SHIFT_SPOKEN_QUESTIONS 1
#endif
// Plus2 has no SD slot: notes are stored in LittleFS (custom partition table, see platformio.ini) at 8 kHz.
// Core2 / CoreS3 store 16 kHz notes on the SD card (LittleFS fallback when no card is inserted).

// ---------------------------------------------------------------------------
// Hub: two Dashies (car + pocket) coordinate through the bridge (POST /hub/hello = heartbeat + poll).
// See bridge/README.md "Hub". Build with -DHUB_ENABLED=0 to compile it out (the Wi-Fi list stays).
//   car    (Core2 on the dashboard): reads the finished shift summary when you get in, shows the pocket device's status,
//          sends a crash alert to the pocket device.
//   pocket (Plus2): vibrates / beeps on a crash alert from the car, and on "where is my pocket device?".
// ---------------------------------------------------------------------------
#ifndef HUB_ENABLED
#define HUB_ENABLED 1
#endif
#ifndef HUB_ROLE_POCKET                    // 1 = pocket device, 0 = car device. Default: buttons-only boards (Plus2) are pocket.
#ifdef DASHIE_BUTTONS_ONLY
#define HUB_ROLE_POCKET 1
#else
#define HUB_ROLE_POCKET 0
#endif
#endif
// SSID of the phone hotspot in the car. MUST equal HUB_CAR_SSID on the bridge. The bridge decides "you are in the car"
// when both devices report this SSID. It also makes the pocket device poll fast only while it is on this network.
#ifndef HUB_CAR_SSID
#define HUB_CAR_SSID ""
#endif
#ifndef HUB_POLL_FAST_S                    // used until the bridge sends its own poll_s (car 10 s / pocket 20 s)
#define HUB_POLL_FAST_S (HUB_ROLE_POCKET ? 20 : 10)
#endif
#ifndef HUB_POLL_SLOW_S                    // pocket device away from the car hotspot: poll rarely (battery: 200 mAh!)
#define HUB_POLL_SLOW_S 60
#endif
#ifndef HUB_ALERT_ON_TIMEOUT_ONLY          // 0 = send the crash alert the moment the impact is detected (default)
#define HUB_ALERT_ON_TIMEOUT_ONLY 0        // 1 = only if the 15 s "Are you OK?" countdown ends without being cancelled
#endif
#ifndef HUB_ALERT_RETRY_S                  // keep retrying the alert this long while Wi-Fi is down / the bridge unreachable
#define HUB_ALERT_RETRY_S 90
#endif
#ifndef HUB_PALERT_MAX_S                   // pocket crash alert stops by itself after this long (battery)
#define HUB_PALERT_MAX_S 120
#endif
#ifndef HUB_FIND_MAX_S                     // cap for find_me duration_s from the bridge
#define HUB_FIND_MAX_S 60
#endif
#ifndef HUB_POCKET_NO_CRASH                // 1 = a pocket device does not run its own crash detection while a shift is active
#define HUB_POCKET_NO_CRASH 1              // (a 4 g knock in a kitchen pocket must not start a 15 s alarm). Car devices unaffected.
#endif
#ifndef HUB_FW
#define HUB_FW "dashie-hub-1"
#endif

// Buzzer-only boards (Plus2) have no speaker in the pocket / in the kitchen: wrap-up questions stay text + vibration/beeps.
#ifdef DASHIE_NO_SPEAKER
#undef SHIFT_SPOKEN_QUESTIONS
#define SHIFT_SPOKEN_QUESTIONS 0
#endif
