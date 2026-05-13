/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_TYPES_H
#define AG_WIFI_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>

// -- Sentinels --

inline constexpr int8_t WIFI_RSSI_INVALID = 0;
inline constexpr uint32_t WIFI_IP_INVALID = 0;

// Maximum number of scan results returned by a single scan call. The HAL
// drops anything beyond this even if the radio reports more APs.
inline constexpr uint16_t WIFI_SCAN_MAX_RESULTS = 32;

// Default DHCP acquisition timeout if not overridden via Kconfig.
inline constexpr uint32_t WIFI_DEFAULT_DHCP_TIMEOUT_MS = 15000;

// -- Enums --

enum class WifiMode : uint8_t {
  Off,
  Sta,
  Ap,
  ApSta,
};

enum class WifiStaState : uint8_t {
  Disconnected,
  Connecting,
  Connected, // L2 associated, no IP yet
  GotIp,     // L3 ready, full connectivity
};

enum class WifiAuthMode : uint8_t {
  open,
  wep,
  wpa_psk,
  wpa2_psk,
  wpa_wpa2_psk,
  wpa3_psk,
  wpa2_wpa3_psk,
  wapi_psk,
  owe,
  unknown,
};

enum class WifiDisconnectReason : uint8_t {
  unknown,
  auth_failed,
  no_ap_found,
  assoc_failed,
  ap_disconnected,
  connection_lost,
  handshake_failed,
  dhcp_failed,
  requested_by_user,
};

enum class WifiPowerSave : uint8_t {
  None,
  MinModem,
  MaxModem,
};

enum class WifiStatus : uint8_t {
  Ok,
  Failed,
  InvalidState,
  InvalidArgument,
  AlreadyInProgress,
};

// -- Data Structs --

struct WifiScanEntry {
  char ssid[33] = {};
  uint8_t bssid[6] = {};
  int8_t rssi = WIFI_RSSI_INVALID;
  WifiAuthMode auth_mode = WifiAuthMode::unknown;
  uint8_t channel = 0;
};

struct WifiScanConfig {
  uint16_t max_results = 20;
  bool show_hidden = false;
};

struct WifiStaConfig {
  char ssid[33] = {};
  char password[64] = {};
  uint8_t max_retry_count = 5; // 0 = no auto-retry
  uint32_t initial_retry_interval_ms = 1000;
  uint32_t max_retry_interval_ms = 30000; // backoff cap
};

struct WifiApConfig {
  char ssid[33] = {};     // required, caller provides
  char password[64] = {}; // empty = open; non-empty = WPA2-PSK
  uint8_t channel = 1;
  uint8_t max_connections = 4;
};

struct WifiStaticIpConfig {
  uint32_t ip = 0; // network byte order
  uint32_t netmask = 0;
  uint32_t gateway = 0;
  uint32_t dns_primary = 0; // 0 = no override
  uint32_t dns_secondary = 0;
};

struct WifiMdnsServiceRecord {
  const char *service_type = nullptr; // e.g., "_http._tcp"
  uint16_t port = 0;
  const char *const *txt_keys = nullptr; // parallel arrays
  const char *const *txt_values = nullptr;
  uint8_t txt_count = 0;
};

struct WifiMdnsConfig {
  const char *hostname = nullptr; // e.g., "airgradient-ab12"
  const WifiMdnsServiceRecord *services = nullptr;
  uint8_t service_count = 0;
};

struct WifiStatusSnapshot {
  WifiMode mode = WifiMode::Off;
  WifiStaState sta_state = WifiStaState::Disconnected;
  uint32_t ip = WIFI_IP_INVALID;
  int8_t rssi = WIFI_RSSI_INVALID;
  uint8_t bssid[6] = {};
  uint8_t channel = 0;
  char ssid[33] = {};
  uint8_t ap_client_count = 0;
};

// -- Callbacks --

/// Invoked when the STA associates with an AP (L2 link up).
/// Query WifiManager::status_snapshot() for connection details.
using WifiConnectedCallback = std::function<void()>;

/// Invoked when the STA disconnects after retry exhaustion or explicit
/// disconnect(). reason indicates why the connection was lost.
using WifiDisconnectedCallback = std::function<void(WifiDisconnectReason reason)>;

/// Invoked when the STA acquires an IP address via DHCP or static config.
/// ip is in network byte order.
using WifiGotIpCallback = std::function<void(uint32_t ip)>;

/// Invoked when a scan completes. results points to an array of count
/// entries. The buffer is only valid for the duration of the callback —
/// callers must copy what they need.
using WifiScanCompleteCallback = std::function<void(const WifiScanEntry *results, uint16_t count)>;

/// Invoked when a client connects to the soft-AP. mac is the client's
/// 6-byte MAC address.
using WifiApClientJoinedCallback = std::function<void(const uint8_t mac[6])>;

/// Invoked when a client disconnects from the soft-AP.
using WifiApClientLeftCallback = std::function<void(const uint8_t mac[6])>;

#endif // AG_WIFI_TYPES_H
