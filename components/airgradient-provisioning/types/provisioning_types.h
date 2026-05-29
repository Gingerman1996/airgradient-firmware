/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_TYPES_H
#define AG_PROVISIONING_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>

#include "hal/ble_types.h"
#include "types/wifi_types.h"

// ---------------------------------------------------------------------------
// Common provisioning data shared by both transports.
// ---------------------------------------------------------------------------

struct ProvisioningData {
  char ssid[33] = {0};
  char password[64] = {0};
  bool disable_cloud = false;
  WifiStaticIpConfig static_ip = {};

  bool has_static_ip() const { return static_ip.ip != 0; }
};

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

enum class ProvisioningEvent : uint8_t {
  Started,       // transports active, waiting for credentials
  Connecting,    // credentials received, WiFi STA connect in progress
  ConnectFailed, // connect attempt failed, still listening
  Connected,     // WiFi STA connected with IP
  Stopped,       // provisioning torn down
};

enum class ProvisioningStopReason : uint8_t {
  ProductRequested, // product called stop()
  TimedOut,         // inactivity timeout expired
};

struct ProvisioningEventInfo {
  ProvisioningEvent event = ProvisioningEvent::Started;
  ProvisioningData data = {};
  uint32_t ip = 0;
  ProvisioningStopReason stop_reason = ProvisioningStopReason::ProductRequested;
};

using ProvisioningEventCallback = std::function<void(const ProvisioningEventInfo &)>;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

enum class ProvisioningState : uint8_t {
  Idle,
  WaitingForCredentials,
  Connecting,
  Connected,
};

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct ProvisioningApConfig {
  char ssid[33] = {0};
  char password[64] = {0};
  uint8_t channel = 1;
  uint8_t max_clients = 4;
};

struct ProvisioningBleConfig {
  const char *device_name = "AirGradient";
  const char *manufacturer_data = nullptr;
  const char *model_name = nullptr;
  const char *serial_number = nullptr;
  const char *firmware_version = nullptr;

  // Forwarded to AgBleServer::set_security(). Defaults preserve the
  // reference product's Just Works + BOND|SC behaviour; AGo overrides
  // auth_flags to SC only for one-shot provisioning without a bond.
  AgBleIoCapability io_capability = AgBleIoCapability::NO_INPUT_NO_OUTPUT;
  uint8_t auth_flags = AgBleAuth::BOND | AgBleAuth::SC;
};

// Which transport(s) ProvisioningManager brings up.
// BleOnly is the safe default; numeric values are locked (do not renumber).
enum class ProvisioningTransport : uint8_t {
  BleOnly = 0, // BLE only, Wi-Fi stays in Sta mode
  WifiOnly,    // captive portal only, no BLE
  Both,        // both; first client to commit wins, the other is torn down
};

struct ProvisioningConfig {
  ProvisioningApConfig ap;
  ProvisioningBleConfig ble;
  ProvisioningTransport transport = ProvisioningTransport::BleOnly;
  uint32_t connect_timeout_ms = 15000;
  uint32_t overall_timeout_ms = 0; // 0 = no timeout
  uint16_t http_port = 80;
};

// ---------------------------------------------------------------------------
// BLE status code constants (Provisioning-owned subset only.
// Application-level codes are sent by product via send_ble_status().)
// ---------------------------------------------------------------------------

namespace ProvisioningBleStatus {

// Provisioning-owned codes (sent automatically by ProvisioningManager).
inline constexpr uint8_t WIFI_CONNECTED = 0;
inline constexpr uint8_t WIFI_CONNECT_FAILED = 10;

// Application-level codes (sent by product via send_ble_status()).
inline constexpr uint8_t CONNECTING_TO_SERVER = 1;
inline constexpr uint8_t SERVER_REACHABLE = 2;
inline constexpr uint8_t MONITOR_CONFIGURED = 3;
inline constexpr uint8_t SERVER_UNREACHABLE = 11;
inline constexpr uint8_t GET_CONFIG_FAILED = 12;
inline constexpr uint8_t NOT_REGISTERED = 13;

} // namespace ProvisioningBleStatus

#endif // AG_PROVISIONING_TYPES_H
