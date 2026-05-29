#include "test_http_server.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "drivers/esp_wifi_hal.h"
#include "drivers/idf_http_server.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "services/wifi_manager.h"
#include "types/http_types.h"
#include "types/wifi_types.h"

// Embedded index.html shipped with this product (see main/CMakeLists.txt
// EMBED_FILES).  The two symbols below are produced by the linker.
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

static constexpr const char *TAG = "test_http_server";

namespace {

constexpr const char *AP_SSID = "airgradient-ref";
constexpr const char *AP_PASSWORD = "airgradient";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNS = 4;

bool init_softap(WifiManager &mgr) {
  mgr.set_on_ap_client_joined([](const uint8_t mac[6]) {
    ESP_LOGI(TAG, "station %02X:%02X:%02X:%02X:%02X:%02X joined", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  });
  mgr.set_on_ap_client_left([](const uint8_t mac[6]) {
    ESP_LOGI(TAG, "station %02X:%02X:%02X:%02X:%02X:%02X left", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  });

  if (mgr.set_mode(WifiMode::Ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "set_mode(Ap) failed");
    return false;
  }

  WifiApConfig ap = {};
  std::strncpy(ap.ssid, AP_SSID, sizeof(ap.ssid) - 1);
  std::strncpy(ap.password, AP_PASSWORD, sizeof(ap.password) - 1);
  ap.channel = AP_CHANNEL;
  ap.max_connections = AP_MAX_CONNS;
  if (mgr.start_ap(ap) != WifiStatus::Ok) {
    ESP_LOGE(TAG, "start_ap failed");
    return false;
  }

  ESP_LOGI(TAG, "SoftAP up: ssid=\"%s\" channel=%u pw=\"%s\"", AP_SSID,
           static_cast<unsigned>(AP_CHANNEL), AP_PASSWORD);
  ESP_LOGI(TAG, "join the AP, then browse to http://192.168.4.1/");
  return true;
}

// --- Handlers ------------------------------------------------------------

void handle_status(const HttpRequest &, HttpResponse &resp) {
  const int64_t uptime_us = esp_timer_get_time();
  char buf[96];
  const int n =
      std::snprintf(buf, sizeof(buf), R"({"uptime_ms":%lld,"ssid":"%s","build":"reference"})",
                    static_cast<long long>(uptime_us / 1000), AP_SSID);
  if (n <= 0) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"snprintf"})");
    return;
  }
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}

void handle_echo(const HttpRequest &req, HttpResponse &resp) {
  if (req.body() == nullptr || req.body_length() == 0) {
    resp.json(HttpStatus::BadRequest, R"({"error":"empty body"})");
    return;
  }
  // Content type is whatever the client sent; we just mirror the bytes
  // as application/octet-stream to keep the demo simple.
  resp.body(HttpStatus::Ok, req.body(), req.body_length(), "application/octet-stream");
}

void handle_greet(const HttpRequest &req, HttpResponse &resp) {
  char name[32] = {};
  if (!req.get_query_param("name", name, sizeof(name))) {
    resp.json(HttpStatus::BadRequest, R"({"error":"missing name"})");
    return;
  }
  char buf[96];
  const int n = std::snprintf(buf, sizeof(buf), R"({"hello":"%s"})", name);
  if (n <= 0) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"snprintf"})");
    return;
  }
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}

} // namespace

void run_test_http_server() {
  ESP_LOGI(TAG, "--- HTTP server test start ---");

  // Construct the wifi stack via airgradient-wifi. These outlive the
  // function trivially because run_test_http_server never returns.
  EspWifiHal wifi_hal;
  WifiManager wifi(wifi_hal);
  if (wifi_hal.init() != WifiStatus::Ok) {
    ESP_LOGE(TAG, "EspWifiHal::init failed; aborting test");
    return;
  }

  if (!init_softap(wifi)) {
    ESP_LOGE(TAG, "SoftAP init failed; aborting test");
    return;
  }

  // Construct on the heap so it lives for the whole program. The driver
  // tears down the httpd in its destructor; we never call it because the
  // test loops forever.
  auto *server = new IdfHttpServer();

  // --- Pre-start routes: static assets and status -------------------------
  bool ok = true;
  ok &= server->register_route(HttpMethod::Get, "/api/status", handle_status);
  ok &= server->register_static("/", index_html_start, index_html_end, "text/html");
  ok &= server->register_static("/index.html", index_html_start, index_html_end, "text/html");

  if (!ok) {
    ESP_LOGE(TAG, "pre-start route registration failed");
    return;
  }

  if (!server->start(CONFIG_AG_HTTP_PORT)) {
    ESP_LOGE(TAG, "server start failed");
    return;
  }

  ESP_LOGI(TAG, "HTTP server listening on port %d", CONFIG_AG_HTTP_PORT);

  // --- Post-start routes: added to the running server --------------------
  ok = true;
  ok &= server->register_route(HttpMethod::Post, "/api/echo", handle_echo);
  ok &= server->register_route(HttpMethod::Get, "/api/greet", handle_greet);

  if (!ok) {
    ESP_LOGE(TAG, "post-start route registration failed");
    return;
  }

  ESP_LOGI(TAG, "dynamic routes registered on running server");

  // Keep this task alive so the server stays up.
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "still serving (uptime %lld ms)",
             static_cast<long long>(esp_timer_get_time() / 1000));
  }
}
