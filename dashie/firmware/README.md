# Dashie firmware (M5Stack Core2 / CoreS3 / StickC Plus2)

```
pio run -e core2            # or: cores3, plus2, core2_a2dp, plus2_a2dp
pio run -e core2 -t upload
pio device monitor
```
Edit `src/config.h` first (Wi-Fi networks, `BRIDGE_URL`, timezone). **Read the IRAM warning in the Hub section before building the A2DP envs with Wi-Fi configured.**

## Boards / environments
| env | board | notes |
|---|---|---|
| `core2` | M5Stack Core2 | 320x240 touch, speaker. Reference board. |
| `cores3` | M5Stack CoreS3 | like Core2, but **no Bluetooth Classic** (no A2DP) |
| `core2_a2dp` | Core2 | + A2DP voice to the car kit |
| `plus2` | M5StickC **PLUS2** | 240x135 (landscape), **buttons only**, buzzer only: replies are text + beeps |
| `plus2_a2dp` | Plus2 | + A2DP: spoken replies go through the car hands-free kit (the only way to hear them) |

The original M5StickC PLUS (no "2") has no PSRAM and is **not supported**.

### Screen layout (`src/layout.h`)
Every screen positions its content through one `Layout L`, initialised from `M5.Display.width()/height()`.
Height >= 200 selects the original 320x240 numbers (Core2/CoreS3 look exactly as before); below that the compact
240x135 profile is used (smaller face, 16 px hint bar, settings list scrolls 4 rows at a time, rows that would overflow
switch to the small font, "Whoa!" replaces the hint bar). Adding a new board size = adding a profile in `Layout::init`.

### Buttons (one mapping for all boards, see `handleInput()` in `main.cpp`)
Everything funnels into `onAction('A'|'B'|'C'|'T'|'S')`.

| action | Core2 / CoreS3 (touch) | Plus2 (buttons) |
|---|---|---|
| talk / confirm / cancel crash alert | **A** (or tap the face) | **A** front button (click) |
| next screen | **B** | **B** side button (click) |
| option: reset / change value | **C** | **B hold** (reset on Noise/G/Trip); in Settings **B click** = change value |
| Settings | B cycles to it | **PWR click** toggles Settings (PWR/A-hold = back) |
| inside Settings: next row | **A** | **A** |
| crash alert | any button / touch cancels | any button cancels |

Plus2: holding PWR for a few seconds still powers the stick off (hardware behaviour).

## Files
| file | what |
|---|---|
| `src/layout.h` | screen geometry for 320x240 and 240x135 |
| `src/main.cpp` | state machine, audio, IMU, screens, networking |
| `src/shift.h`, `src/shift.cpp` | shift mode logic: storage (SD / LittleFS), streaming note writer, upload queue, `/shift/finish` |
| `src/bridge_net.h` | shared bridge helpers (base URL, token, https client, ISO time); used by shift and hub |
| `src/hub.h`, `src/hub.cpp` | hub worker task: heartbeat/poll, commands, acks, crash alert (car <-> pocket) |
| `src/wifi_mgr.h` | Wi-Fi manager: up to 4 networks, joins the strongest, backoff reconnect |
| `partitions_plus2.csv` | Plus2 partition table (2 MB app + ~6 MB LittleFS) |
| `src/trigger.h` | "double shout" voice trigger + Edge Impulse hook (`USE_EI_KWS`) |
| `src/face.h` | animated face (moods: neutral/happy/worried/angry/confused/listen/think/talk) |
| `src/strings.h` | ALL UI strings, EN + HE table; language switch in Settings (saved in Preferences) |
| `src/rtl.h` | `rtlVisual()` - logical -> visual order for Hebrew, digits/Latin stay LTR |
| `src/font_he.h` | **you create it** (see below). Without it Hebrew falls back to English, build still works |

## Hebrew font (optional but needed for the Hebrew UI/replies)
The firmware looks for `src/font_he.h` (`__has_include`). If it is missing, `HAVE_HE_FONT` is
undefined, the language item shows "Hebrew (no font)" and everything stays English.

