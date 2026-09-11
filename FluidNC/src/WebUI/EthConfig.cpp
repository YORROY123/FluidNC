// Copyright (c) 2026 Mitch Bradley
// Use of this source code is governed by a GPLv3 license that can be found in the LICENSE file.

// Ethernet network module. Mirrors WifiConfig.cpp's structure and settings
// naming where it makes sense, but the physical PHY (pins, chip select,
// phy_type) is configured in config.yaml as Machine::EthPhy (config->_ethernet),
// not via NVS settings -- see the discussion in Machine/EthPhy.h.
//
// $network/type selects whether WiFi or Ethernet actually brings up an
// interface; both modules create their settings unconditionally so that $
// commands still work regardless of which is selected, but only the
// selected one starts hardware.

#include "Config.h"
#if MAX_N_ETH

#    include "Settings.h"
#    include "Machine/MachineConfig.h"

#    include "Channel.h"
#    include "Error.h"
#    include "Module.h"
#    include "Authentication.h"

#    include "Main.h"
#    include "System.h"
#    include <atomic>

#    include "WebUIServer.h"
#    include "TelnetServer.h"
#    include "NotificationsService.h"

#    include "NetSettings.h"
#    include "Driver/localfs.h"
#    include "Driver/watchdog.h"  // feed_watchdog

#    include <ETH.h>

// Direct lwip use, for the watchdog's gateway-ARP liveness probe. Precedent
// for reaching below the Arduino layer: ASyncTCP_Shim.cpp, TelnetClient.cpp.
#    include <esp_netif_net_stack.h>  // esp_netif_get_netif_impl
#    include <lwip/etharp.h>
#    include <lwip/tcpip.h>  // LOCK_TCPIP_CORE

#    include <algorithm>
#    include <string>
#    include <cstring>

namespace WebUI {

    static constexpr int NET_DHCP_MODE   = 0;
    static constexpr int NET_STATIC_MODE = 1;

    static const enum_opt_t ethIpModeOptions = {
        { "DHCP", NET_DHCP_MODE },
        { "Static", NET_STATIC_MODE },
    };

    static const char* NULL_IP = "0.0.0.0";

    static EnumSetting*   _eth_ip_mode;
    static IPaddrSetting* _eth_ip;
    static IPaddrSetting* _eth_gateway;
    static IPaddrSetting* _eth_netmask;

    std::string ethIp() { return IP_string(ETH.localIP()); }

    // ------------------------------------------------------------------
    // W5500 watchdog: sample PHYCFGR every 3 s and gateway ARP every 20 s.
    // 0xFF is valid at 100M/full duplex; floating MISO requires data-path
    // detection. ARP recovery is armed only after seeing the gateway since
    // the last start, so a powered-off gateway causes at most one restart.
    // Recovery runs in the polling task, only outside motion. A failed begin
    // can take about 2 s; the stepper preparation task is separate, but polling
    // of realtime characters is delayed during these driver calls.
    // ------------------------------------------------------------------

    enum class EthRecovery : uint8_t {
        Off,      // not armed (WiFi mode, no ethernet: section, or non-W5500 PHY)
        Monitor,  // interface believed healthy; probing for death
        Stop,     // declared dead; tear the driver down (waits for _nextActionMs)
        Start,    // driver torn down; try to bring it back up
        Jammed,   // stop failed; periodically try hardware reset and restart/stop
    };

    static std::atomic<EthRecovery> _rstate { EthRecovery::Off };
    static std::atomic_flag _driverBusy = ATOMIC_FLAG_INIT;

    // Nonblocking mutual exclusion: a busy $EI returns an error, and poll
    // retries later. Never spin while the other task is in a slow driver call.
    class DriverGuard {
        bool _locked;
    public:
        DriverGuard() : _locked(!_driverBusy.test_and_set(std::memory_order_acquire)) {}
        ~DriverGuard() { if (_locked) _driverBusy.clear(std::memory_order_release); }
        explicit operator bool() const { return _locked; }
    };
    static bool _gatewaySeen = false;
    static bool _logProbe = true;

    static uint32_t _lastProbeMs   = 0;
    static uint32_t _lastArpMs     = 0;
    static uint32_t _nextActionMs  = 0;
    static uint32_t _recoveredMs   = 0;
    static int      _badProbes     = 0;
    static int      _arpMisses     = 0;
    static int      _beginAttempts = 0;
    static int      _failedRounds  = 0;

