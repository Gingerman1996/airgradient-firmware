#include "test_provisioning.h"

#include <cinttypes>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/esp_wifi_hal.h"
#include "drivers/idf_http_server.h"
#include "drivers/nimble_ble_server.h"
#include "rtos.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"
#include "types/provisioning_types.h"

// ---------------------------------------------------------------------------
// Build-time configuration. Override with -D in EXTRA_CXXFLAGS if needed.
// ---------------------------------------------------------------------------

#ifndef TEST_PROVISIONING_AP_SSID
#define TEST_PROVISIONING_AP_SSID "airgradient-prov"
#endif

// Empty string = open AP. WPA2-PSK requires >= 8 characters.
#ifndef TEST_PROVISIONING_AP_PASSWORD
#define TEST_PROVISIONING_AP_PASSWORD ""
#endif

#ifndef TEST_PROVISIONING_AP_CHANNEL
#define TEST_PROVISIONING_AP_CHANNEL 1
#endif

#ifndef TEST_PROVISIONING_AP_MAX_CLIENTS
#define TEST_PROVISIONING_AP_MAX_CLIENTS 4
#endif

// 0 disables the inactivity timeout. Defaults to 5 minutes so the smoke
// test eventually self-terminates if nobody connects.
#ifndef TEST_PROVISIONING_OVERALL_TIMEOUT_MS
#define TEST_PROVISIONING_OVERALL_TIMEOUT_MS 300000U
#endif

namespace {

constexpr const char *TAG = "test_provisioning";

const char *event_name(ProvisioningEvent e) {
  switch (e) {
  case ProvisioningEvent::Started:
    return "Started";
  case ProvisioningEvent::Connecting:
    return "Connecting";
  case ProvisioningEvent::ConnectFailed:
    return "ConnectFailed";
  case ProvisioningEvent::Connected:
    return "Connected";
  case ProvisioningEvent::Stopped:
    return "Stopped";
  }
  return "?";
}

const char *stop_reason_name(ProvisioningStopReason r) {
  switch (r) {
  case ProvisioningStopReason::ProductRequested:
    return "ProductRequested";
  case ProvisioningStopReason::TimedOut:
    return "TimedOut";
  }
  return "?";
}

// Shared latch used by the main task loop to react to lifecycle events.
// `volatile sig_atomic_t` would be more correct, but we only read these
// from the product task; the provisioning manager guarantees callbacks
// run on its dispatch task and updates here are simple word-sized
// writes that are atomic on Xtensa / RISC-V.
struct EventLatch {
  volatile bool got_connected = false;
  volatile bool got_stopped = false;
  volatile uint32_t connected_ip = 0;
};

} // namespace

