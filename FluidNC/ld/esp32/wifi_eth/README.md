# wifi_eth/sections.ld

Not hand-written and not interchangeable with `../wifi/sections.ld`.

`../vtable_in_dram.py` prepends `FluidNC/ld/esp32/$PIOENV` to `LIBPATH` so the
linker finds this `sections.ld` before the framework's own copy (see
`../README.md` for why the vtables have to move to DRAM at all). That means the
file here must be the stock `sections.ld` **of the framework this env actually
builds against**, plus the one added `INCLUDE` line.

`wifi/`, `bt/` and `noradio/` hold copies taken from
framework-arduinoespressif32 2.0.17 (ESP-IDF 4.4). The `wifi_eth` env builds
against pioarduino / Arduino core 3.x (ESP-IDF 5.5.4), whose `sections.ld` is a
different file, so it needs its own copy.

Regenerate after changing the platform version:

    python FluidNC/ld/esp32/make_wifi_eth_sections_ld.py

That script copies
`~/.platformio/packages/framework-arduinoespressif32-libs/esp32/ld/sections.ld`
and inserts `INCLUDE ../vtable_in_dram.ld` just before `_data_end` in the
`dram0_0_seg` output section, which is where the other copies carry it.

Note the failure mode if this directory is missing or stale: the linker simply
falls back to the stock script with **no error**, the vtables stay in flash, and
the board panics only later, when a long move happens to coincide with flash
cache activity.