    static constexpr uint32_t PROBE_PERIOD_MS    = 3000;
    static constexpr int      PROBE_DEAD_COUNT   = 3;  // ~9 s to declare SPI death
    static constexpr uint32_t ARP_PERIOD_MS      = 20000;
    static constexpr int      ARP_DEAD_COUNT     = 6;
    static constexpr int      ATTEMPTS_PER_ROUND = 5;
    static constexpr uint32_t ATTEMPT_SPACING_MS = 1000;
    static constexpr uint32_t HEALTHY_RESET_MS   = 60000;  // healthy time that clears _failedRounds

    // Layer A: is the W5500 answering sanely on SPI?
    static bool probeOk() {
        esp_eth_handle_t h = ETH.handle();  // NULL when the driver is not installed
        if (h == NULL) {
            return false;
        }
        // The W5500 EMAC accepts only PHYCFGR through ETH_CMD_READ_PHY_REG.
        // Register encoding is offset << 16 | bsb << 3; PHYCFGR is offset
        // 0x2E in the common register block (bsb 0), i.e. 0x002E0000.
        constexpr uint32_t        W5500_REG_PHYCFGR = 0x002E0000;
        uint32_t                  v                 = 0;
        esp_eth_phy_reg_rw_data_t reg               = { .reg_addr = W5500_REG_PHYCFGR, .reg_value_p = &v };
        if (esp_eth_ioctl(h, ETH_CMD_READ_PHY_REG, &reg) != ESP_OK) {
            return false;
        }
        if (_logProbe) {
            log_debug("Ethernet W5500 PHYCFGR=" << v);
            _logProbe = false;
        }
        // RST bit 7 must be released. 0xFF is healthy 100M/full-duplex
        // link-up, so it must not be classified as floating MISO here.
        // Do not enforce bit 6 until its startup value is verified on hardware.
        return (v & 0x80) != 0;
    }

    // Layer B: does the gateway's ARP entry stay resolved? Returns true
    // when the data path is alive OR when we cannot judge -- never accuse
    // the interface based on states that are normal (link down, no lease
    // yet, no gateway configured).
    static bool arpAlive() {
        if (!ETH.linkUp() || !ETH.hasIP()) {
            _arpMisses = 0;
            return true;
        }
        ip4_addr_t gw;
        gw.addr = static_cast<uint32_t>(ETH.gatewayIP());  // both sides are network byte order
        if (gw.addr == 0) {
            return true;  // gateway-less LAN: this layer is inert; Layer A still covers SPI death
        }
        esp_netif_t* en = ETH.netif();
        if (en == nullptr) {
            return true;
        }
        struct netif* n = static_cast<struct netif*>(esp_netif_get_netif_impl(en));
        if (n == nullptr) {
            return true;
        }
        struct eth_addr*  hw;
        const ip4_addr_t* ip;
        LOCK_TCPIP_CORE();  // a real mutex here: CONFIG_LWIP_TCPIP_CORE_LOCKING=y; normally held for microseconds
        bool resolved = etharp_find_addr(n, &gw, &hw, &ip) >= 0;
        if (!resolved) {
            etharp_request(n, &gw);  // the answer is checked on the next round
        }
        UNLOCK_TCPIP_CORE();
        if (resolved) {
            _gatewaySeen = true;
        }
        return resolved || !_gatewaySeen;
    }

    // Inter-round backoff, capped: steady state is one round per 10 minutes, forever.
    static uint32_t backoffMs(int round) {
        static const uint32_t table[] = { 5000, 15000, 60000, 300000, 600000 };
        return table[std::min(round, 4)];
    }

    static void declareDead(const char* reason) {
        log_error("Ethernet: interface dead (" << reason << "); restarting");
        // Make isOn()/networkConnected() tell the truth during the outage;
        // EthPhy::init() sets it again on a successful bring-up.
        config->_ethernet->config_ok = false;
        _badProbes                   = 0;
        _arpMisses                   = 0;
        _beginAttempts               = 0;
        _nextActionMs                = millis();
        _rstate                      = EthRecovery::Stop;
    }