void run_test_provisioning() {
  ESP_LOGI(TAG, "--- Provisioning smoke test start ---");

  EspWifiHal wifi_hal;
  WifiManager wifi(wifi_hal);
  if (wifi_hal.init() != WifiStatus::Ok) {
    ESP_LOGE(TAG, "EspWifiHal::init failed; aborting");
    return;
  }

  IdfHttpServer http;
  NimbleBleServer ble;

  // HTTP server must be started AFTER routes are registered. The
  // provisioning manager registers its routes inside start(); we start
  // the server immediately after. The BLE server is initialised by
  // ProvisioningManager::start() and deinitialised by stop().
  ProvisioningManager prov;
  EventLatch latch;
  prov.set_on_event([&latch](const ProvisioningEventInfo &info) {
    switch (info.event) {
    case ProvisioningEvent::Started:
      ESP_LOGI(TAG, "event=Started — portal is live");
      break;
    case ProvisioningEvent::Connecting:
      ESP_LOGI(TAG, "event=Connecting ssid='%s' static_ip=%s disable_cloud=%d", info.data.ssid,
               info.data.has_static_ip() ? "yes" : "no", static_cast<int>(info.data.disable_cloud));
      break;
    case ProvisioningEvent::ConnectFailed:
      ESP_LOGW(TAG, "event=ConnectFailed — staying in WaitingForCredentials");
      break;
    case ProvisioningEvent::Connected:
      ESP_LOGI(TAG, "event=Connected ip=%u.%u.%u.%u", static_cast<unsigned>(info.ip & 0xff),
               static_cast<unsigned>((info.ip >> 8) & 0xff),
               static_cast<unsigned>((info.ip >> 16) & 0xff),
               static_cast<unsigned>((info.ip >> 24) & 0xff));
      latch.connected_ip = info.ip;
      latch.got_connected = true;
      break;
    case ProvisioningEvent::Stopped:
      ESP_LOGI(TAG, "event=Stopped reason=%s", stop_reason_name(info.stop_reason));
      latch.got_stopped = true;
      break;
    default:
      ESP_LOGW(TAG, "event=%s (unhandled)", event_name(info.event));
      break;
    }
  });

  ProvisioningConfig cfg = {};
  // Default is BleOnly; Reference opts in to dual-transport.
  cfg.transport = ProvisioningTransport::Both;
  std::strncpy(cfg.ap.ssid, TEST_PROVISIONING_AP_SSID, sizeof(cfg.ap.ssid) - 1);
  std::strncpy(cfg.ap.password, TEST_PROVISIONING_AP_PASSWORD, sizeof(cfg.ap.password) - 1);
  cfg.ap.channel = TEST_PROVISIONING_AP_CHANNEL;
  cfg.ap.max_clients = TEST_PROVISIONING_AP_MAX_CLIENTS;
  cfg.overall_timeout_ms = TEST_PROVISIONING_OVERALL_TIMEOUT_MS;

  // ProvisioningManager owns both the HTTP and BLE server lifecycles
  // for the duration of the session — start() registers portal routes,
  // starts the HTTP server, initialises BLE, and begins advertising.
  // stop() tears everything down.
  cfg.ble.device_name = "AirGradient";
  cfg.ble.model_name = "Reference";
  cfg.ble.serial_number = "000000";
  cfg.ble.firmware_version = "0.0.0";
  if (!prov.start(wifi, ble, http, cfg)) {
    ESP_LOGE(TAG, "ProvisioningManager::start failed");
    return;
  }

  ESP_LOGI(TAG, "provisioning active (portal + BLE):");
  ESP_LOGI(TAG, "  AP SSID:  \"%s\"", TEST_PROVISIONING_AP_SSID);
  ESP_LOGI(TAG, "  password: \"%s\" (empty = open)", TEST_PROVISIONING_AP_PASSWORD);
  ESP_LOGI(TAG, "  portal:   http://192.168.4.1/");
  ESP_LOGI(TAG, "  BLE:      advertising as \"%s\"", cfg.ble.device_name);
  ESP_LOGI(TAG, "  timeout:  %u ms (0 = disabled)",
           static_cast<unsigned>(TEST_PROVISIONING_OVERALL_TIMEOUT_MS));

  // Main loop — react to lifecycle events. The provisioning manager
  // dispatches events from its own task contexts; we only observe
  // latched flags here. On Connected we exercise send_ble_status()
  // and call stop() so the full lifecycle is covered by the smoke test.
  bool post_connect_done = false;
  while (true) {
    if (latch.got_connected && !post_connect_done) {
      post_connect_done = true;
      ESP_LOGI(TAG, "post-connect: pushing application BLE status codes");
      prov.send_ble_status(ProvisioningBleStatus::CONNECTING_TO_SERVER);
      RTOS::delay_ms(200);
      prov.send_ble_status(ProvisioningBleStatus::SERVER_REACHABLE);
      RTOS::delay_ms(200);
      ESP_LOGI(TAG, "post-connect: stopping provisioning (AP down, HTTP stopped, BLE off)");
      prov.stop();
    }
    if (latch.got_stopped) {
      ESP_LOGI(TAG, "provisioning torn down — STA-only mode, idling");
      // Drop into a slow heartbeat loop so the device stays attached
      // for log inspection.
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "idle (state=%d)", static_cast<int>(prov.state()));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
