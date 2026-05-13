/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_manager.h"

#include <cstring>

#include "../hal/wifi_hal.h"

#ifdef CONFIG_AG_WIFI_DHCP_TIMEOUT_MS
static constexpr uint32_t k_default_dhcp_timeout_ms = CONFIG_AG_WIFI_DHCP_TIMEOUT_MS;
#else
static constexpr uint32_t k_default_dhcp_timeout_ms = WIFI_DEFAULT_DHCP_TIMEOUT_MS;
#endif

WifiManager::WifiManager(WifiHal &hal) : _hal(hal) {
  _dhcp_timeout_ms = k_default_dhcp_timeout_ms;

  _hal.set_on_sta_connected([this]() { _on_hal_sta_connected(); });
  _hal.set_on_sta_disconnected([this](int reason) { _on_hal_sta_disconnected(reason); });
  _hal.set_on_got_ip([this](uint32_t ip) { _on_hal_got_ip(ip); });
  _hal.set_on_scan_complete(
      [this](const WifiScanEntry *r, uint16_t c) { _on_hal_scan_complete(r, c); });
  _hal.set_on_ap_client_joined([this](const uint8_t mac[6]) { _on_hal_ap_client_joined(mac); });
  _hal.set_on_ap_client_left([this](const uint8_t mac[6]) { _on_hal_ap_client_left(mac); });
  _hal.set_on_dhcp_timeout([this]() { _on_hal_dhcp_timeout(); });
  _hal.set_on_retry_due([this]() { _on_hal_retry_due(); });
}

