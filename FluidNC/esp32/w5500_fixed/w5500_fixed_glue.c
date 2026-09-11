/*
 * Copyright (c) 2026 Mitch Bradley
 * Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.
 *
 * The one entry point the patched Arduino ETH.cpp calls to build the vendored
 * W5500 MAC and PHY. It exists so ETH.cpp never has to see the vendored
 * config struct: upstream changed eth_w5500_config_t to nest its common
 * fields under .base, which does not match the older flat struct that our
 * ESP-IDF 5.5.4 headers still declare. Keeping that difference on this side
 * of the call keeps the ETH.cpp patch small and stable.
 *
 * See README.md in this directory for why the driver is vendored at all.
 */

#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"

esp_err_t fluidnc_eth_new_w5500(spi_host_device_t              host,
                                spi_device_interface_config_t* devcfg,
                                int                            int_gpio_num,
                                uint32_t                       poll_period_ms,
                                const eth_mac_config_t*        mac_config,
                                const eth_phy_config_t*        phy_config,
                                esp_eth_mac_t**                out_mac,
                                esp_eth_phy_t**                out_phy) {
    if (!devcfg || !mac_config || !phy_config || !out_mac || !out_phy) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_mac = NULL;
    *out_phy = NULL;

    fluidnc_eth_w5500_config_t w5500_config = FLUIDNC_ETH_W5500_DEFAULT_CONFIG(host, devcfg);
    w5500_config.base.int_gpio_num          = int_gpio_num;
    w5500_config.base.poll_period_ms        = poll_period_ms;

    esp_eth_mac_t* mac = fluidnc_eth_mac_new_w5500(&w5500_config, mac_config);
    if (!mac) {
        return ESP_FAIL;
    }
    esp_eth_phy_t* phy = fluidnc_eth_phy_new_w5500(phy_config);
    if (!phy) {
        // Upstream's own factory leaks nothing on this path only if we undo
        // the MAC ourselves; esp_eth_driver_install() is never reached.
        mac->del(mac);
        return ESP_FAIL;
    }

    *out_mac = mac;
    *out_phy = phy;
    return ESP_OK;
}
