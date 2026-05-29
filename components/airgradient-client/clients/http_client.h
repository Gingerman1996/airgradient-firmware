/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_HTTP_CLIENT_H
#define AG_HTTP_CLIENT_H

#include <cstddef>
#include <cstdint>

// HttpClient is consumed by AgClient.  Buffers are caller-owned; no
// retention.  Not ISR-safe, not thread-safe, blocks.
class HttpClient {
public:
  virtual ~HttpClient() = default;

  // Returns true if the exchange completed (any status), false on
  // transport failure.  Response is NUL-terminated; on overflow, writes
  // what fits and sets *truncated=true.  Invalid args (body_size==0 or
  // response_body==nullptr) return false with *bytes_written=0.
  virtual bool get(const char *url, const char *cert_pem, int &status_code, char *response_body,
                   size_t body_size, size_t *bytes_written, bool *truncated) = 0;

  // Returns true if the exchange completed (any status), false on
  // transport failure.
  virtual bool post(const char *url, const char *cert_pem, const char *content_type,
                    const uint8_t *body, size_t body_len, int &status_code) = 0;
};

#endif // AG_HTTP_CLIENT_H
