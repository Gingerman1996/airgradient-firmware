#include "test_wifi.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"

#include "drivers/esp_wifi_hal.h"
#include "rtos.h"
#include "services/wifi_manager.h"
#include "types/wifi_types.h"

// ---------------------------------------------------------------------------
// Build-time configuration. Override with -D in EXTRA_CXXFLAGS.
// ---------------------------------------------------------------------------

// Comment to run WiFi sta
// #define TEST_WIFI_RUN_AP

// Opt-in sub-features (STA flow only; ignored when TEST_WIFI_RUN_AP is set):
//   TEST_WIFI_STATIC_IP    — apply a static IP before connect and PASS/FAIL
//                            verify the got-IP callback reports the same IP
//   TEST_WIFI_MODE_SWITCH  — after the initial connect settles, run a
//                            scripted FSM sweep:
//                              Sta -> ApSta -> Ap -> Off -> Sta
#define TEST_WIFI_STATIC_IP
#define TEST_WIFI_MODE_SWITCH

#ifndef TEST_WIFI_SSID
#define TEST_WIFI_SSID "airgradient"
#endif
#ifndef TEST_WIFI_PASSWORD
#define TEST_WIFI_PASSWORD "cleanair"
#endif

#ifndef TEST_WIFI_AP_SSID
#define TEST_WIFI_AP_SSID "airgradient"
#endif
#ifndef TEST_WIFI_AP_PASSWORD
// Empty string => open AP. WPA2-PSK requires >= 8 characters.
#define TEST_WIFI_AP_PASSWORD "cleanair"
#endif

#ifndef TEST_WIFI_HOSTNAME
#define TEST_WIFI_HOSTNAME "agwifi-test"
#endif

#ifndef TEST_WIFI_STATUS_INTERVAL_MS
#define TEST_WIFI_STATUS_INTERVAL_MS 5000U
#endif

// Defaults sized for the `bles` subnet used during bring-up. Pick an
// address outside the AP's DHCP pool to avoid lease conflicts.
#ifndef TEST_WIFI_STATIC_IP_ADDR
#define TEST_WIFI_STATIC_IP_ADDR "192.168.110.251"
#endif
#ifndef TEST_WIFI_STATIC_IP_NETMASK
#define TEST_WIFI_STATIC_IP_NETMASK "255.255.255.0"
#endif
#ifndef TEST_WIFI_STATIC_IP_GATEWAY
#define TEST_WIFI_STATIC_IP_GATEWAY "192.168.110.1"
#endif
#ifndef TEST_WIFI_STATIC_IP_DNS1
#define TEST_WIFI_STATIC_IP_DNS1 "8.8.8.8"
#endif
#ifndef TEST_WIFI_STATIC_IP_DNS2
#define TEST_WIFI_STATIC_IP_DNS2 "1.1.1.1"
#endif

#ifndef TEST_WIFI_MODE_SWITCH_DWELL_MS
#define TEST_WIFI_MODE_SWITCH_DWELL_MS 30000U
#endif

static constexpr const char *TAG = "test_wifi";

