/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_HTTP_CLIENT_H
#define AG_WIFI_HTTP_CLIENT_H

#include "../clients/http_client.h"

// esp_http_client wrapper.  Firmware build only; not host-testable.
class WifiHttpClient : public HttpClient {
public:
  WifiHttpClient() = default;
  ~WifiHttpClient() override = default;

  bool get(const char *url, const char *cert_pem, int &status_code, char *response_body,
           size_t body_size, size_t *bytes_written, bool *truncated) override;

  bool post(const char *url, const char *cert_pem, const char *content_type, const uint8_t *body,
            size_t body_len, int &status_code) override;

private:
  static constexpr int DEFAULT_TIMEOUT_MS = 15000;
};

#endif // AG_WIFI_HTTP_CLIENT_H