    class EthConfig : public Module {
    private:
        static Error showSetEthParams(const char* parameter, AuthenticationLevel auth_level, Channel& out) {
            if (*parameter == '\0') {
                log_stream(out,
                           "IP:" << _eth_ip->getStringValue() << " GW:" << _eth_gateway->getStringValue()
                                 << " MSK:" << _eth_netmask->getStringValue());
                return Error::Ok;
            }
            std::string gateway, netmask, ip;
            if (!(get_param(parameter, "GW", gateway) && get_param(parameter, "MSK", netmask) && get_param(parameter, "IP", ip))) {
                return Error::InvalidValue;
            }
            Error err = _eth_ip->setStringValue(ip);
            if (err == Error::Ok) {
                err = _eth_netmask->setStringValue(netmask);
            }
            if (err == Error::Ok) {
                err = _eth_gateway->setStringValue(gateway);
            }
            return err;
        }

        static void reportStatus(Channel& out) {
            DriverGuard guard;
            if (!guard) {
                log_string(out, "Ethernet: driver busy; retry status shortly");
                return;
            }
            switch (_rstate.load()) {
                case EthRecovery::Off:
                    log_string(out, "Watchdog: off");
                    break;
                case EthRecovery::Monitor:
                    log_string(out, "Watchdog: monitoring");
                    break;
                case EthRecovery::Stop:
                case EthRecovery::Start:
                    log_stream(out, "Watchdog: recovering (attempt " << (_beginAttempts + 1) << ", round " << (_failedRounds + 1) << ")");
                    break;
                case EthRecovery::Jammed:
                    log_string(out, "Watchdog: jammed - hardware recovery pending (requires rst_pin)");
                    break;
            }
            if (!isOn()) {
                log_string(out, "Ethernet: Off");
                return;
            }
            log_stream(out, "Available Size for LocalFS: " << formatBytes(localfs_size()));
            log_stream(out, "Web port: " << WebUI_Server::port());
            log_stream(out, "Hostname: " << ETH.getHostname());
            log_stream(out, "MAC: " << ETH.macAddress().c_str());
            log_stream(out, "Link: " << (ETH.linkUp() ? "Up" : "Down"));
            if (ETH.linkUp()) {
                log_stream(out, "IP Mode: " << _eth_ip_mode->getStringValue());
                log_stream(out, "IP: " << IP_string(ETH.localIP()));
                log_stream(out, "Gateway: " << IP_string(ETH.gatewayIP()));
                log_stream(out, "Mask: " << IP_string(ETH.subnetMask()));
                log_stream(out, "DNS: " << IP_string(ETH.dnsIP()));
            }

            LogStream s(out, "Notifications: ");
            s << (NotificationsService::started() ? "Enabled" : "Disabled");
            if (NotificationsService::started()) {
                s << "(" << NotificationsService::getTypeString() << ")";
            }
        }

        void status_report(Channel& out) override { reportStatus(out); }

        void wifi_stats(JSONencoder& j) override {
            if (!isOn()) {
                j.id_value_object("Current Network Mode", "Ethernet Off");
                return;
            }
            j.id_value_object("Current Network Mode", std::string("Ethernet (") + ETH.macAddress().c_str() + ")");
            j.id_value_object("Available Size for LocalFS", formatBytes(localfs_size()));
            j.id_value_object("Web port", WebUI_Server::port());
            j.id_value_object("Hostname", ETH.getHostname());
            j.id_value_object("Link", ETH.linkUp() ? "Up" : "Down");
            if (ETH.linkUp()) {
                j.id_value_object("IP Mode", _eth_ip_mode->getStringValue());
                j.id_value_object("IP", IP_string(ETH.localIP()));
                j.id_value_object("Gateway", IP_string(ETH.gatewayIP()));
                j.id_value_object("Mask", IP_string(ETH.subnetMask()));
                j.id_value_object("DNS", IP_string(ETH.dnsIP()));
            }
        }

        static Error showEthStatus(const char* parameter, AuthenticationLevel auth_level, Channel& out) {
            (void)parameter;
            (void)auth_level;
            reportStatus(out);
            return Error::Ok;
        }

        static bool isOn() { return config->_ethernet && config->_ethernet->config_ok; }