namespace {

const char *_mode_to_str(WifiMode mode) {
  switch (mode) {
  case WifiMode::Off:
    return "Off";
  case WifiMode::Sta:
    return "Sta";
  case WifiMode::Ap:
    return "Ap";
  case WifiMode::ApSta:
    return "ApSta";
  }
  return "?";
}

const char *_sta_state_to_str(WifiStaState state) {
  switch (state) {
  case WifiStaState::Disconnected:
    return "Disconnected";
  case WifiStaState::Connecting:
    return "Connecting";
  case WifiStaState::Connected:
    return "Connected";
  case WifiStaState::GotIp:
    return "GotIp";
  }
  return "?";
}

const char *_reason_to_str(WifiDisconnectReason reason) {
  switch (reason) {
  case WifiDisconnectReason::unknown:
    return "Unknown";
  case WifiDisconnectReason::auth_failed:
    return "AuthFailed";
  case WifiDisconnectReason::no_ap_found:
    return "NoApFound";
  case WifiDisconnectReason::assoc_failed:
    return "AssocFailed";
  case WifiDisconnectReason::ap_disconnected:
    return "ApDisconnected";
  case WifiDisconnectReason::connection_lost:
    return "ConnectionLost";
  case WifiDisconnectReason::handshake_failed:
    return "HandshakeFailed";
  case WifiDisconnectReason::dhcp_failed:
    return "DhcpFailed";
  case WifiDisconnectReason::requested_by_user:
    return "RequestedByUser";
  }
  return "?";
}

const char *_auth_to_str(WifiAuthMode mode) {
  switch (mode) {
  case WifiAuthMode::open:
    return "OPEN";
  case WifiAuthMode::wep:
    return "WEP";
  case WifiAuthMode::wpa_psk:
    return "WPA-PSK";
  case WifiAuthMode::wpa2_psk:
    return "WPA2-PSK";
  case WifiAuthMode::wpa_wpa2_psk:
    return "WPA/WPA2-PSK";
  case WifiAuthMode::wpa3_psk:
    return "WPA3-PSK";
  case WifiAuthMode::wpa2_wpa3_psk:
    return "WPA2/WPA3-PSK";
  case WifiAuthMode::wapi_psk:
    return "WAPI-PSK";
  case WifiAuthMode::owe:
    return "OWE";
  case WifiAuthMode::unknown:
  default:
    return "?";
  }
}

void _log_ip(uint32_t ip) {
  // ip is in network byte order (little-endian on ESP32 host).
  const uint8_t b0 = static_cast<uint8_t>(ip & 0xFF);
  const uint8_t b1 = static_cast<uint8_t>((ip >> 8) & 0xFF);
  const uint8_t b2 = static_cast<uint8_t>((ip >> 16) & 0xFF);
  const uint8_t b3 = static_cast<uint8_t>((ip >> 24) & 0xFF);
  ESP_LOGI(TAG, "got IP: %u.%u.%u.%u", b0, b1, b2, b3);
}

void _log_snapshot(const WifiStatusSnapshot &snap) {
  ESP_LOGI(TAG, "status: mode=%s sta=%s rssi=%d ch=%u ip=0x%08" PRIX32 " ap_clients=%u",
           _mode_to_str(snap.mode), _sta_state_to_str(snap.sta_state), snap.rssi, snap.channel,
           snap.ip, snap.ap_client_count);
}

// File-scope flag toggled by the scan-complete callback (set in
// run_test_wifi). volatile because the callback fires from the ESP-IDF
// system event loop task while _run_sta_test polls from app_main.
volatile bool _scan_done = false;

#ifdef TEST_WIFI_STATIC_IP
// Captured at static-IP setup so the on_got_ip callback can PASS/FAIL the
// IP it observes against the value we asked the manager to apply.
volatile uint32_t _expected_static_ip = 0;
#endif

// Parse a dotted-decimal IPv4 string into a uint32_t in network byte order
// (matches WifiStaticIpConfig field layout and esp_netif_ip_info_t).
uint32_t _parse_ip4(const char *str) {
  esp_ip4_addr_t addr = {};
  esp_netif_str_to_ip4(str, &addr);
  return addr.addr;
}

#ifndef TEST_WIFI_RUN_AP

bool _wait_for_scan_complete(uint32_t timeout_ms) {
  const uint64_t deadline = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline) {
    if (_scan_done) {
      return true;
    }
    RTOS::delay_ms(50);
  }
  return false;
}

WifiApConfig _build_ap_config() {
  WifiApConfig ap = {};
  std::strncpy(ap.ssid, TEST_WIFI_AP_SSID, sizeof(ap.ssid) - 1);
  std::strncpy(ap.password, TEST_WIFI_AP_PASSWORD, sizeof(ap.password) - 1);
  ap.channel = 1;
  ap.max_connections = 4;
  return ap;
}

#ifdef TEST_WIFI_MODE_SWITCH
void _log_step(const char *name, const WifiManager &mgr) {
  ESP_LOGI(TAG, "=== STEP: %s ===", name);
  _log_snapshot(mgr.status_snapshot());
}
#endif

void _run_sta_test(WifiManager &mgr) {
  static constexpr const char *kSsid = TEST_WIFI_SSID;
  static constexpr const char *kPassword = TEST_WIFI_PASSWORD;

  if (kSsid[0] == '\0') {
    ESP_LOGE(TAG, "TEST_WIFI_SSID is empty — pass it via "
                  "-DEXTRA_CXXFLAGS='-DTEST_WIFI_SSID=\\\"...\\\" "
                  "-DTEST_WIFI_PASSWORD=\\\"...\\\"'");
    return;
  }

  // Configure mDNS BEFORE connecting; the manager auto-starts mDNS on
  // got-IP and auto-stops on disconnect / mode-Off.
  WifiMdnsServiceRecord svc = {};
  svc.service_type = "_http._tcp";
  svc.port = 80;
  WifiMdnsConfig mdns = {};
  mdns.hostname = TEST_WIFI_HOSTNAME;
  mdns.services = &svc;
  mdns.service_count = 1;
  if (mgr.set_mdns_config(mdns) != WifiStatus::Ok) {
    ESP_LOGW(TAG, "set_mdns_config failed");
  }

  if (mgr.set_mode(WifiMode::Sta) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Sta) failed");
    return;
  }

