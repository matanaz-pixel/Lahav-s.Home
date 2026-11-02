# Dashie firmware (M5Stack Core2 / CoreS3 / StickC Plus2)

```
pio run -e core2            # or: cores3, plus2, core2_a2dp, plus2_a2dp
pio run -e core2 -t upload
pio device monitor
```
Edit `src/config.h` first (Wi-Fi hotspot of your phone, `BRIDGE_URL`, timezone).

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
  PlatformIO has no board file for it; `board = m5stick-c` is used with the flash size forced to 8 MB (`default_8MB.csv`),
  and M5Unified/M5GFX detect the Plus2 at run time. `-DBOARD_HAS_PSRAM` comes from the shared flags.
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
