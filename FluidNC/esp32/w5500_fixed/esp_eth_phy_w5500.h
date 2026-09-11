/*
 * Vendored from espressif/esp-eth-drivers @ 400336ad705a2cd88c3d40a1d21bac1085af5865
 * SPDX-License-Identifier: Apache-2.0
 *
 * Do not edit. Regenerate with firmware-patch/rst-watchdog/vendor_w5500.py.
 * The only changes applied are the identifier renames listed in that script;
 * see FluidNC/esp32/w5500_fixed/README.md for why this is vendored at all.
 */
/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#pragma once

#include "esp_eth_com.h"
#include "esp_eth_phy.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
* @brief Create a PHY instance of W5500
*
* @param[in] config: configuration of PHY
*
* @return
*      - instance: create PHY instance successfully
*      - NULL: create PHY instance failed because some error occurred
*/
esp_eth_phy_t *fluidnc_eth_phy_new_w5500(const eth_phy_config_t *config);

#ifdef __cplusplus
}
#endif
