# PlatformIO pre-script for the *_a2dp envs.
#
# Problem: Wi-Fi + Bluetooth Classic (A2DP) overflows IRAM by ~2 KB at link time ("iram0_0_seg overflowed").
# Cause:   the precompiled Arduino-ESP32 2.0.x core is built with CONFIG_SPIRAM_CACHE_WORKAROUND=y (needed only for
#          ESP32 silicon rev 0/1), and its generated sections.ld therefore places ~110 libc functions (stdio, time,
#          strftime, string functions...) in IRAM.
#          M5Stack Core2 (ESP32-D0WDQ6-V3) and StickC Plus2 (ESP32-PICO-V3-02) are silicon rev 3, which does not have
#          the PSRAM cache bug, so those libc functions can live in flash like in any normal ESP-IDF build.
# Fix:     write a patched copy of sections.ld into the build dir (it is first on the -L search path) with the libc
#          block removed from .iram0.text. Nothing in the firmware or its ISRs depends on those functions being in IRAM.
# Do NOT use this on a board with ESP32 rev 0/1 silicon (old M5Stack Core/Fire) that uses PSRAM.
import os
import re

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

fw = env.PioPlatform().get_package_dir("framework-arduinoespressif32")  # noqa: F821
src = os.path.join(fw, "tools", "sdk", "esp32", "ld", "sections.ld")
dst_dir = env.subst("$BUILD_DIR")  # noqa: F821
dst = os.path.join(dst_dir, "sections.ld")

lines = open(src).read().split("\n")
out, in_iram_text, removed = [], False, 0
libc_rule = re.compile(r"^\s*\*libc\.a:[A-Za-z0-9_\-]+\.\*\(\.literal \.literal\.\* \.text \.text\.\*\)\s*$")
for ln in lines:
    if re.match(r"^\s*\.iram0\.text\s*:", ln):
        in_iram_text = True
    elif in_iram_text and re.match(r"^\s*\}\s*>\s*iram0_0_seg", ln):
        in_iram_text = False
    if in_iram_text and libc_rule.match(ln):
        removed += 1
        continue
    out.append(ln)

os.makedirs(dst_dir, exist_ok=True)
new = "\n".join(out)
if not os.path.exists(dst) or open(dst).read() != new:
    open(dst, "w").write(new)
print("trim_iram.py: removed %d libc IRAM rules from sections.ld (%s)" % (removed, dst))
env.Depends("$BUILD_DIR/${PROGNAME}.elf", dst)  # noqa: F821