WifiManager::~WifiManager() {
  // Detach callbacks so the HAL cannot call back into a destroyed manager.
  _hal.set_on_sta_connected(nullptr);
  _hal.set_on_sta_disconnected(nullptr);
  _hal.set_on_got_ip(nullptr);
  _hal.set_on_scan_complete(nullptr);
  _hal.set_on_ap_client_joined(nullptr);
  _hal.set_on_ap_client_left(nullptr);
  _hal.set_on_dhcp_timeout(nullptr);
  _hal.set_on_retry_due(nullptr);
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

WifiDisconnectReason WifiManager::map_disconnect_reason(int raw_reason) {
  // Numeric values come from esp_wifi_types_generic.h (wifi_err_reason_t).
  // We avoid pulling that header into pure-C++ code by hard-coding the
  // values. Keep this table in sync with the spec mapping.
  switch (raw_reason) {
  // AuthFailed
  case 2:   // WIFI_REASON_AUTH_EXPIRE
  case 6:   // WIFI_REASON_NOT_AUTHED (deprecated)
  case 202: // WIFI_REASON_AUTH_FAIL
    return WifiDisconnectReason::auth_failed;

  // NoApFound
  case 201: // WIFI_REASON_NO_AP_FOUND
  case 210: // WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
  case 211: // WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
  case 212: // WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD
    return WifiDisconnectReason::no_ap_found;

  // AssocFailed
  case 4: // WIFI_REASON_ASSOC_EXPIRE (deprecated)
  case 5: // WIFI_REASON_ASSOC_TOOMANY
  case 7: // WIFI_REASON_NOT_ASSOCED (deprecated)
  case 9: // WIFI_REASON_ASSOC_NOT_AUTHED
    return WifiDisconnectReason::assoc_failed;

  // ApDisconnected
  case 8:   // WIFI_REASON_ASSOC_LEAVE
  case 206: // WIFI_REASON_AP_TSF_RESET
    return WifiDisconnectReason::ap_disconnected;

  // ConnectionLost
  case 200: // WIFI_REASON_BEACON_TIMEOUT
  case 208: // WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG
    return WifiDisconnectReason::connection_lost;

  // HandshakeFailed
  case 14:  // WIFI_REASON_MIC_FAILURE
  case 15:  // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
  case 16:  // WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT
  case 17:  // WIFI_REASON_IE_IN_4WAY_DIFFERS
  case 204: // WIFI_REASON_HANDSHAKE_TIMEOUT
    return WifiDisconnectReason::handshake_failed;

  default:
    return WifiDisconnectReason::unknown;
  }
}

bool WifiManager::is_retriable(WifiDisconnectReason reason) {
  switch (reason) {
  case WifiDisconnectReason::ap_disconnected:
  case WifiDisconnectReason::connection_lost:
  case WifiDisconnectReason::handshake_failed:
  case WifiDisconnectReason::unknown:
    return true;
  case WifiDisconnectReason::auth_failed:
  case WifiDisconnectReason::no_ap_found:
  case WifiDisconnectReason::assoc_failed:
  case WifiDisconnectReason::dhcp_failed:
  case WifiDisconnectReason::requested_by_user:
    return false;
  }
  return false;
}

uint32_t WifiManager::compute_backoff_ms(uint32_t initial_ms, uint32_t cap_ms, uint32_t attempt) {
  if (initial_ms == 0) {
    return 0;
  }
  // Doubling per attempt; clamp to cap and avoid overflow.
  uint32_t value = initial_ms;
  for (uint32_t i = 0; i < attempt; ++i) {
    if (value > cap_ms / 2) {
      return cap_ms;
    }
    value *= 2;
  }
  if (value > cap_ms) {
    value = cap_ms;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

WifiStatus WifiManager::set_mdns_config(const WifiMdnsConfig &config) {
  if (config.hostname == nullptr || config.hostname[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }
  if (config.service_count > MAX_MDNS_SERVICES) {
    return WifiStatus::InvalidArgument;
  }

  // Copy hostname into local storage.
  std::strncpy(_mdns_hostname, config.hostname, sizeof(_mdns_hostname) - 1);
  _mdns_hostname[sizeof(_mdns_hostname) - 1] = '\0';

  // Copy service records by value (the service-type / TXT pointers
  // themselves remain caller-owned and must outlive the manager).
  for (uint8_t i = 0; i < config.service_count; ++i) {
    _mdns_services[i] = config.services[i];
  }

  _mdns_config.hostname = _mdns_hostname;
  _mdns_config.services = (config.service_count > 0) ? _mdns_services : nullptr;
  _mdns_config.service_count = config.service_count;
  _has_mdns_config = true;
  return WifiStatus::Ok;
}

WifiStatus WifiManager::set_static_ip(const WifiStaticIpConfig &config) {
  return _hal.set_static_ip(config);
}

WifiStatus WifiManager::clear_static_ip() { return _hal.clear_static_ip(); }

WifiStatus WifiManager::set_power_save(WifiPowerSave mode) { return _hal.set_power_save(mode); }

void WifiManager::set_dhcp_timeout_ms(uint32_t timeout_ms) { _dhcp_timeout_ms = timeout_ms; }

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------

WifiStatus WifiManager::set_mode(WifiMode mode) {
  const WifiMode current = _hal.get_mode();
  if (current == mode) {
    return WifiStatus::Ok;
  }

  // Leaving STA: emit requested_by_user and swallow the driver echo,
  // mirroring disconnect(). Otherwise the echo would be classified as
  // Unknown (retriable) and arm a retry in a mode without STA.
  const bool leaving_sta = (current == WifiMode::Sta || current == WifiMode::ApSta) &&
                           (mode == WifiMode::Off || mode == WifiMode::Ap);
  if (leaving_sta) {
    _hal.cancel_retry_timer();
    _hal.cancel_dhcp_timeout();
    _stop_mdns_if_running();
    const WifiStaState prev = _sta_state;
    _sta_state = WifiStaState::Disconnected;
    _retry_attempt = 0;
    _disconnect_requested = true;
    if (prev != WifiStaState::Disconnected) {
      _emit_disconnected(WifiDisconnectReason::requested_by_user);
    }
  }

  return _hal.set_mode(mode);
}

WifiMode WifiManager::get_mode() const { return _hal.get_mode(); }

// ---------------------------------------------------------------------------
// STA operations
// ---------------------------------------------------------------------------

WifiStatus WifiManager::connect(const WifiStaConfig &config) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return WifiStatus::InvalidState;
  }
  if (config.ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }
  if (_sta_state == WifiStaState::Connecting) {
    return WifiStatus::AlreadyInProgress;
  }

  _sta_config = config;
  _has_sta_config = true;
  _retry_attempt = 0;
  _disconnect_requested = false;
  _start_connect_attempt();
  return WifiStatus::Ok;
}

WifiStatus WifiManager::disconnect() {
  _disconnect_requested = true;
  _hal.cancel_retry_timer();
  _hal.cancel_dhcp_timeout();
  const WifiStaState prev = _sta_state;
  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _stop_mdns_if_running();

  WifiStatus status = WifiStatus::Ok;
  if (prev == WifiStaState::Connecting || prev == WifiStaState::Connected ||
      prev == WifiStaState::GotIp) {
    status = _hal.disconnect_sta();
  }
  // Synthesise a RequestedByUser reason for the product-facing callback.
  // Only fire if we actually had an active connection or pending retry to
  // tear down — silent for an already-disconnected state.
  if (prev != WifiStaState::Disconnected) {
    _emit_disconnected(WifiDisconnectReason::requested_by_user);
  }
  return status;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

WifiStatus WifiManager::start_scan(const WifiScanConfig &config) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return WifiStatus::InvalidState;
  }
  // Spec answer 3: only allow scan when STA is fully disconnected.
  if (_sta_state != WifiStaState::Disconnected) {
    return WifiStatus::InvalidState;
  }
  return _hal.start_scan(config);
}

// ---------------------------------------------------------------------------
// AP
// ---------------------------------------------------------------------------

WifiStatus WifiManager::start_ap(const WifiApConfig &config) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Ap && mode != WifiMode::ApSta) {
    return WifiStatus::InvalidState;
  }
  if (config.ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }
  return _hal.start_ap(config);
}