        // Manual trigger for EthPhy::init(), independent of $network/type,
        // so the PHY bring-up sequence can be traced (e.g. with a logic
        // analyzer) without needing Ethernet selected as the active network
        // type and without the reboot-loop risk of running it automatically
        // at boot before the wiring is known good.
        static Error initEth(const char* parameter, AuthenticationLevel auth_level, Channel& out) {
            (void)parameter;
            (void)auth_level;
            if (!config->_ethernet) {
                log_stream(out, "Ethernet is not configured (no ethernet: section)");
                return Error::InvalidStatement;
            }

            if (inMotionState()) {
                return Error::IdleError;
            }
            DriverGuard guard;
            if (!guard) {
                log_stream(out, "Ethernet recovery busy; retry $EI shortly");
                return Error::InvalidStatement;
            }
            _rstate = EthRecovery::Off;

            // ETH.begin() against a live handle is a phantom success ("ETH
            // Already Started"), and EthPhy::init()'s cs_pin setAttr() can
            // yank CS away from a healthy driver, so always stop first.
            if (ETH.handle() != NULL) {
                log_stream(out, "Stopping existing Ethernet driver...");
                config->_ethernet->config_ok = false;
                stopDriver();
                if (ETH.handle() != NULL && hasHardwareReset()) {
                    unjamDriver();
                }
                if (ETH.handle() != NULL) {
                    // esp_eth_stop() failed; the driver FSM is stuck and a
                    // second stop would just return ESP_ERR_INVALID_STATE.
                    log_stream(out, "Failed to stop Ethernet driver; hardware recovery pending, or reboot with $Bye");
                    _nextActionMs = millis() + 5000;
                    _rstate = EthRecovery::Jammed;
                    return Error::InvalidStatement;
                }
            }

            log_stream(out, "Initializing Ethernet PHY...");
            bool ethActive = networkType() == NetworkTypeEthernet;
            // When Ethernet is the active network type do the full bring-up
            // (hostname, static IP, link/DHCP wait); otherwise just the PHY,
            // preserving the logic-analyzer use case under WiFi mode.
            bool ok = ethActive ? StartEth(true) : config->_ethernet->init();
            log_stream(out, "Ethernet PHY init " << (ok ? "succeeded" : "failed"));

            if (ethActive && config->_ethernet->_phy_type == Machine::EthPhy::W5500) {
                if (ok) {
                    // Re-arm the watchdog on the fresh interface.
                    _badProbes    = 0;
                    _arpMisses    = 0;
                    _failedRounds = 0;
                    _lastProbeMs = _lastArpMs = _recoveredMs = millis();
                    _rstate                                  = EthRecovery::Monitor;
                } else {
                    // Same as the boot-failure path: hand the interface to
                    // the recovery machinery instead of leaving it down.
                    _beginAttempts = 0;
                    _failedRounds  = 0;
                    _nextActionMs  = millis() + 5000;
                    _rstate        = EthRecovery::Stop;
                }
            }
            return ok ? Error::Ok : Error::InvalidStatement;
        }

        static bool hasHardwareReset() {
            return config->_ethernet && config->_ethernet->_phy_type == Machine::EthPhy::W5500
                && config->_ethernet->_rst.defined();
        }

        static void stopDriver() {
            if (hasHardwareReset()) {
                config->_ethernet->hardReset();
            }
            ETH.end();
        }

        static void unjamDriver() {
            if (ETH.handle() == nullptr || !hasHardwareReset()) return;
            config->_ethernet->hardReset();
            // A failed stop has already moved the IDF FSM to STOP. Starting
            // again lets ETH.end() pass its stop step and release the handle.
            esp_err_t err = esp_eth_start(ETH.handle());
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                log_debug("Ethernet jam recovery start failed: " << err);
            }
            ETH.end();
        }