#ifdef TEST_WIFI_STATIC_IP
  // Apply static IP BEFORE connect so the driver skips DHCP after L2
  // association and applies the static config immediately. The on_got_ip
  // callback PASS/FAILs the observed IP against _expected_static_ip.
  WifiStaticIpConfig static_ip = {};
  static_ip.ip = _parse_ip4(TEST_WIFI_STATIC_IP_ADDR);
  static_ip.netmask = _parse_ip4(TEST_WIFI_STATIC_IP_NETMASK);
  static_ip.gateway = _parse_ip4(TEST_WIFI_STATIC_IP_GATEWAY);
  static_ip.dns_primary = _parse_ip4(TEST_WIFI_STATIC_IP_DNS1);
  static_ip.dns_secondary = _parse_ip4(TEST_WIFI_STATIC_IP_DNS2);
  _expected_static_ip = static_ip.ip;
  ESP_LOGI(TAG, "applying static IP: addr=%s nm=%s gw=%s dns1=%s dns2=%s", TEST_WIFI_STATIC_IP_ADDR,
           TEST_WIFI_STATIC_IP_NETMASK, TEST_WIFI_STATIC_IP_GATEWAY, TEST_WIFI_STATIC_IP_DNS1,
           TEST_WIFI_STATIC_IP_DNS2);
  if (mgr.set_static_ip(static_ip) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_static_ip failed");
    return;
  }
#endif

  // Trigger a scan first — only legal while disconnected (per spec).
  // Wait for the scan-complete callback to set _scan_done before
  // proceeding; otherwise esp_wifi_connect() below would cancel the
  // in-progress scan and the callback would deliver 0 results.
  _scan_done = false;
  ESP_LOGI(TAG, "starting scan...");
  if (mgr.start_scan() != WifiStatus::Ok) {
    ESP_LOGW(TAG, "start_scan failed");
  } else if (!_wait_for_scan_complete(10000)) {
    ESP_LOGW(TAG, "scan did not complete within 10s — proceeding anyway");
  }

  WifiStaConfig cfg;
  std::strncpy(cfg.ssid, kSsid, sizeof(cfg.ssid) - 1);
  std::strncpy(cfg.password, kPassword, sizeof(cfg.password) - 1);
  cfg.max_retry_count = 5;
  cfg.initial_retry_interval_ms = 1000;
  cfg.max_retry_interval_ms = 16000;

  ESP_LOGI(TAG, "connecting to '%s'...", kSsid);
  const WifiStatus connect_status = mgr.connect(cfg);
  if (connect_status != WifiStatus::Ok) {
    ESP_LOGE(TAG, "connect() returned %d", static_cast<int>(connect_status));
    return;
  }

#ifdef TEST_WIFI_MODE_SWITCH
  // Let the initial connect settle (L2 + DHCP/static IP + mDNS) before
  // starting the FSM sweep. Each step dwells for visual / log inspection.
  ESP_LOGI(TAG, "=== mode-switch sweep starts in %ums ===", TEST_WIFI_MODE_SWITCH_DWELL_MS);
  RTOS::delay_ms(TEST_WIFI_MODE_SWITCH_DWELL_MS);

  // Step 2: Sta -> ApSta. STA association should persist; AP should
  // come up alongside (we have to start_ap() ourselves — set_mode does
  // not implicitly start AP).
  _log_step("Sta -> ApSta", mgr);
  if (mgr.set_mode(WifiMode::ApSta) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(ApSta) failed");
  } else if (mgr.start_ap(_build_ap_config()) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "start_ap in ApSta failed");
  }
  RTOS::delay_ms(TEST_WIFI_MODE_SWITCH_DWELL_MS);
  _log_snapshot(mgr.status_snapshot());

  // Step 3: ApSta -> Ap. STA should tear down; on_disconnected should
  // fire with RequestedByUser (manager-initiated); mDNS auto-stops.
  _log_step("ApSta -> Ap", mgr);
  if (mgr.set_mode(WifiMode::Ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Ap) failed");
  }
  RTOS::delay_ms(TEST_WIFI_MODE_SWITCH_DWELL_MS);
  _log_snapshot(mgr.status_snapshot());

  // Step 4: Ap -> Off. Full radio teardown.
  _log_step("Ap -> Off", mgr);
  if (mgr.set_mode(WifiMode::Off) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Off) failed");
  }
  RTOS::delay_ms(TEST_WIFI_MODE_SWITCH_DWELL_MS);
  _log_snapshot(mgr.status_snapshot());

  // Step 5: Off -> Sta + reconnect. Verifies full re-init from cold.
  // Static IP (if configured) is still latched in the HAL and should
  // be reapplied on the new association.
  _log_step("Off -> Sta + reconnect", mgr);
  if (mgr.set_mode(WifiMode::Sta) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Sta) failed");
  } else {
    const WifiStatus rc = mgr.connect(cfg);
    if (rc != WifiStatus::Ok) {
      ESP_LOGE(TAG, "reconnect() returned %d", static_cast<int>(rc));
    }
  }
  RTOS::delay_ms(TEST_WIFI_MODE_SWITCH_DWELL_MS);
  _log_snapshot(mgr.status_snapshot());

  ESP_LOGI(TAG, "=== mode-switch sweep complete, idling ===");
