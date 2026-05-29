/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_http_client.h"

#include <cstring>

#include "esp_http_client.h"

#include "ag_log.h"

namespace {
constexpr const char *TAG = "WifiHttpClient";

struct ResponseCapture {
  char *buf;
  size_t cap;     // includes space for trailing NUL
  size_t written; // excludes NUL
  bool truncated;
};

esp_err_t http_event_capture(esp_http_client_event_t *evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA) {
    return ESP_OK;
  }
  auto *cap = static_cast<ResponseCapture *>(evt->user_data);
  if (cap == nullptr || cap->buf == nullptr || cap->cap == 0) {
    return ESP_OK;
  }

  // Reserve one byte for trailing NUL.
  const size_t writable = (cap->cap > 0) ? (cap->cap - 1) : 0;
  const size_t remaining = (cap->written < writable) ? (writable - cap->written) : 0;
  const size_t incoming = static_cast<size_t>(evt->data_len);

  if (incoming > remaining) {
    cap->truncated = true;
  }

  const size_t to_copy = (incoming < remaining) ? incoming : remaining;
  if (to_copy > 0) {
    std::memcpy(cap->buf + cap->written, evt->data, to_copy);
    cap->written += to_copy;
  }
  return ESP_OK;
}

} // namespace

bool WifiHttpClient::get(const char *url, const char *cert_pem, int &status_code,
                         char *response_body, size_t body_size, size_t *bytes_written,
                         bool *truncated) {
  if (bytes_written != nullptr) {
    *bytes_written = 0;
  }
  if (truncated != nullptr) {
    *truncated = false;
  }
  if (url == nullptr || response_body == nullptr || body_size == 0) {
    AG_LOGE(TAG, "get: invalid arguments");
    return false;
  }

  ResponseCapture cap{response_body, body_size, 0, false};

  esp_http_client_config_t config = {};
  config.url = url;
  config.method = HTTP_METHOD_GET;
  config.cert_pem = cert_pem;
  config.timeout_ms = DEFAULT_TIMEOUT_MS;
  config.event_handler = http_event_capture;
  config.user_data = &cap;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    AG_LOGE(TAG, "get: esp_http_client_init failed");
    return false;
  }

  const esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    AG_LOGE(TAG, "get: esp_http_client_perform failed: %d", err);
    esp_http_client_cleanup(client);
    return false;
  }

  status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  // NUL-terminate whatever made it in (capacity is guaranteed >= 1).
  response_body[cap.written] = '\0';

  if (bytes_written != nullptr) {
    *bytes_written = cap.written;
  }
  if (truncated != nullptr) {
    *truncated = cap.truncated;
  }
  return true;
}

bool WifiHttpClient::post(const char *url, const char *cert_pem, const char *content_type,
                          const uint8_t *body, size_t body_len, int &status_code) {
  if (url == nullptr || body == nullptr) {
    AG_LOGE(TAG, "post: invalid arguments");
    return false;
  }

  esp_http_client_config_t config = {};
  config.url = url;
  config.method = HTTP_METHOD_POST;
  config.cert_pem = cert_pem;
  config.timeout_ms = DEFAULT_TIMEOUT_MS;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    AG_LOGE(TAG, "post: esp_http_client_init failed");
    return false;
  }

  esp_http_client_set_header(client, "Content-Type",
                             content_type != nullptr ? content_type : "application/json");
  esp_http_client_set_post_field(client, reinterpret_cast<const char *>(body),
                                 static_cast<int>(body_len));

  const esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    AG_LOGE(TAG, "post: esp_http_client_perform failed: %d", err);
    esp_http_client_cleanup(client);
    return false;
  }

  status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return true;
}
