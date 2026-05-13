/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_HAL_H
#define AG_WIFI_HAL_H

#include <functional>

#include "../types/wifi_types.h"

/// Thin hardware abstraction for Wi-Fi operations.
///
/// Implemented by the ESP-IDF driver (EspWifiHal). Used by WifiManager.
/// Product code should not use this interface directly.
///
/// The HAL translates raw ESP-IDF events into typed callbacks. It does NOT
/// own retry logic, state machine management, or mDNS lifecycle. The
/// timer-arming methods exist so WifiManager can keep its scheduling
/// decisions in pure C++ while delegating the actual timer source to the
/// driver (`esp_timer` on hardware, mockable on host).
///
/// ISR-safe: no
/// Thread-safe: no
/// Blocking: implementation-defined per method
class WifiHal {
public:
  virtual ~WifiHal() = default;

  // -- Lifecycle --

  /// Initialize the Wi-Fi subsystem (netif, event loop, default config).
  /// Must be called before any other method.
  virtual WifiStatus init() = 0;

  /// Tear down the Wi-Fi subsystem. Safe to call multiple times.
  virtual void deinit() = 0;

  // -- Mode --

  /// Set the Wi-Fi operating mode. The driver handles internal
  /// teardown/reinit as needed.
  virtual WifiStatus set_mode(WifiMode mode) = 0;

  /// Return the current Wi-Fi operating mode.
  virtual WifiMode get_mode() const = 0;

  // -- STA --

  /// Start STA connection to the given SSID/password. Non-blocking.
  /// Outcome delivered via on_sta_connected / on_sta_disconnected
  /// callbacks.
  virtual WifiStatus connect_sta(const char *ssid, const char *password) = 0;

  /// Disconnect from the current AP. Non-blocking.
  virtual WifiStatus disconnect_sta() = 0;

  // -- Static IP --

  /// Configure a static IP for the STA interface. Takes effect on the
  /// next connection (or immediately if already connected). Persists
  /// until clear_static_ip() is called.
  virtual WifiStatus set_static_ip(const WifiStaticIpConfig &config) = 0;

  /// Clear the static IP configuration. Revert to DHCP.
  virtual WifiStatus clear_static_ip() = 0;

  // -- Scan --

  /// Trigger an async Wi-Fi scan. Results delivered via
  /// on_scan_complete callback.
  virtual WifiStatus start_scan(const WifiScanConfig &config) = 0;

  // -- AP --

  /// Start the soft-AP with the given configuration.
  virtual WifiStatus start_ap(const WifiApConfig &config) = 0;

  /// Stop the soft-AP.
  virtual WifiStatus stop_ap() = 0;

  // -- Status --

  /// Return a snapshot of the current Wi-Fi state.
  virtual WifiStatusSnapshot get_status() const = 0;

  // -- Power Save --

  /// Set the Wi-Fi power save mode. Only meaningful in STA mode.
  virtual WifiStatus set_power_save(WifiPowerSave mode) = 0;

  // -- mDNS --

  /// Start mDNS with the given hostname and service records.
  virtual WifiStatus start_mdns(const WifiMdnsConfig &config) = 0;

  /// Stop mDNS and remove all service records.
  virtual WifiStatus stop_mdns() = 0;

  // -- Credential Storage --

  /// Erase saved Wi-Fi credentials from NVS.
  virtual WifiStatus clear_saved_credentials() = 0;

  // -- Timers (driven by WifiManager) --
  //
  // ESP-IDF does not expose a DHCP-failure event, and the manager owns the
  // retry backoff curve. Both timers are single-shot. arm_* replaces any
  // previously armed instance.

  /// Arm a one-shot DHCP-acquisition watchdog. Fires the on_dhcp_timeout
  /// callback after timeout_ms if not cancelled first.
  virtual WifiStatus arm_dhcp_timeout(uint32_t timeout_ms) = 0;

  /// Cancel a pending DHCP timeout. Safe to call when none is armed.
  virtual WifiStatus cancel_dhcp_timeout() = 0;

  /// Arm a one-shot retry timer. Fires the on_retry_due callback after
  /// delay_ms if not cancelled first.
  virtual WifiStatus arm_retry_timer(uint32_t delay_ms) = 0;

  /// Cancel a pending retry timer. Safe to call when none is armed.
  virtual WifiStatus cancel_retry_timer() = 0;

  // -- Event Callbacks (set by WifiManager) --

  /// Invoked when STA associates with an AP (L2 link up).
  virtual void set_on_sta_connected(WifiConnectedCallback cb) = 0;

  /// Invoked when STA disconnects. reason is the raw ESP-IDF
  /// wifi_err_reason_t value (int). WifiManager maps this to
  /// WifiDisconnectReason.
  virtual void set_on_sta_disconnected(std::function<void(int reason)> cb) = 0;

  /// Invoked when STA acquires an IP address.
  virtual void set_on_got_ip(WifiGotIpCallback cb) = 0;

  /// Invoked when a scan completes with results.
  virtual void set_on_scan_complete(WifiScanCompleteCallback cb) = 0;

  /// Invoked when a client joins the soft-AP.
  virtual void set_on_ap_client_joined(WifiApClientJoinedCallback cb) = 0;

  /// Invoked when a client leaves the soft-AP.
  virtual void set_on_ap_client_left(WifiApClientLeftCallback cb) = 0;

  /// Invoked when the DHCP watchdog fires before got-IP arrives.
  virtual void set_on_dhcp_timeout(std::function<void()> cb) = 0;

  /// Invoked when the retry timer fires.
  virtual void set_on_retry_due(std::function<void()> cb) = 0;
};

#endif // AG_WIFI_HAL_H
