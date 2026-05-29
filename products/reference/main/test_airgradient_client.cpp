#include "test_airgradient_client.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "measures_types.h"
#include "services/ag_client.h"
#include "types/client_types.h"

static constexpr const char *TAG = "test_ag_client";

// ---- Edit these before flashing --------------------------------------
// Do not commit real credentials or a real device serial number.
static constexpr const char *WIFI_SSID = "myssid";
static constexpr const char *WIFI_PASSWORD = "mypassword";
// 12-char hex; must be registered on HTTP_DOMAIN for the "happy" case.
static constexpr const char *SERIAL_NUMBER = "aabbccddeeff";
// Empty -> use compiled-in "hw.airgradient.com".
static constexpr const char *HTTP_DOMAIN = "";
// ----------------------------------------------------------------------

static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAILED_BIT = BIT1;
static constexpr int WIFI_MAX_RETRIES = 5;
static constexpr size_t CONFIG_BUFFER_SIZE = 2048;

namespace {

EventGroupHandle_t s_wifi_event_group = nullptr;
int s_retry_count = 0;

void on_wifi_event(void * /*arg*/, esp_event_base_t event_base, int32_t event_id,
                   void * /*event_data*/) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_count < WIFI_MAX_RETRIES) {
      ++s_retry_count;
      ESP_LOGW(TAG, "WiFi disconnect, retry %d/%d", s_retry_count, WIFI_MAX_RETRIES);
      esp_wifi_connect();
    } else {
      ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRIES);
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
    }
  }
}

void on_ip_event(void * /*arg*/, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_count = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

// Blocks until DHCP IP arrives or retry budget runs out.
bool connect_wifi(const char *ssid, const char *password) {
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == nullptr) {
    ESP_LOGE(TAG, "xEventGroupCreate failed");
    return false;
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t any_wifi = nullptr;
  esp_event_handler_instance_t got_ip = nullptr;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event,
                                                      nullptr, &any_wifi));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event,
                                                      nullptr, &got_ip));

  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid,
               sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), password,
               sizeof(wifi_config.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Connecting to SSID '%s' ...", ssid);
  const EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                          /*xClearOnExit=*/pdFALSE, /*xWaitForAllBits=*/pdFALSE, portMAX_DELAY);

  return (bits & WIFI_CONNECTED_BIT) != 0;
}

// All fields invalid except a handful of realistic readings.
Measures make_smoke_measures() {
  Measures m{};

  m.co2.co2 = MeasuresInvalid::CO2;
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.temp_hum_b.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_b.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_01 = MeasuresInvalid::PM;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.pm_a.pm_10 = MeasuresInvalid::PM;
  m.pm_a.pm_01_sp = MeasuresInvalid::PM;
  m.pm_a.pm_25_sp = MeasuresInvalid::PM;
  m.pm_a.pm_10_sp = MeasuresInvalid::PM;
  m.pm_a.pm_03_pc = MeasuresInvalid::PM;
  m.pm_a.pm_05_pc = MeasuresInvalid::PM;
  m.pm_a.pm_01_pc = MeasuresInvalid::PM;
  m.pm_a.pm_25_pc = MeasuresInvalid::PM;
  m.pm_a.pm_5_pc = MeasuresInvalid::PM;
  m.pm_a.pm_10_pc = MeasuresInvalid::PM;
  m.pm_b.pm_01 = MeasuresInvalid::PM;
  m.pm_b.pm_25 = MeasuresInvalid::PM;
  m.pm_b.pm_10 = MeasuresInvalid::PM;
  m.pm_b.pm_03_pc = MeasuresInvalid::PM;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  m.electrode.o3_we = MeasuresInvalid::VOLT;
  m.electrode.o3_ae = MeasuresInvalid::VOLT;
  m.electrode.no2_we = MeasuresInvalid::VOLT;
  m.electrode.no2_ae = MeasuresInvalid::VOLT;
  m.electrode.afe_temp = MeasuresInvalid::VOLT;
  // MeasuresPower defaults to invalid.

  // Realistic readings.
  m.co2.co2 = 450;
  m.temp_hum_a.temperature = 23.5f;
  m.temp_hum_a.humidity = 42.0f;

  return m;
}

// -127 when AP info is unavailable.
int read_wifi_signal() {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
    return ap.rssi;
  }
  return -127;
}

