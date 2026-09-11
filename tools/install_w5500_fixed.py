"""Point Arduino's W5500 bring-up at FluidNC's vendored driver.

ETH.cpp builds the SPI host config and then calls the stock MAC/PHY factories
from the precompiled libesp_eth.a. Those are the ones with the RX-desync
infinite loop (esp-eth-drivers#151) that panics this board. This rewrites the
two factory calls to go through fluidnc_eth_new_w5500(), which builds the
vendored driver instead. See FluidNC/esp32/w5500_fixed/README.md.

The hook is a weak symbol, so an environment that does not compile
w5500_fixed still links and keeps the stock driver.

Fails the build if ETH.cpp does not look the way we expect, rather than
silently shipping the unfixed driver. Idempotent.
"""
from pathlib import Path

DECL_ANCHOR = 'bool ETHClass::beginSPI('

DECLS = '''#if CONFIG_ETH_SPI_ETHERNET_W5500
// FluidNC: implemented by FluidNC/esp32/w5500_fixed/w5500_fixed_glue.c.
// Weak, so environments that do not build that directory fall back to the
// stock driver below.
extern "C" esp_err_t __attribute__((weak))
fluidnc_eth_new_w5500(spi_host_device_t host, spi_device_interface_config_t *devcfg, int int_gpio_num,
                      uint32_t poll_period_ms, const eth_mac_config_t *mac_config, const eth_phy_config_t *phy_config,
                      esp_eth_mac_t **out_mac, esp_eth_phy_t **out_phy);
#endif

'''

OLD_FACTORY = '''    _mac = esp_eth_mac_new_w5500(&mac_config, &eth_mac_config);
    _phy = esp_eth_phy_new_w5500(&phy_config);'''

NEW_FACTORY = '''    // FluidNC: the stock W5500 driver has no recovery path when the frame
    // length read from the chip is implausible; its RX task then spins until
    // the task watchdog reboots the board. Use the vendored driver that has
    // the upstream fix. On failure _mac/_phy stay NULL and the existing NULL
    // checks below clean up, same as a stock factory failure.
    if (fluidnc_eth_new_w5500) {
      fluidnc_eth_new_w5500(
        spi_host, &spi_devcfg, mac_config.int_gpio_num, mac_config.poll_period_ms, &eth_mac_config, &phy_config, &_mac,
        &_phy
      );
    } else {
      _mac = esp_eth_mac_new_w5500(&mac_config, &eth_mac_config);
      _phy = esp_eth_phy_new_w5500(&phy_config);
    }'''


def patch_text(text):
    if NEW_FACTORY in text:
        if DECLS not in text:
            raise RuntimeError('ETH.cpp has the vendored-driver call but not its declaration')
        return text
    if text.count(OLD_FACTORY) != 1:
        raise RuntimeError('Unsupported Arduino ETH.cpp: W5500 MAC/PHY construction not found exactly once')
    if text.count(DECL_ANCHOR) != 1:
        raise RuntimeError('Unsupported Arduino ETH.cpp: beginSPI definition not found exactly once')
    text = text.replace(OLD_FACTORY, NEW_FACTORY)
    return text.replace(DECL_ANCHOR, DECLS + DECL_ANCHOR)


if __name__ == '__main__':
    import unittest

    SAMPLE = 'prologue\n' + DECL_ANCHOR + '...) {\n' + OLD_FACTORY + '\n  } else\n'

    class PatchTests(unittest.TestCase):
        def test_declaration_precedes_the_definition(self):
            out = patch_text(SAMPLE)
            self.assertLess(out.index(DECLS), out.index(DECL_ANCHOR))
            self.assertIn(NEW_FACTORY, out)

        def test_stock_factories_remain_as_the_fallback(self):
            out = patch_text(SAMPLE)
            self.assertIn('_mac = esp_eth_mac_new_w5500(&mac_config, &eth_mac_config);', out)
            self.assertIn('_phy = esp_eth_phy_new_w5500(&phy_config);', out)

        def test_idempotent(self):
            self.assertEqual(patch_text(patch_text(SAMPLE)), patch_text(SAMPLE))

        def test_unknown_or_ambiguous_version_fails(self):
            for value in ('unknown', SAMPLE + OLD_FACTORY, DECL_ANCHOR + DECL_ANCHOR + OLD_FACTORY):
                with self.assertRaises(RuntimeError):
                    patch_text(value)

        def test_half_applied_file_is_rejected(self):
            with self.assertRaises(RuntimeError):
                patch_text(NEW_FACTORY)

    unittest.main()
else:
    try:
        Import('env')
    except NameError:
        pass
    else:
        if env['PIOENV'] == 'wifi_eth':
            package = env.PioPlatform().get_package_dir('framework-arduinoespressif32')
            source = Path(package) / 'libraries/Ethernet/src/ETH.cpp'
            original = source.read_text(encoding='utf-8')
            patched = patch_text(original)
            if patched != original:
                source.write_text(patched, encoding='utf-8', newline='\n')
                print('FluidNC: Arduino W5500 bring-up routed to the vendored driver')
