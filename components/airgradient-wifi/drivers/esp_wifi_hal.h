/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_ESP_WIFI_HAL_H
#define AG_ESP_WIFI_HAL_H

#include "../hal/wifi_hal.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"

/// ESP-IDF backed WifiHal implementation.
///
/// Wraps esp_wifi_*, esp_netif_*, mdns_*, esp_timer_*. Translates the raw
/// ESP-IDF event loop into typed callbacks.
///
/// All callbacks fire from the ESP-IDF system event loop task context.
class EspWifiHal : public WifiHal {
public:
  EspWifiHal();
  ~EspWifiHal() override;

  EspWifiHal(const EspWifiHal &) = delete;
  EspWifiHal &operator=(const EspWifiHal &) = delete;

  // -- WifiHal overrides --
  WifiStatus init() override;
  void deinit() override;

  WifiStatus set_mode(WifiMode mode) override;
  WifiMode get_mode() const override;

  WifiStatus connect_sta(const char *ssid, const char *password, bool persist = true) override;
  WifiStatus disconnect_sta() override;

  bool has_saved_credentials() const override;

  WifiStatus set_static_ip(const WifiStaticIpConfig &config) override;
  WifiStatus clear_static_ip() override;

  WifiStatus start_scan(const WifiScanConfig &config) override;

  WifiStatus start_ap(const WifiApConfig &config) override;
  WifiStatus stop_ap() override;

  WifiStatusSnapshot get_status() const override;

  WifiStatus set_power_save(WifiPowerSave mode) override;

  WifiStatus start_mdns(const WifiMdnsConfig &config) override;
  WifiStatus stop_mdns() override;

  WifiStatus clear_saved_credentials() override;

  WifiStatus arm_dhcp_timeout(uint32_t timeout_ms) override;
  WifiStatus cancel_dhcp_timeout() override;
  WifiStatus arm_retry_timer(uint32_t delay_ms) override;
  WifiStatus cancel_retry_timer() override;

  void set_on_sta_connected(WifiConnectedCallback cb) override;
  void set_on_sta_disconnected(std::function<void(int reason)> cb) override;
  void set_on_got_ip(WifiGotIpCallback cb) override;
  void set_on_scan_complete(WifiScanCompleteCallback cb) override;
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb) override;
  void set_on_ap_client_left(WifiApClientLeftCallback cb) override;
  void set_on_dhcp_timeout(std::function<void()> cb) override;
  void set_on_retry_due(std::function<void()> cb) override;

private:
  // WiFi IDF API Callbacks. Only bridge to own methods.
  static void _wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
  static void _ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);
  static void _dhcp_timer_cb(void *arg);
  static void _retry_timer_cb(void *arg);

  void _handle_wifi_event(int32_t id, void *data);
  void _handle_ip_event(int32_t id, void *data);
  void _apply_static_ip_to_netif();
  void _apply_dhcp_to_netif();

  esp_netif_t *_sta_netif = nullptr;
  esp_netif_t *_ap_netif = nullptr;
  esp_event_handler_instance_t _wifi_handler_instance = nullptr;
  esp_event_handler_instance_t _ip_handler_instance = nullptr;
  esp_timer_handle_t _dhcp_timer = nullptr;
  esp_timer_handle_t _retry_timer = nullptr;

  bool _initialized = false;
  bool _mdns_started = false;
  WifiMode _mode = WifiMode::Off;
  bool _has_static_ip = false;
  WifiStaticIpConfig _static_ip = {};
  WifiStatusSnapshot _snapshot = {};

  WifiConnectedCallback _on_sta_connected;
  std::function<void(int)> _on_sta_disconnected;
  WifiGotIpCallback _on_got_ip;
  WifiScanCompleteCallback _on_scan_complete;
  WifiApClientJoinedCallback _on_ap_client_joined;
  WifiApClientLeftCallback _on_ap_client_left;
  std::function<void()> _on_dhcp_timeout;
  std::function<void()> _on_retry_due;
};

#endif // AG_ESP_WIFI_HAL_H