// ---------------------------------------------------------------------------
// Credential storage
// ---------------------------------------------------------------------------

WifiStatus WifiManager::clear_saved_credentials() { return _hal.clear_saved_credentials(); }

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

WifiStatusSnapshot WifiManager::status_snapshot() const {
  WifiStatusSnapshot snapshot = _hal.get_status();
  // Manager owns sta_state; HAL only sees raw events.
  snapshot.sta_state = _sta_state;
  // Drop stale STA-only fields when disconnected (esp-idf does not fire
  // IP_EVENT_STA_LOST_IP on esp_wifi_stop, so the HAL cache lingers).
  if (_sta_state == WifiStaState::Disconnected) {
    snapshot.ip = WIFI_IP_INVALID;
    snapshot.rssi = WIFI_RSSI_INVALID;
    snapshot.channel = 0;
    for (uint8_t i = 0; i < sizeof(snapshot.bssid); ++i) {
      snapshot.bssid[i] = 0;
    }
    snapshot.ssid[0] = '\0';
  }
  return snapshot;
}

// ---------------------------------------------------------------------------
// Callback setters
// ---------------------------------------------------------------------------

void WifiManager::set_on_connected(WifiConnectedCallback cb) { _on_connected = std::move(cb); }
void WifiManager::set_on_disconnected(WifiDisconnectedCallback cb) {
  _on_disconnected = std::move(cb);
}
void WifiManager::set_on_got_ip(WifiGotIpCallback cb) { _on_got_ip = std::move(cb); }
void WifiManager::set_on_scan_complete(WifiScanCompleteCallback cb) {
  _on_scan_complete = std::move(cb);
}
void WifiManager::set_on_ap_client_joined(WifiApClientJoinedCallback cb) {
  _on_ap_client_joined = std::move(cb);
}
void WifiManager::set_on_ap_client_left(WifiApClientLeftCallback cb) {
  _on_ap_client_left = std::move(cb);
}

// ---------------------------------------------------------------------------
// HAL event handlers
// ---------------------------------------------------------------------------

void WifiManager::_on_hal_sta_connected() {
  _sta_state = WifiStaState::Connected;
  // Note: do NOT reset _retry_attempt here. L2 association succeeding
  // and then dropping again is itself part of the retry sequence — the
  // backoff curve only resets on a full got-IP (see _on_hal_got_ip).
  // Arm DHCP watchdog: ESP-IDF doesn't tell us when DHCP fails, so we
  // give it a bounded window to deliver IP_EVENT_STA_GOT_IP.
  _hal.arm_dhcp_timeout(_dhcp_timeout_ms);
  if (_on_connected) {
    _on_connected();
  }
}

