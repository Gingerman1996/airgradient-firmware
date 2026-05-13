/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef HAL_HTTP_REQUEST_H
#define HAL_HTTP_REQUEST_H

#include <cstddef>

#include "types/http_types.h"

// Abstract request view. Backed by httpd_req_t* at runtime and by
// TestHttpRequest in host tests. All accessors are allocation-free; the
// driver buffers the body up to CONFIG_AG_HTTP_MAX_BODY_SIZE before the
// handler runs.
class HttpRequest {
public:
  virtual ~HttpRequest() = default;

  virtual HttpMethod method() const = 0;
  virtual const char *uri() const = 0;

  // Body access. Returns nullptr for bodyless requests (GET, etc.).
  virtual const char *body() const = 0;
  virtual size_t body_length() const = 0;

  // Key-by-key header lookup. Writes value into buf (NUL-terminated) and
  // returns true on match.
  virtual bool get_header(const char *name, char *buf, size_t buf_len) const = 0;

  // Key-by-key query-parameter lookup. Writes value into buf
  // (NUL-terminated) and returns true on match.
  virtual bool get_query_param(const char *key, char *buf, size_t buf_len) const = 0;
};

#endif // HAL_HTTP_REQUEST_H
