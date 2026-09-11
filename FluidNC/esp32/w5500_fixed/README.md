# w5500_fixed — W5500 driver vendored from `espressif/esp-eth-drivers`

## Why

The W5500 driver inside the precompiled `libesp_eth.a` that ships with
Arduino-ESP32 3.3.9 (ESP-IDF v5.5.4) has no recovery path when the frame-length
header it reads from the chip is implausible. From
[esp-eth-drivers#151](https://github.com/espressif/esp-eth-drivers/issues/151),
which describes the loop in `emac_w5500_task()`:

```c
do {
    if ((ret = emac_w5500_alloc_recv_buf(emac, &buffer, &frame_len)) == ESP_OK) {
        // happy path
    } else if (ret == ESP_ERR_NO_MEM) {
        emac_w5500_flush_recv_frame(emac);     // this error recovers
    } else {
        ESP_LOGE(TAG, "unexpected error 0x%x", ret);
        // ESP_ERR_INVALID_SIZE lands here: RX_RD is not advanced, nothing is
        // flushed, packets_remain stays true -> the loop never terminates
    }
} while (emac->packets_remain);
```

One corrupted length is enough to strand `Sn_RX_RD` off a frame boundary. Every
later header read then parses payload bytes as a length, so the error repeats
forever: the task pegs its core, the IDLE task starves, and with
`CONFIG_ESP_TASK_WDT_PANIC=y` / `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` — both set in
this Arduino core — the board panics and reboots about five seconds later.

That matches what this machine does. It also explains the part that nothing else
did: why a momentary SPI glitch on the ribbon turns into a link that never comes
back rather than a hiccup.

Espressif fixed it, but only in the standalone component, and their answer on
the issue is to move to IDF >= 6.0. We cannot: the only Arduino core built on
IDF 6 is 4.0.0-alpha1, and FluidNC pins
`framework-arduinoespressif32@^3.20017.241212`. Putting a CNC controller on an
alpha core to collect one driver fix is the wrong trade, so the driver is
vendored instead.

The fix itself is a plausibility check on the length plus
`wiznet_drain_rx_buffer()`, which resyncs `Sn_RX_RD` to the chip's `Sn_RX_WR`.
Vendoring also picks up several things we had separately identified as gaps:
bounded `wiznet_send_command()`, CLOSE waiting for `Sn_SR == SOCK_CLOSED`, an RX
drain before stop, and [#120](https://github.com/espressif/esp-eth-drivers/issues/120)
(`w5500_tsk` surviving a failed driver install).

## How

- Sources come from `espressif/esp-eth-drivers` at commit `400336ad`, fetched by
  `firmware-patch/rst-watchdog/vendor_w5500.py` in the ITRI project repo. **Do
  not edit them here** — change the script and re-run it, so a later upstream
  commit still produces a readable diff.
- The script applies four identifier renames and nothing else:

  | upstream | here | why |
  |---|---|---|
  | `eth_w5500_config_t` | `fluidnc_eth_w5500_config_t` | IDF 5.5.4's `esp_eth_mac_spi.h`, which these sources include, defines the older flat struct under that name |
  | `ETH_W5500_DEFAULT_CONFIG` | `FLUIDNC_ETH_W5500_DEFAULT_CONFIG` | same header, same collision |
  | `esp_eth_mac_new_w5500` | `fluidnc_eth_mac_new_w5500` | would collide at link with `libesp_eth.a` |
  | `esp_eth_phy_new_w5500` | `fluidnc_eth_phy_new_w5500` | same |

  The script fails if a rename matches nothing or if an old name survives, so an
  upstream reshuffle stops the build instead of silently reintroducing a clash.
- `w5500_fixed_glue.c` is ours, not vendored. It is the only entry point the
  patched `ETH.cpp` calls, which keeps the caller away from the changed config
  struct layout.
- Only this directory's `.c` files are compiled, and only in the `wifi_eth`
  environment; see `build_src_filter` in `platformio_override.ini`. Renaming
  rather than relying on link order is deliberate here: `libesp_eth.a`'s
  `esp_eth_mac_w5500.c.obj` and `esp_eth_phy_w5500.c.obj` export exactly one
  symbol each, so link order would also work, but the renames make it
  impossible to link the stock driver by accident.

## Verified

Every file compiles clean with `-Wall` against the IDF 5.5.4 headers using
PlatformIO's own include and define flags. Upstream's `idf: '>=6.0'` in
`idf_component.yml` is a support policy; the three IDF-6-only GPIO calls
(`gpio_func_sel`, `gpio_output_enable`, `gpio_input_enable`) all sit behind
`#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)` and are not reached.

## Not verified

Nothing here has run on hardware yet. A clean build says the code links, not
that the W5500 still works — the RX path, the buffer accounting and the stop
sequence are all different code now. This needs a soak with real traffic before
it can be called an improvement, and it does not address why the SPI link is
marginal in the first place.