void WifiManager::_on_hal_sta_disconnected(int raw_reason) {
  _hal.cancel_dhcp_timeout();
  // mDNS is interface-bound; tear it down on STA loss.
  _stop_mdns_if_running();

  if (_disconnect_requested) {
    // The user-initiated path already emitted RequestedByUser via
    // disconnect(); swallow the driver echo.
    _disconnect_requested = false;
    _sta_state = WifiStaState::Disconnected;
    return;
  }

  const WifiDisconnectReason reason = map_disconnect_reason(raw_reason);

  // Decide whether to retry. max_retry_count == 0 disables retries
  // entirely.
  const bool can_retry = is_retriable(reason) && _has_sta_config &&
                         _sta_config.max_retry_count > 0 &&
                         _retry_attempt < _sta_config.max_retry_count;
  if (can_retry) {
    const uint32_t delay = compute_backoff_ms(_sta_config.initial_retry_interval_ms,
                                              _sta_config.max_retry_interval_ms, _retry_attempt);
    _retry_attempt += 1;
    // Stay logically "Connecting" while waiting for the next attempt —
    // that keeps scan-while-retrying gated.
    _sta_state = WifiStaState::Connecting;
    _hal.arm_retry_timer(delay);
    return;
  }

  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _emit_disconnected(reason);
}

void WifiManager::_on_hal_got_ip(uint32_t ip) {
  _hal.cancel_dhcp_timeout();
  _sta_state = WifiStaState::GotIp;
  _retry_attempt = 0;
  _start_mdns_if_configured();
  if (_on_got_ip) {
    _on_got_ip(ip);
  }
}

void WifiManager::_on_hal_scan_complete(const WifiScanEntry *results, uint16_t count) {
  if (_on_scan_complete) {
    _on_scan_complete(results, count);
  }
}

void WifiManager::_on_hal_ap_client_joined(const uint8_t mac[6]) {
  if (_on_ap_client_joined) {
    _on_ap_client_joined(mac);
  }
}

void WifiManager::_on_hal_ap_client_left(const uint8_t mac[6]) {
  if (_on_ap_client_left) {
    _on_ap_client_left(mac);
  }
}

void WifiManager::_on_hal_dhcp_timeout() {
  if (_sta_state != WifiStaState::Connected) {
    return;
  }
  // Treat as a non-retriable disconnect so the caller can decide what to
  // do (re-provision, fall back, etc.). Drop the L2 association first.
  _hal.disconnect_sta();
  _stop_mdns_if_running();
  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _emit_disconnected(WifiDisconnectReason::dhcp_failed);
}

void WifiManager::_on_hal_retry_due() {
  if (_disconnect_requested || _sta_state != WifiStaState::Connecting) {
    return;
  }
  _start_connect_attempt();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void WifiManager::_start_connect_attempt() {
  _sta_state = WifiStaState::Connecting;
  const WifiStatus status = _hal.connect_sta(_sta_config.ssid, _sta_config.password);
  if (status != WifiStatus::Ok) {
    // The HAL refused outright (bad args, mode race, ...). Fail fast —
    // this isn't a transient condition the backoff curve fixes.
    _sta_state = WifiStaState::Disconnected;
    _retry_attempt = 0;
    _emit_disconnected(WifiDisconnectReason::unknown);
  }
}

void WifiManager::_stop_mdns_if_running() {
  if (_mdns_running) {
    _hal.stop_mdns();
    _mdns_running = false;
  }
}

void WifiManager::_start_mdns_if_configured() {
  if (!_has_mdns_config || _mdns_running) {
    return;
  }
  if (_hal.start_mdns(_mdns_config) == WifiStatus::Ok) {
    _mdns_running = true;
  }
}

void WifiManager::_emit_disconnected(WifiDisconnectReason reason) {
  if (_on_disconnected) {
    _on_disconnected(reason);
  }
}
