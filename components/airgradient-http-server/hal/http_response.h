/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef HAL_HTTP_RESPONSE_H
#define HAL_HTTP_RESPONSE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "types/http_types.h"

struct HttpHeader {
  const char *name = nullptr;
  const char *value = nullptr;
};

// Concrete response value type. The handler populates this; the driver
// reads it after the handler returns. Two body modes are supported:
//
//   Owning   (json / body / no_content) — copies into _owned_body, safe
//                                          for stack-local handler buffers.
//   Borrowing (body_static)             — stores a raw pointer, zero copy.
//                                          Caller must guarantee the data
//                                          has static lifetime (e.g. flash-
//                                          embedded EMBED_FILES symbols).
//
// Default status is 500 Internal Server Error: if a handler forgets to
// populate the response the result is an error, not an accidental 200 OK.
struct HttpResponse {
  // --- Readable state (inspected by tests and the driver) ---

  HttpStatus status = HttpStatus::InternalServerError;
  const char *content_type = nullptr;

  static constexpr size_t MAX_HEADERS = 4;
  HttpHeader headers[MAX_HEADERS] = {};
  size_t header_count = 0;

  // --- Owning setters (copy data — safe for stack-local buffers) ---

  void json(HttpStatus s, const char *data, size_t len) {
    status = s;
    content_type = "application/json";
    _owned_body.assign(data, len);
    _static_body = nullptr;
    _body_len = len;
    _is_static = false;
  }

  void json(HttpStatus s, const char *data) {
    json(s, data, data == nullptr ? 0 : std::strlen(data));
  }

  void body(HttpStatus s, const void *data, size_t len, const char *ct) {
    status = s;
    content_type = ct;
    _owned_body.assign(static_cast<const char *>(data), len);
    _static_body = nullptr;
    _body_len = len;
    _is_static = false;
  }

  void no_content() {
    status = HttpStatus::NoContent;
    content_type = nullptr;
    _owned_body.clear();
    _static_body = nullptr;
    _body_len = 0;
    _is_static = false;
  }

  // --- Borrowing setter (zero-copy — data must outlive the response) ---

  void body_static(HttpStatus s, const void *data, size_t len, const char *ct) {
    status = s;
    content_type = ct;
    _static_body = data;
    _body_len = len;
    _is_static = true;
    _owned_body.clear();
  }

  // --- Accessors (used by the driver to read the body) ---

  const void *body_data() const {
    return _is_static ? _static_body : static_cast<const void *>(_owned_body.data());
  }

  size_t body_size() const { return _body_len; }

  bool is_body_static() const { return _is_static; }

  // --- Header helper. Silently ignored once MAX_HEADERS is reached. ---

  void set_header(const char *name, const char *value) {
    if (header_count < MAX_HEADERS) {
      headers[header_count] = {name, value};
      ++header_count;
    }
  }

private:
  std::string _owned_body;
  const void *_static_body = nullptr;
  size_t _body_len = 0;
  bool _is_static = false;
};

#endif // HAL_HTTP_RESPONSE_H
