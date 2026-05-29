/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_MANAGER_H
#define AG_WIFI_MANAGER_H

#include "../types/wifi_types.h"

class WifiHal;

/// High-level Wi-Fi manager. This is the product-facing API.
///
/// Owns: mode state machine, connection retry with exponential backoff,
/// mDNS auto-start/stop lifecycle, disconnect reason normalisation, DHCP
/// acquisition timeout policy.
///
/// Pure C++ logic — host-testable when constructed with a mock WifiHal.
///
/// Callbacks fire in the ESP-IDF system event loop task context.
/// Callers must not block inside callbacks or call back into WifiManager.
///
/// ISR-safe: no
/// Thread-safe: no
/// Blocking: no (all outcomes delivered via callbacks)
class WifiManager {
public:
  static constexpr uint16_t MAX_MDNS_SERVICES = 4;

  explicit WifiManager(WifiHal &hal);
  ~WifiManager();

  WifiManager(const WifiManager &) = delete;
  WifiManager &operator=(const WifiManager &) = delete;

  // -- Configuration (call before connect) --

  /// Set the mDNS hostname and service records. Copied internally.
  /// mDNS starts automatically when STA gets an IP and stops on
  /// disconnect or mode Off.
  WifiStatus set_mdns_config(const WifiMdnsConfig &config);

  /// Configure a static IP for STA connections. Persists until
  /// clear_static_ip() is called.
  WifiStatus set_static_ip(const WifiStaticIpConfig &config);

  /// Clear the static IP configuration. Revert to DHCP for
  /// subsequent connections.
  WifiStatus clear_static_ip();

  /// Set the Wi-Fi power save mode. Only meaningful in STA mode.
  /// Default: WifiPowerSave::None.
  WifiStatus set_power_save(WifiPowerSave mode);

  /// Override the DHCP acquisition timeout. Defaults to
  /// CONFIG_AG_WIFI_DHCP_TIMEOUT_MS (or WIFI_DEFAULT_DHCP_TIMEOUT_MS on
  /// host builds).
  void set_dhcp_timeout_ms(uint32_t timeout_ms);

  // -- Mode Control --

  /// Set the Wi-Fi operating mode. All transitions are legal.
  /// Idempotent: setting the current mode returns Ok.
  /// Setting Off tears down STA, AP, and mDNS.
  WifiStatus set_mode(WifiMode mode);

  /// Return the current Wi-Fi operating mode.
  WifiMode get_mode() const;

  // -- STA Operations --

  /// Start a STA connection. Non-blocking. Requires Sta or ApSta mode
  /// (returns InvalidState otherwise).
  ///
  /// max_retry_count = 0 disables auto-retry.
  ///
  /// Empty config.ssid => use NVS-saved credentials; returns NotFound
  /// (no driver call) when the HAL reports none. Retry / backoff
  /// fields still apply since they are manager-owned policy.
  ///
  /// Outcome delivered via on_connected / on_got_ip / on_disconnected.
  WifiStatus connect(const WifiStaConfig &config);

  /// True when the HAL has STA credentials persisted in NVS. Lets
  /// callers pick between saved-creds and fallback paths without
  /// attempting a connect.
  bool has_saved_credentials() const;

  /// Disconnect from the current AP and cancel any pending retry.
  WifiStatus disconnect();

  // -- Scan --

  /// Trigger an async Wi-Fi scan. Results delivered via
  /// on_scan_complete callback. Requires Sta or ApSta mode AND the STA
  /// must be disconnected (no scan-while-connected at v1).
  WifiStatus start_scan(const WifiScanConfig &config = {});

  // -- AP Operations --

  /// Start the soft-AP. Requires Ap or ApSta mode (returns
  /// InvalidState otherwise). SSID is required (must not be empty).
  WifiStatus start_ap(const WifiApConfig &config);

  // -- Credential Storage --

  /// Erase saved Wi-Fi credentials from NVS. For factory-reset
  /// and reprovisioning scenarios.
  WifiStatus clear_saved_credentials();

  // -- Status --

  /// Return a snapshot of the current Wi-Fi state (mode, STA state,
  /// IP, RSSI, BSSID, channel, AP client count).
  WifiStatusSnapshot status_snapshot() const;

  // -- Product-Facing Callbacks --

  /// Invoked when STA associates with an AP (L2 link up).
  void set_on_connected(WifiConnectedCallback cb);

  /// Invoked when STA disconnects after retry exhaustion or explicit
  /// disconnect(). Reason is normalised from raw ESP-IDF codes.
  void set_on_disconnected(WifiDisconnectedCallback cb);

  /// Invoked when STA acquires an IP address (DHCP or static).
  /// mDNS is started automatically before this callback fires.
  void set_on_got_ip(WifiGotIpCallback cb);

  /// Invoked when a scan completes. Buffer valid only during callback.
  void set_on_scan_complete(WifiScanCompleteCallback cb);

  /// Invoked when a client joins the soft-AP.
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb);

  /// Invoked when a client leaves the soft-AP.
  void set_on_ap_client_left(WifiApClientLeftCallback cb);

  // -- Visible for tests --

  /// Map a raw ESP-IDF wifi_err_reason_t (int) to a normalised
  /// WifiDisconnectReason. Pure function; static so tests can call it
  /// without instantiating a manager.
  static WifiDisconnectReason map_disconnect_reason(int raw_reason);

  /// True if the given normalised reason is eligible for auto-retry.
  static bool is_retriable(WifiDisconnectReason reason);

  /// Compute the next retry backoff (capped exponential, doubling each
  /// attempt). attempt is 0-based.
  static uint32_t compute_backoff_ms(uint32_t initial_ms, uint32_t cap_ms, uint32_t attempt);

private:
  // -- HAL event handlers (registered with hal during construction) --
  void _on_hal_sta_connected();
  void _on_hal_sta_disconnected(int raw_reason);
  void _on_hal_got_ip(uint32_t ip);
  void _on_hal_scan_complete(const WifiScanEntry *results, uint16_t count);
  void _on_hal_ap_client_joined(const uint8_t mac[6]);
  void _on_hal_ap_client_left(const uint8_t mac[6]);
  void _on_hal_dhcp_timeout();
  void _on_hal_retry_due();

  // -- Internal helpers --
  void _start_connect_attempt();
  void _stop_mdns_if_running();
  void _start_mdns_if_configured();
  void _emit_disconnected(WifiDisconnectReason reason);

  WifiHal &_hal;

  // Configuration / latched state
  WifiStaConfig _sta_config = {};
  bool _has_sta_config = false;
  WifiMdnsConfig _mdns_config = {};
  WifiMdnsServiceRecord _mdns_services[MAX_MDNS_SERVICES] = {};
  char _mdns_hostname[64] = {};
  bool _has_mdns_config = false;
  bool _mdns_running = false;
  uint32_t _dhcp_timeout_ms = WIFI_DEFAULT_DHCP_TIMEOUT_MS;

  // Live state
  WifiStaState _sta_state = WifiStaState::Disconnected;
  uint32_t _retry_attempt = 0; // 0-based count of retries already used
  bool _disconnect_requested = false;

  // Product-facing callbacks
  WifiConnectedCallback _on_connected;
  WifiDisconnectedCallback _on_disconnected;
  WifiGotIpCallback _on_got_ip;
  WifiScanCompleteCallback _on_scan_complete;
  WifiApClientJoinedCallback _on_ap_client_joined;
  WifiApClientLeftCallback _on_ap_client_left;
};

#endif // AG_WIFI_MANAGER_H
