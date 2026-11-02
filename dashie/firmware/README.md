# Dashie firmware (M5Stack Core2 / CoreS3)

```
pio run -e core2            # or: pio run -e cores3
pio run -e core2 -t upload
pio device monitor
```
Edit `src/config.h` first (Wi-Fi hotspot of your phone, `BRIDGE_URL`, timezone).

## Files
| file | what |
|---|---|
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
