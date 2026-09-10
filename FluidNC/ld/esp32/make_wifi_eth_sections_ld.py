"""Generate FluidNC/ld/esp32/wifi_eth/sections.ld from the core 3.x framework.

The checked-in wifi/bt/noradio copies come from framework-arduinoespressif32
2.0.17 (ESP-IDF 4.4) and cannot be reused for a core 3.x build.  This takes the
stock esp32 sections.ld out of the installed pioarduino framework package and
adds the same INCLUDE line the other copies carry.
"""
import pathlib, sys

repo = pathlib.Path(r"C:\Users\user\FluidNC-w5500")
pkgs = pathlib.Path.home() / ".platformio" / "packages"

# pioarduino 55.x ships the prebuilt IDF libs and their linker scripts in a
# separate framework-arduinoespressif32-libs package; core 2.0.17 kept them
# under tools/sdk inside the framework package itself.
cands = [
    pkgs / "framework-arduinoespressif32-libs" / "esp32" / "ld" / "sections.ld",
    pkgs / "framework-arduinoespressif32" / "tools" / "esp32-arduino-libs" / "esp32" / "ld" / "sections.ld",
    pkgs / "framework-arduinoespressif32" / "tools" / "sdk" / "esp32" / "ld" / "sections.ld",
]
cands = [c for c in cands if c.is_file()]
if not cands:
    sys.exit("no stock esp32 sections.ld found under " + str(pkgs))
src = cands[0]
print("stock:", src)

text = src.read_text(encoding="utf-8")

anchor = "\n    _data_end = ABSOLUTE(.);"
if anchor not in text:
    anchor = "\n  _data_end = ABSOLUTE(.);"
if anchor not in text:
    sys.exit("could not find the _data_end anchor in " + str(src))
if text.count(anchor) != 1:
    sys.exit("_data_end anchor appears %d times; refusing to guess" % text.count(anchor))

text = text.replace(anchor, "\n    INCLUDE ../vtable_in_dram.ld\n" + anchor, 1)

out = repo / "FluidNC" / "ld" / "esp32" / "wifi_eth" / "sections.ld"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(text, encoding="utf-8", newline="\n")
print("wrote:", out, out.stat().st_size, "bytes")