        // block == true is the boot/$EI path: retry ETH.begin() internally
        // and wait for link/DHCP. block == false is the watchdog's recovery
        // path: single begin attempt (the watchdog state machine owns retry
        // spacing) and no waiting -- link and DHCP complete asynchronously,
        // exactly as after a cable replug.
        static bool StartEth(bool block = true) {
            if (!config->_ethernet) {
                log_info("Ethernet is not configured (no ethernet: section)");
                return false;
            }

            log_info("Hostname is " << _hostname->get());

            if (!config->_ethernet->init(block ? 5 : 1)) {
                return false;
            }

            _gatewaySeen = false;
            _logProbe = true;
            ETH.setHostname(_hostname->get());

            if (_eth_ip_mode->get() == NET_STATIC_MODE) {
                uint32_t ip      = (uint32_t)_eth_ip->get();
                uint32_t gateway = (uint32_t)_eth_gateway->get();
                uint32_t netmask = (uint32_t)_eth_netmask->get();
                log_info("Using static Ethernet config IP=" << IP_string(ip) << " GW=" << IP_string(gateway)
                                                              << " MASK=" << IP_string(netmask));
                // Use the gateway as the DNS forwarder, matching WifiConfig's STA behavior.
                if (!ETH.config(IPAddress(ip), IPAddress(gateway), IPAddress(netmask), IPAddress(gateway))) {
                    config->_ethernet->config_ok = false;
                    log_error("Failed to apply static Ethernet config");
                    return false;
                }
            }

            if (block) {
                // Wait briefly for link up / DHCP lease, mirroring WifiConfig::ConnectSTA2AP
                // but without WiFi's association-retry complexity -- a wired link either
                // comes up quickly or it's not plugged in.
                // feed_watchdog() is a no-op if this task isn't TWDT-subscribed (the
                // normal case -- see platform_preinit()), but costs nothing and
                // protects against a multi-second blocking loop like this one
                // tripping the watchdog if that ever changes.
                for (int i = 0; i < 50 && !ETH.linkUp(); ++i) {
                    feed_watchdog();
                    delay_ms(100);
                }
                if (!ETH.linkUp()) {
                    log_info("Ethernet link is down (cable unplugged?)");
                    // Not fatal: the module stays "on" and will report link-down status;
                    // it will come up automatically if a cable is connected later.
                } else {
                    log_info("Ethernet link up");
                    for (int i = 0; i < 50 && _eth_ip_mode->get() == NET_DHCP_MODE && ETH.localIP() == IPAddress((uint32_t)0); ++i) {
                        feed_watchdog();
                        delay_ms(100);
                    }
                    log_info("Ethernet IP is " << IP_string(ETH.localIP()));
                }
            }
            return true;
        }

    public:
        EthConfig(const char* name) : Module(name) {}

        void init() override {
            _eth_ip_mode = new EnumSetting("Ethernet IP Mode", WEBSET, WA, NULL, "Ethernet/IPMode", NET_DHCP_MODE, &ethIpModeOptions);
            _eth_ip      = new IPaddrSetting("Ethernet Static IP", WEBSET, WA, NULL, "Ethernet/IP", NULL_IP);
            _eth_gateway = new IPaddrSetting("Ethernet Static Gateway", WEBSET, WA, NULL, "Ethernet/Gateway", NULL_IP);
            _eth_netmask = new IPaddrSetting("Ethernet Static Mask", WEBSET, WA, NULL, "Ethernet/Netmask", NULL_IP);

            new WebReportCommand(NULL, WEBCMD, WG, NULL, "Ethernet/Status", showEthStatus, anyState);
            new WebCommand("IP=ipaddress MSK=netmask GW=gateway", WEBCMD, WA, NULL, "Ethernet/Setup", showSetEthParams);
            new WebCommand(NULL, WEBCMD, WA, "EI", "Ethernet/Init", initEth, anyState);

            if (networkType() != NetworkTypeEthernet) {
                log_info("Ethernet is disabled ($network/type is WiFi)");
                return;
            }

            bool w5500 = config->_ethernet && config->_ethernet->_phy_type == Machine::EthPhy::W5500;
            if (StartEth()) {
                log_info("Ethernet on");
                if (w5500) {
                    _rstate      = EthRecovery::Monitor;
                    _lastProbeMs = _lastArpMs = _recoveredMs = millis();
                }
            } else {
                log_info("Ethernet off");
                if (w5500) {
                    // Boot-time init failed. Hand the interface to the same
                    // recovery machinery instead of giving up: measured on
                    // marginal-SPI hardware, boot init can fail 10/10 while
                    // the identical sequence moments later ($EI) succeeds
                    // 10/10 -- no reason a human has to type it.
                    log_info("Ethernet: will keep retrying in the background");
                    _beginAttempts = 0;
                    _failedRounds  = 0;
                    _nextActionMs  = millis() + 5000;
                    _rstate        = EthRecovery::Stop;
                }
            }
        }

        void deinit() override {
            _rstate = EthRecovery::Off;
            // Module shutdown must finish after any in-flight watchdog pass.
            while (_driverBusy.test_and_set(std::memory_order_acquire)) delay_ms(10);
            _rstate = EthRecovery::Off;
            if (ETH.handle() != nullptr) stopDriver();
            _driverBusy.clear(std::memory_order_release);
        }