const char *result_to_str(AgClientResult r) {
  switch (r) {
  case AgClientResult::Ok:
    return "Ok";
  case AgClientResult::BufferTooSmall:
    return "BufferTooSmall";
  case AgClientResult::TransportError:
    return "TransportError";
  case AgClientResult::ServerError:
    return "ServerError";
  case AgClientResult::NotRegistered:
    return "NotRegistered";
  }
  return "?";
}

struct TestCase {
  const char *name;
  const char *serial_number;
  const char *domain_override; // "" -> keep file-level HTTP_DOMAIN
  AgClientResult expected_fetch;
  AgClientResult expected_post;
};

bool run_case(const TestCase &tc, int signal, const Measures &measures) {
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "==== Case: %s ====", tc.name);
  ESP_LOGI(TAG, "  sn='%s' domain_override='%s'", tc.serial_number, tc.domain_override);
  ESP_LOGI(TAG, "  expect fetch=%s post=%s", result_to_str(tc.expected_fetch),
           result_to_str(tc.expected_post));

  AgClient client;
  if (!client.begin(tc.serial_number, NetworkType::Wifi)) {
    ESP_LOGE(TAG, "  [FAIL] AgClient::begin");
    return false;
  }

  if (tc.domain_override[0] != '\0') {
    client.set_http_domain(tc.domain_override);
  } else if (HTTP_DOMAIN[0] != '\0') {
    client.set_http_domain(HTTP_DOMAIN);
  }

  bool ok = true;

  // ---- fetch_config -------------------------------------------------
  static char config_buf[CONFIG_BUFFER_SIZE];
  size_t config_written = 0;
  const AgClientResult fetch_result =
      client.http_fetch_config(config_buf, sizeof(config_buf), &config_written);
  const bool fetch_match = (fetch_result == tc.expected_fetch);
  ESP_LOGI(TAG, "  fetch_config: got=%s expect=%s bytes=%zu  %s", result_to_str(fetch_result),
           result_to_str(tc.expected_fetch), config_written, fetch_match ? "[PASS]" : "[FAIL]");
  if (fetch_result == AgClientResult::Ok && config_written > 0) {
    ESP_LOGI(TAG, "  fetch_config body: %s", config_buf);
  }
  ok &= fetch_match;

  // ---- post_measures ------------------------------------------------
  const AgClientResult post_result = client.http_post_measures(measures, signal);
  const bool post_match = (post_result == tc.expected_post);
  ESP_LOGI(TAG, "  post_measures: got=%s expect=%s  %s", result_to_str(post_result),
           result_to_str(tc.expected_post), post_match ? "[PASS]" : "[FAIL]");
  ok &= post_match;

  ESP_LOGI(TAG, "==== Case %s: %s ====", tc.name, ok ? "PASS" : "FAIL");
  return ok;
}

} // namespace

void run_test_airgradient_client() {
  ESP_LOGI(TAG, "--- AirGradient client smoke test start ---");
  ESP_LOGI(TAG, "Config: ssid='%s' sn='%s' default_domain_override='%s'", WIFI_SSID, SERIAL_NUMBER,
           HTTP_DOMAIN);

  if (!connect_wifi(WIFI_SSID, WIFI_PASSWORD)) {
    ESP_LOGE(TAG, "[FAIL] WiFi connect");
    return;
  }
  ESP_LOGI(TAG, "[PASS] WiFi connected");

  const Measures measures = make_smoke_measures();
  const int signal = read_wifi_signal();
  ESP_LOGI(TAG, "Signal for posts: %d", signal);

  // "happy" requires SERIAL_NUMBER registered on HTTP_DOMAIN.
  // "unregistered" uses an unknown SN -> 400 fetch / non-2xx post.
  // "wrong endpoint" uses .invalid TLD for guaranteed DNS failure.
  static const TestCase cases[] = {
      {
          "happy",
          SERIAL_NUMBER,
          "",
          AgClientResult::Ok,
          AgClientResult::Ok,
      },
      {
          "unregistered serial",
          "00000000000f",
          "",
          AgClientResult::NotRegistered,
          AgClientResult::ServerError,
      },
      {
          "wrong endpoint (DNS failure)",
          SERIAL_NUMBER,
          "no-such-host.airgradient-test.invalid",
          AgClientResult::TransportError,
          AgClientResult::TransportError,
      },
  };

  size_t passed = 0;
  for (const auto &tc : cases) {
    if (run_case(tc, signal, measures)) {
      ++passed;
    }
  }

  const size_t total = sizeof(cases) / sizeof(cases[0]);
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "--- Summary: %zu/%zu cases passed ---", passed, total);
  ESP_LOGI(TAG, "--- AirGradient client smoke test done ---");
}
