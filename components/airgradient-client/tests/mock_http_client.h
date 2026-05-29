/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_MOCK_HTTP_CLIENT_H
#define AG_MOCK_HTTP_CLIENT_H

#include <cstring>
#include <string>
#include <vector>

#include "clients/http_client.h"

// Hand-rolled mock; two methods, simpler than trompeloeil here.
class MockHttpClient : public HttpClient {
public:
  // Programmable.
  bool next_transport_ok = true;
  int next_status = 200;
  std::string next_get_body;

  // Recorded.
  std::string last_url;
  std::string last_content_type;
  std::vector<uint8_t> last_post_body;
  int get_call_count = 0;
  int post_call_count = 0;

  bool get(const char *url, const char * /*cert_pem*/, int &status_code, char *response_body,
           size_t body_size, size_t *bytes_written, bool *truncated) override {
    ++get_call_count;
    last_url = url != nullptr ? url : "";
    if (bytes_written != nullptr) {
      *bytes_written = 0;
    }
    if (truncated != nullptr) {
      *truncated = false;
    }
    if (!next_transport_ok) {
      return false;
    }
    status_code = next_status;

    const size_t writable = (body_size > 0) ? (body_size - 1) : 0;
    const size_t available = next_get_body.size();
    const size_t to_copy = (available < writable) ? available : writable;
    if (response_body != nullptr && body_size > 0) {
      std::memcpy(response_body, next_get_body.data(), to_copy);
      response_body[to_copy] = '\0';
    }
    if (bytes_written != nullptr) {
      *bytes_written = to_copy;
    }
    if (truncated != nullptr) {
      *truncated = (available > writable);
    }
    return true;
  }

  bool post(const char *url, const char * /*cert_pem*/, const char *content_type,
            const uint8_t *body, size_t body_len, int &status_code) override {
    ++post_call_count;
    last_url = url != nullptr ? url : "";
    last_content_type = content_type != nullptr ? content_type : "";
    last_post_body.assign(body, body + body_len);
    if (!next_transport_ok) {
      return false;
    }
    status_code = next_status;
    return true;
  }
};

#endif // AG_MOCK_HTTP_CLIENT_H
