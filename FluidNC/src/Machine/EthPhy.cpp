// Copyright (c) 2026 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

#include "Config.h"
#if MAX_N_ETH

#    include "EthPhy.h"
#    include "MachineConfig.h"
#    include "Assertion.h"

#    include <ETH.h>

namespace Machine {
    const EnumItem EthPhy::phyTypes[] = {
        { EthPhy::W5500, "w5500" },
        { EthPhy::KSZ8851, "ksz8851" },
        { EthPhy::DM9051, "dm9051" },
        EnumItem(EthPhy::W5500),
    };

    static eth_phy_type_t arduinoPhyType(uint32_t phy_type) {
        switch (phy_type) {
            case EthPhy::KSZ8851:
                return ETH_PHY_KSZ8851;
            case EthPhy::DM9051:
                return ETH_PHY_DM9051;
            case EthPhy::W5500:
            default:
                return ETH_PHY_W5500;
        }
    }

    void EthPhy::validate() {
        // config->_spi->defined() cannot be checked here: SPIBus::defined()
        // only becomes true once SPIBus::init() actually runs the hardware
        // init, which happens later in the startup sequence than validate().
        // That check belongs in init(), same as SDCard does it.
        Assert(_cs.defined(), "Ethernet cs_pin must be configured");
    }

    void EthPhy::afterParse() {}

    bool EthPhy::init() {
        if (!_cs.defined()) {
            log_debug("Ethernet not configured (no cs_pin)");
            return false;
        }
        if (!config->_spi->defined()) {
            log_error("Ethernet needs SPI defined");
            return false;
        }

        log_info("Ethernet PHY " << phyTypes[_phy_type].name << " cs_pin:" << _cs.name() << " int_pin:" << _int.name()
                                 << " rst_pin:" << _rst.name());

        _cs.setAttr(Pin::Attr::Output);
        pinnum_t csPin = _cs.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);

        int intPin = -1;
        if (_int.defined()) {
            _int.setAttr(Pin::Attr::Input);
            intPin = _int.getNative(Pin::Capabilities::Input | Pin::Capabilities::Native);
        }

        int rstPin = -1;
        if (_rst.defined()) {
            _rst.setAttr(Pin::Attr::Output);
            rstPin = _rst.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        }

        // config->_spi holds Pin objects for the shared SPI bus (sck/mosi/miso).
        // ETH.begin()'s SPI-Ethernet overload takes raw pin numbers and adds its
        // own ESP-IDF spi_master device, with its own CS, on the host we give it.
        //
        // That host is deliberately the *same* one FluidNC's spi_init_bus() uses:
        // esp32/spi.cpp initializes HSPI_HOST, which is SPI2_HOST on both esp32
        // (hal/esp32/include/hal/spi_types.h) and esp32s3. Sharing is what makes
        // the W5500 coexist with SDCard on one set of physical pins, and it is
        // safe because ETH.cpp's beginSPI() treats ESP_ERR_INVALID_STATE from its
        // own spi_bus_initialize() as success - i.e. "already initialized, just
        // add my device". The ordering that this relies on is guaranteed:
        // Main.cpp calls config->_spi->init() before any Module::init(), and
        // EthConfig is a Module (init_priority 106).
        pinnum_t sckPin  = config->_spi->_sck.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pinnum_t mosiPin = config->_spi->_mosi.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pinnum_t misoPin = config->_spi->_miso.getNative(Pin::Capabilities::Input | Pin::Capabilities::Native);

        // Argument order after spi_host is (sck, miso, mosi) - see
        // libraries/Ethernet/src/ETH.h in Arduino-ESP32 core 3.x. Passing
        // mosi where miso is expected wires the W5500 up backwards, so the
        // PHY never answers and ETH.begin() fails (or worse, half-works).
        // Retry, because one attempt is not reliable at boot.
        //
        // The PHY keeps its own power across an ESP32 soft reset, and rst_pin is
        // optional (commonly NO_PIN), so nothing resets the chip when the CPU
        // restarts. If the reset landed part-way through an SPI transaction the
        // PHY is still waiting for the rest of that command, and answers the
        // chip-ID read out of whatever is left in its shift register - the driver
        // reports "version mismatched, expected 0x04, got 0x00" and gives up.
        //
        // Retrying pushes the chip's SPI state machine back into sync. Measured on
        // a W5500 over a marginal SPI link: boot-time init failed ten times in a
        // row, while $EI - which calls this same function - succeeded ten times in
        // a row moments later.
        const int maxAttempts = 5;
        const int retryDelayMs = 100;  // some W5500 modules want ~10ms after a reset

        bool ok = false;
        for (int attempt = 1; attempt <= maxAttempts && !ok; ++attempt) {
            ok = ETH.begin(arduinoPhyType(_phy_type),
                           _phy_addr,
                           int(csPin),
                           intPin,
                           rstPin,
                           SPI2_HOST,
                           int(sckPin),
                           int(misoPin),
                           int(mosiPin),
                           uint8_t(_frequency_hz / 1000000));
            if (!ok && attempt < maxAttempts) {
                log_debug("Ethernet PHY init attempt " << attempt << " failed, retrying");
                delay_ms(retryDelayMs);
            }
        }
        if (!ok) {
            log_error("Ethernet PHY init failed after " << maxAttempts << " attempts");
            return false;
        }
        config_ok = true;
        return true;
    }
}
#endif