        // Watchdog state machine; see the block comment above EthRecovery.
        // Called continuously from the protocol main loop. Slow driver calls run only during recovery outside motion;
        // when healthy the cost is one SPI register read per 3 s and one
        // ARP lookup per 20 s.
        void poll() override {
            DriverGuard guard;
            if (!guard || _rstate == EthRecovery::Off) {
                return;
            }
            uint32_t now = millis();
            switch (_rstate.load()) {
                case EthRecovery::Monitor:
                    if (now - _lastProbeMs >= PROBE_PERIOD_MS) {
                        _lastProbeMs = now;
                        if (probeOk()) {
                            _badProbes = 0;
                            if (_failedRounds && now - _recoveredMs >= HEALTHY_RESET_MS) {
                                _failedRounds = 0;
                            }
                        } else if (++_badProbes >= PROBE_DEAD_COUNT) {
                            declareDead("W5500 not responding on SPI");
                            break;
                        }
                    }
                    if (now - _lastArpMs >= ARP_PERIOD_MS) {
                        _lastArpMs = now;
                        if (arpAlive()) {
                            _arpMisses = 0;
                        } else if (++_arpMisses >= ARP_DEAD_COUNT) {
                            declareDead("gateway unreachable (data path dead)");
                            break;
                        }
                    }
                    break;
                case EthRecovery::Stop:
                    if (inMotionState() || (int32_t)(now - _nextActionMs) < 0) {
                        break;
                    }
                    if (ETH.handle() != NULL) {
                        stopDriver();
                        if (ETH.handle() != NULL) {
                            // The FSM may already be STOP; recover on a later pass.
                            log_error("Ethernet: driver stop failed; entering hardware recovery");
                            _nextActionMs = millis() + 5000;
                            _rstate = EthRecovery::Jammed;
                            break;
                        }
                    }
                    _nextActionMs = now;  // begin on the next pass (splits the two slow ops)
                    _rstate       = EthRecovery::Start;
                    break;
                case EthRecovery::Start:
                    if (inMotionState() || (int32_t)(now - _nextActionMs) < 0) {
                        break;
                    }
                    // probeOk() guards against a phantom success: a begin
                    // that "succeeded" against a half-installed driver.
                    if (StartEth(false) && probeOk()) {
                        log_info("Ethernet: interface restarted (attempt " << (_beginAttempts + 1) << ", round " << (_failedRounds + 1)
                                                                           << ")");
                        _recoveredMs = _lastProbeMs = _lastArpMs = millis();
                        _badProbes                               = 0;
                        _arpMisses                               = 0;
                        _rstate = EthRecovery::Monitor;  // _failedRounds resets after HEALTHY_RESET_MS of health
                        break;
                    }
                    if (++_beginAttempts < ATTEMPTS_PER_ROUND) {
                        _nextActionMs = now + ATTEMPT_SPACING_MS;
                    } else {
                        ++_failedRounds;
                        _beginAttempts = 0;
                        uint32_t wait  = backoffMs(_failedRounds - 1);
                        if (_failedRounds <= 5) {
                            log_warn("Ethernet: recovery round " << _failedRounds << " failed; retrying in " << (wait / 1000) << " s");
                        } else if (_failedRounds == 6) {
                            log_error("Ethernet: recovery failing persistently; will keep retrying every 10 minutes. "
                                      "Check SPI wiring or reboot with $Bye");
                        }  // rounds > 6: silent
                        _nextActionMs = now + wait;
                    }
                    config->_ethernet->config_ok = false;
                    _rstate = EthRecovery::Stop;  // always re-end() before the next begin (cleans up half-installs)
                    break;
                case EthRecovery::Jammed:
                    if (inMotionState() || (int32_t)(now - _nextActionMs) < 0 || !hasHardwareReset()) break;
                    unjamDriver();
                    _nextActionMs = millis() + 60000;
                    if (ETH.handle() == nullptr) {
                        _nextActionMs = millis();
                        _rstate = EthRecovery::Start;
                    }
                    break;
                default:
                    break;
            }
        }

        bool is_radio() override { return false; }

        ~EthConfig() { deinit(); }
    };

    ModuleFactory::InstanceBuilder<EthConfig> __attribute__((init_priority(106))) eth_module("ethernet", true);
}
#endif