#endif

  while (true) {
    RTOS::delay_ms(TEST_WIFI_STATUS_INTERVAL_MS);
    _log_snapshot(mgr.status_snapshot());
  }
}

#else // TEST_WIFI_RUN_AP

void _run_ap_test(WifiManager &mgr) {
  static constexpr const char *kSsid = TEST_WIFI_AP_SSID;
  static constexpr const char *kPassword = TEST_WIFI_AP_PASSWORD;

  if (mgr.set_mode(WifiMode::Ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Ap) failed");
    return;
  }

  WifiApConfig ap = {};
  std::strncpy(ap.ssid, kSsid, sizeof(ap.ssid) - 1);
  std::strncpy(ap.password, kPassword, sizeof(ap.password) - 1);
  ap.channel = 1;
  ap.max_connections = 4;

  ESP_LOGI(TAG, "starting AP '%s' (auth=%s)", kSsid, kPassword[0] == '\0' ? "OPEN" : "WPA2-PSK");
  if (mgr.start_ap(ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "start_ap failed");
    return;
  }

  while (true) {
    RTOS::delay_ms(TEST_WIFI_STATUS_INTERVAL_MS);
    _log_snapshot(mgr.status_snapshot());
  }
}

#endif // TEST_WIFI_RUN_AP

} // namespace

void run_test_wifi() {
  ESP_LOGI(TAG, "--- Wi-Fi test start (runs indefinitely) ---");

  EspWifiHal hal;
  WifiManager mgr(hal);

  if (hal.init() != WifiStatus::Ok) {
    ESP_LOGE(TAG, "EspWifiHal::init failed");
    return;
  }

  mgr.set_on_connected([]() { ESP_LOGI(TAG, "event: connected (L2 link up)"); });
  mgr.set_on_got_ip([](uint32_t ip) {
    _log_ip(ip);
#ifdef TEST_WIFI_STATIC_IP
    if (_expected_static_ip != 0) {
      if (ip == _expected_static_ip) {
        ESP_LOGI(TAG, "STATIC IP CHECK: PASS (got IP matches configured)");
      } else {
        ESP_LOGE(TAG, "STATIC IP CHECK: FAIL (got 0x%08" PRIX32 ", expected 0x%08" PRIX32 ")", ip,
                 _expected_static_ip);
      }
    }
#endif
  });
  mgr.set_on_disconnected([](WifiDisconnectReason r) {
    ESP_LOGW(TAG, "event: disconnected reason=%s", _reason_to_str(r));
  });
  mgr.set_on_scan_complete([](const WifiScanEntry *results, uint16_t count) {
    ESP_LOGI(TAG, "scan complete: %u network%s", count, count == 1 ? "" : "s");
    for (uint16_t i = 0; i < count; ++i) {
      const WifiScanEntry &e = results[i];
      ESP_LOGI(TAG,
               "  [%u] ssid='%s' rssi=%d ch=%u auth=%s "
               "bssid=%02X:%02X:%02X:%02X:%02X:%02X",
               i, e.ssid, e.rssi, e.channel, _auth_to_str(e.auth_mode), e.bssid[0], e.bssid[1],
               e.bssid[2], e.bssid[3], e.bssid[4], e.bssid[5]);
    }
    _scan_done = true;
  });
  mgr.set_on_ap_client_joined([](const uint8_t mac[6]) {
    ESP_LOGI(TAG, "AP client joined: %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  });
  mgr.set_on_ap_client_left([](const uint8_t mac[6]) {
    ESP_LOGI(TAG, "AP client left:   %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  });

#ifndef TEST_WIFI_RUN_AP
  _run_sta_test(mgr);
#else
  _run_ap_test(mgr);
#endif
}
