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

// Uncomment to enable an Edge Impulse keyword model (see trigger.h).
// #define USE_EI_KWS

// Timing / limits
#define MIC_RATE        16000
#define MAX_REC_MS      6000
#define END_SILENCE_MS  900
#define IDLE_DIM_MS     20000
#define BRIDGE_TIMEOUT  12000
#define RESP_MAX_BYTES  (256 * 1024)

// Driving-event thresholds (in g)
#define BRAKE_G         0.45f
#define TURN_G          0.45f
#define CRASH_G         4.0f
#define CRASH_COUNTDOWN_S 15