1. Pick a font with Hebrew + Latin + digits, e.g. Heebo or Assistant (SIL OFL). Size ~20 px.
2. Make a VLW file containing these ranges: `0x0020-0x007E` (ASCII), `0x05D0-0x05EA` (Hebrew letters),
   `0x05B0-0x05C7` optional (niqqud), `0x2022 0x00B0`:
   - Processing "Create Font..." (from Bodmer's TFT_eSPI `Tools/Create_Smooth_Font`) -> `Heebo20.vlw`, or
   - LovyanGFX / M5GFX `lgfx_fontconvert`-style generators (any tool that writes the Processing VLW format).
3. Convert to a C array and name it `font_he_vlw`:
   ```
   xxd -i Heebo20.vlw > /tmp/x.h
   # then create src/font_he.h:
   #pragma once
   #include <stdint.h>
   const uint8_t font_he_vlw[] PROGMEM = { ...bytes from x.h... };
   ```
   (`src/font_he.h.example` shows the exact shape.)
4. Rebuild. A 20 px font with ~100 glyphs is ~15-25 KB of flash.

Notes: text is wrapped in logical order, then each line goes through `rtlVisual()` before drawing.
Replies from the bridge that contain Hebrew are drawn with the Hebrew font and right-aligned.

## Bridge contract
`POST BRIDGE_URL` (`Content-Type: audio/wav`, 16 kHz mono 16-bit) ->
```json
{"text":"...","reply":"...","action":null,"audio_url":null}
```
- `reply` in the language the user spoke.
- `audio_url` (absolute, or `/path` relative to the bridge) -> mono 16-bit WAV, up to 600 KB, downloaded into PSRAM
  and played while the face "talks". If the download fails the reply is shown as text only.
- `action`: `beep`, `face:happy|angry|confused|worried`. Anything else is ignored.

## Known limitations
- Mic is uncalibrated: tune `DB_OFFSET` against a phone dB meter.
- IMU is sampled from the main loop (~30-60 Hz); a very short impact can be missed while a frame is being drawn.
- IMU axes assume: screen facing the driver, device upright (X = right, Y = up, Z = towards the driver).
- Crash alert is local (screen + beeps). No automatic calls/messages.
- HTTPS to the bridge skips certificate verification.

## A2DP: voice replies through the car hands-free kit (envs `core2_a2dp`, `plus2_a2dp`)

Core2 and Plus2 only (the CoreS3's ESP32-S3 has no Bluetooth Classic). Build/flash: `pio run -e core2_a2dp -t upload`
(or `-e plus2_a2dp`). On Plus2 there is no speaker, so if the kit cannot be reached the reply stays **text only**.
Library: `pschatzmann/ESP32-A2DP` pinned to tag `v1.8.7` (not in the PlatformIO registry, fetched from git).

**How it works.** For each spoken reply Dashie brings Bluetooth up, connects to the kit as an A2DP *source*,
streams the ElevenLabs WAV (16 kHz mono -> 44.1 kHz stereo, with gain headroom and 0.3 s of lead-in silence),
then disconnects and shuts Bluetooth down so the phone can have the kit again. If the kit can't be reached in
`A2DP_CONNECT_TIMEOUT_MS` (or RAM is short), the reply plays on Dashie's own speaker instead. Text is always shown.

**First-time setup (Deelife NX12/HY62 or any kit):**
1. Put the kit in **pairing mode first** (long press, as for pairing a phone). Only then does it show up in a scan.
2. Settings (Core2: B until Settings; Plus2: PWR click) -> move to **Test kit** with A -> Core2: press C (OPT), Plus2: click B.
3. If `A2DP_TARGET_NAME` is not found, Dashie shows the audio devices it saw (up to 8): Core2: A = next, C = pick, B = back;
   Plus2: A = next, B = pick, PWR (or A hold) = back.
   The pick is saved in Preferences (name + address) and overrides `config.h`; the test then reruns by itself.
4. Next times Dashie connects to the saved address directly. If that fails once, the following attempt scans again.

Since the kit supports two devices at once, the phone can stay connected. Test kit tells you whether it really
works with your car: a short two-tone beep should come out of the car speakers while the phone stays connected.

**Config (`src/config.h`):** `A2DP_TARGET_NAME`, `A2DP_CONNECT_TIMEOUT_MS` (6000), `A2DP_SCAN_MS`,
`A2DP_STREAM_START_TIMEOUT_MS`, `A2DP_MIN_FREE_HEAP`, `A2DP_GAIN_Q15`, `A2DP_RELEASE_BLE` (off).

**Not verified on hardware (be suspicious of all of it):**
- Compiles, but nothing was run on a Core2: pairing, connection, audio quality and teardown/restart cycles are untested.
- Wi-Fi + Bluetooth Classic together is tight on internal RAM. `A2DP_MIN_FREE_HEAP` (90000) is a guess; the serial log
  prints `a2dp: free internal ...` before each session, tune from that.
- The library only checks "ready to stream" every 10 s; Dashie nudges it once a second after connecting.
  If audio still starts late, that is where to look.
- `A2DP_RELEASE_BLE` is off on purpose: the prebuilt Arduino core runs the controller in dual mode, and releasing
  BLE memory before enabling it may fail.
- Kits that aren't discoverable outside pairing mode can only be reached through the saved address.

## Plus2 notes (envs `plus2`, `plus2_a2dp`)
- **Hardware:** ESP32-PICO-V3-02, 8 MB flash, **2 MB PSRAM**, 135x240 ST7789, PDM mic, MPU6886, buzzer, buttons A/B/PWR.
  PlatformIO has no board file for it; `board = m5stick-c` is used with the flash size forced to 8 MB (`partitions_plus2.csv`),
  and M5Unified/M5GFX detect the Plus2 at run time. `-DBOARD_HAS_PSRAM` comes from the shared flags.
  The partition table is `partitions_plus2.csv` (2 MB app + ~6 MB LittleFS for shift-mode notes instead of `default_8MB.csv`);
  changing the partition table needs a normal USB flash (`pio run -e plus2 -t upload`), not OTA. `plus2_a2dp` uses 81 % of the 2 MB app slot.
- **Memory:** internal RAM is the same ~320 KB as on the Core2, so Wi-Fi + A2DP is exactly as tight as there
  (`A2DP_MIN_FREE_HEAP` check + fallback are shared). What differs is PSRAM: 2 MB instead of 8 MB. A voice request
  peaks at about 0.25 MB (recording 192 KB + sprite 65 KB + JSON) plus the downloaded reply WAV (<= `AUDIO_MAX_BYTES`,
  600 KB default; a 160-character reply is ~300 KB). That fits, but if you hit "Out of memory" lower `AUDIO_MAX_BYTES`
  with `-DAUDIO_MAX_BYTES=\(300*1024\)`. Without A2DP (`plus2`) the reply WAV is not downloaded at all.
- **Rotation / IMU:** `DASHIE_ROTATION` (default 1) selects landscape; use 3 if the buttons are on the wrong side.
  The IMU axes for the lying-landscape mounting are a guess (`-DDASHIE_IMU_LAT_EXPR=ly`, forward = -lz): on the G-Force
  screen the ball must move to the side you steer towards and forward/back with braking. If it is mirrored or swapped,
  change `DASHIE_IMU_LAT_EXPR` / `DASHIE_IMU_FWD_EXPR` (`(-ly)`, `(lx)`, `(lz)` ...). Crash detection uses |a| and is unaffected.
- **Hebrew:** same font hook as the other boards; a 20 px font is wide on 240 px, so check long labels.
- **Not verified on hardware:** everything. The three builds compile and the layout maths is unit-checked on the PC,
  but fit of text on the 135 px panel, button feel, rotation, IMU axes and A2DP on this board have never run.

## Shift mode (all envs; `-DSHIFT_ENABLED=0` compiles it out)
Voice notes during a work shift, uploaded to the bridge, which e-mails / files / reads back a summary
(`../SPEC_SHIFT.md`, bridge side: `../bridge/README.md`). Code: `src/shift.cpp` + `shift.h` (storage, writer, uploads, finish),
the UI glue is in `main.cpp` ("Shift mode UI"), shared bridge helpers in `src/bridge_net.h`.

**Use it**
1. **Start:** B until the *Shift* screen, then **hold A** (Core2: touch zone A, Plus2: button A).
2. **Note:** one physical button. Core2: the side button (`BtnPWR`) click = start, click again = stop.
   Plus2: **A** click. Vibration (Core2) / beeps (Plus2): 1 = recording, 2 = saved, 3 = error / auto-stop at 90 s.
   Notes under 1 s are discarded, so a double press is a silent "peek" at the screen.
3. **Pocket mode:** after 10 s without input the screen turns off and (Core2) the touch zones are ignored; only the
   physical button works. Voice trigger is off during a shift. Recording happens only on a click, never continuously.
4. **End:** hold the note button (~1 s Core2, 1.5 s Plus2), confirm with one click within 6 s. Four wrap-up questions
   appear (click = record the answer, hold = skip; question 4 is followed by the upload + summary). Needs Wi-Fi to
   finish: without it the shift is saved and sent later in the background (a vibration tells you the summary is ready;
   **B-hold / C on the Shift screen = "Read last summary"**).
5. **Pending follow-ups:** if the bridge asks up to 3 follow-up questions they are shown the same way; the answers are
   uploaded as notes with `X-Kind: answer`.

**Storage:** Core2/CoreS3 -> SD card, 16 kHz (LittleFS fallback at 8 kHz when no card is inserted). Plus2 -> LittleFS, 8 kHz
(~16 KB/s, ~6 MB ~ 6 min; the Shift screen shows the minutes left). Notes are written as `.tmp` and renamed to
`nNNNN_<epoch>.wav` (`aNNNN_...` = answer) when they end; a note cut off by a crash or battery pull is repaired and queued at
the next boot. A note is deleted from the device only after the bridge acknowledged it (HTTP 200, also for `duplicate`).
A note the bridge rejects for good (400/413/415/422) is parked as `*.rej` so it cannot block the queue.
The shift state (id, seq counter, "finishing") is kept in NVS, so it survives a reboot / battery swap.

**Bridge contract used** (`/shift/*`, Bearer `BRIDGE_TOKEN` if set): `POST /shift/start {shift_id, started_at}`;
`POST /shift/note` (WAV body; `X-Shift-Id`, `X-Seq`, `X-Time`, `X-Kind: answer`); `POST /shift/finish {shift_id, ended_at}` ->
`done {summary_md, spoken, audio_urls[]}` or `need_info {questions[]}`. Times are ISO 8601 in local time with offset.
`audio_urls` are downloaded and **merged into one WAV in PSRAM** so the A2DP kit connects once; the budget is
`SHIFT_AUDIO_MAX_BYTES` (Core2 2.5 MB ~ 75 s, Plus2 1 MB ~ 30 s), longer summaries are cut short (the text is shown in full).
`HTTPClient` cannot wait longer than 65 s for a response, while the bridge may need ~90 s: the device simply asks again
(the bridge answers from its cache once done). While waiting, the screen shows "Sending / Summarizing" for up to
`SHIFT_FINISH_WAIT_MS` (130 s); after that the shift keeps finishing in the background.

**Config (`src/config.h`):** `BRIDGE_TOKEN` (`SHIFT_BRIDGE_TOKEN` is an alias), `SHIFT_BRIDGE_BASE`, `SHIFT_NOTE_MAX_S` (90),
`SHIFT_NOTE_MIN_MS`, `SHIFT_POCKET_MS` (10000), `SHIFT_LOW_BATT_PCT` (20), `SHIFT_FINISH_WAIT_MS`, `SHIFT_AUDIO_MAX_BYTES`,
`SHIFT_SPOKEN_QUESTIONS` (1: the wrap-up questions are spoken from `GET /shift/question/<n>.wav` on boards with a speaker, i.e. Core2/CoreS3;
on the Plus2 (buzzer only) they stay text + vibration, and if the bridge has no TTS configured it answers 404 and the question stays text).
Questions always play on the device's own speaker, never over the A2DP kit.
Wi-Fi: up to four networks (`WIFI_SSID`, `WIFI_SSID_2..4`), see the Hub section.
The clock: RTC (UTC) seeds the system time at boot, NTP corrects it and is written back every 6 h. If the clock was never set
the shift has no times and the summary has no date.

**Behaviour changes outside shift mode (only if you set a token):** none. With `SHIFT_ENABLED=0` the binary is the same code
as before plus ~1.5 KB (`fetchWav` / `startPlayback` refactor, extra enum values).

**Not verified on hardware (all of it):** SD init on the Core2 shared SPI bus (pins come from `M5.getPin(sd_spi_*)`:
18/23/38/4; the display and the SD writer take a mutex around each other, untested), the `BtnPWR` click / ~1 s hold
semantics of the AXP192, the vibration motor feedback timing, pocket-mode touch lock, the Plus2 partition layout and
LittleFS capacity, note writer back-pressure (a full ring drops frames and counts `overruns`), upload while the A2DP
kit or the assistant is active (uploads pause then), memory use of Wi-Fi + TLS during a shift, and the layout of the Shift
and wrap-up screens on 240x135 (only compiled). Plus2: A as note button means no assistant button during a shift.
Not implemented: a note-taking mode for the car device while the pocket device is the recorder (they do not share notes).

## Hub: car device + pocket device (all envs; `-DHUB_ENABLED=0` compiles it out)

Two Dashies coordinate **through the bridge** (no radio between them). Contract and server side: `bridge/hub.py`, `bridge/README.md` "Hub".

| role | default board | what it does |
|---|---|---|
| **car** | Core2 (any non-"buttons only" board) | speaks the finished shift summary when you get in; shows the pocket device on a new screen (B cycles to **Pocket device**: link, pocket status, battery, notes waiting, shift active, "In car"); sends a crash alert to the pocket device |
| **pocket** | Plus2 (`DASHIE_BUTTONS_ONLY`) | vibrates / beeps hard on a crash alert from the car; beeps on "where is my pocket device?" until you press any button |

Override the role with `-DHUB_ROLE_POCKET=1` (or `0`). The device id is `car-<MAC>` / `pkt-<MAC>`.

**How it works.** One worker task does all hub network I/O and never touches M5 / the display / I2C: the main loop feeds it
the battery %, notes waiting and shift flag once a second and picks commands up. `POST /hub/hello` is both heartbeat and poll
(`poll_s` from the bridge, 10 s car / 20 s pocket). Commands are **acked when accepted**, not when finished; a busy device
(recording a note, assistant active, A2DP running) leaves the command un-acked and pending, the bridge redelivers it
(3 times, 10 min TTL). Accepted ids are remembered (16), so a redelivery after a lost ack is not run twice. A 404 on ack = expired.

**Commands.** `read_summary` (car): downloads the signed `audio_urls`, merges them and plays them through the normal reply path
(A2DP kit first when built with it, own speaker otherwise), same memory limits as "Read last summary". `vibrate` / `find_me`
(pocket): new `PALERT` / `FIND` states, forced to full volume, screen switched on even in pocket mode, stop on any button or after
`HUB_PALERT_MAX_S` (120 s) / the bridge's `duration_s` (max `HUB_FIND_MAX_S`). A running note is saved first.

**Crash alert.** When the car device detects an impact, `POST /hub/alert {device_id, type:"crash", g, ts}` is sent in addition
to the local "Are you OK?" screen, retried for `HUB_ALERT_RETRY_S` (90 s) while Wi-Fi/bridge are down; `ts` de-duplicates.
`HUB_ALERT_ON_TIMEOUT_ONLY=1` sends it only if the 15 s countdown ends without being cancelled (fewer false alarms,
at most 15 s later). A pocket device ignores its own IMU crash detection while a shift is active (`HUB_POCKET_NO_CRASH`,
a knock in a kitchen pocket must not start a 15 s alarm).

**Wi-Fi manager (`wifi_mgr.h`, also used with `HUB_ENABLED=0`).** `WIFI_SSID[_2.._4]` / `WIFI_PASS[_2.._4]`: kitchen network at work
+ phone hotspot in the car. Disconnected -> scan -> join the **strongest** configured network; reconnect backoff 3 s ... 60 s;
modem sleep on. A2DP envs do not scan (see below); they try the entries in list order, 8 s each. Set `HUB_CAR_SSID` to the
hotspot name, **identical to `HUB_CAR_SSID` on the bridge**: that is how "you are in the car" is detected, and the pocket device
polls fast only while it is on that SSID (otherwise every `HUB_POLL_SLOW_S`, 60 s, to save its 200 mAh battery).

**Config:** `HUB_ENABLED`, `HUB_ROLE_POCKET`, `HUB_CAR_SSID`, `HUB_POLL_FAST_S`, `HUB_POLL_SLOW_S`, `HUB_ALERT_ON_TIMEOUT_ONLY`,
`HUB_ALERT_RETRY_S`, `HUB_PALERT_MAX_S`, `HUB_FIND_MAX_S`, `HUB_POCKET_NO_CRASH`, `WIFIMGR_SCAN`, `BRIDGE_TOKEN`.
`/voice` now also sends `X-Device-Id` and the Bearer token (needed for the spoken hub commands "where is my pocket device" / "read my summary").
Audio downloads send the Bearer token only to the bridge's own origin.

### IRAM limit: A2DP envs and Wi-Fi (pre-existing, found while building the hub)
**`core2_a2dp` and `plus2_a2dp` do not link once a Wi-Fi network is configured** (`WIFI_SSID` non-empty):
`region 'iram0_0_seg' overflowed by 2004 bytes` (Core2; 1876 on Plus2). It is **not** caused by the hub: the commit before the hub
(shift mode) fails the same way with `-DWIFI_SSID=\"x\"`. With the shipped default (empty SSID) the compiler removes all Wi-Fi
driver calls, the link fits, and the build looked fine. The Bluetooth controller (~31 KB), the Wi-Fi driver and the system
libraries already fill the 128 KB IRAM of the prebuilt Arduino core; the Wi-Fi driver objects that `WiFi.begin()` pulls in are the last ~2 KB.
Nothing in this firmware lives in IRAM (checked in the link map), so it can not be fixed from the sketch.
`wifi_mgr.h` keeps the default config linkable (`kAny` folds to nothing when no SSID is set) and avoids the scan API in A2DP envs,
but the real fix is a core with a smaller IRAM footprint: build the Arduino core with its own `sdkconfig`
(PlatformIO `framework = arduino, espidf` + `custom_sdkconfig`, or `esp32-arduino-lib-builder`) - not tried, long build.
Until then, for a Wi-Fi-connected car device use **`core2`** (own speaker, no A2DP) or accept that the A2DP envs only run with Wi-Fi off.

### Memory / battery notes
- `plus2_a2dp` app slot: 82.2 % (was 81.5 % before the hub). `core2` flash 15 %.
- A Plus2 with Wi-Fi on for a whole shift will not last 8 h on its 200 mAh battery even with modem sleep and slow polling.
  Expect a few hours; charge it, or accept uploading only when you are near Wi-Fi. Not measured.

### Not verified on hardware
Everything above: it compiles (see `pio run -e core2|cores3|plus2|core2_a2dp|plus2_a2dp`, also with `-DHUB_ENABLED=0` and `-DSHIFT_ENABLED=0`),
the bridge side is covered by `bridge/tests`, but no hub command, alert, scan or reconnect has run on a real device. In particular:
Wi-Fi scan/join behaviour with two networks, the 10 s / 20 s poll cost, vibration/beep loudness of the alert on a Plus2 buzzer in a pocket,
the peer screen layout, and the ack/redelivery timing under real packet loss.
