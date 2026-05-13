/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef DRIVERS_IDF_HTTP_REQUEST_H
#define DRIVERS_IDF_HTTP_REQUEST_H

#include <cstddef>
#include <string>

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <sdkconfig.h>

#include "hal/http_request.h"
#include "types/http_types.h"

// Adapter that wraps an esp_http_server httpd_req_t* into the abstract
// HttpRequest interface consumed by HTTP handlers.
//
// Lifetime: constructed on the stack inside the driver's trampoline for
// the duration of a single request; never owned or named by user code.
// This header lives under drivers/ on purpose — consumers should depend on
// hal/http_request.h, not on this adapter.
//
// Body buffering is lazy and one-shot: the body is read on first access
// to body() / body_length() and cached. Reads beyond
// CONFIG_AG_HTTP_MAX_BODY_SIZE are truncated with a warning log.
//
// Header and query-parameter lookups are case-sensitive because the
// underlying esp_http_server accessors are case-sensitive.
class IdfHttpRequest : public HttpRequest {
public:
  IdfHttpRequest(httpd_req_t *req, HttpMethod method) : _req(req), _method(method) {}

  HttpMethod method() const override { return _method; }
  const char *uri() const override { return _req->uri; }

  const char *body() const override {
    _ensure_body();
    return _body.empty() ? nullptr : _body.c_str();
  }

  size_t body_length() const override {
    _ensure_body();
    return _body.size();
  }

  bool get_header(const char *name, char *buf, size_t buf_len) const override {
    if (name == nullptr || buf == nullptr || buf_len == 0) {
      return false;
    }
    const size_t value_len = httpd_req_get_hdr_value_len(_req, name);
    if (value_len == 0) {
      return false;
    }
    // httpd_req_get_hdr_value_str expects buf_len that includes the
    // NUL terminator and returns ESP_OK when the buffer is large enough.
    if (httpd_req_get_hdr_value_str(_req, name, buf, buf_len) != ESP_OK) {
      // Buffer too small — truncate gracefully.
      buf[buf_len - 1] = '\0';
      return false;
    }
    return true;
  }

  bool get_query_param(const char *key, char *buf, size_t buf_len) const override {
    if (key == nullptr || buf == nullptr || buf_len == 0) {
      return false;
    }
    const size_t qs_len = httpd_req_get_url_query_len(_req);
    if (qs_len == 0) {
      return false;
    }
    std::string qs;
    qs.resize(qs_len + 1);
    if (httpd_req_get_url_query_str(_req, qs.data(), qs.size()) != ESP_OK) {
      return false;
    }
    if (httpd_query_key_value(qs.c_str(), key, buf, buf_len) != ESP_OK) {
      return false;
    }
    return true;
  }

private:
  static constexpr const char *_TAG = "IdfHttpRequest";

  void _ensure_body() const {
    if (_body_loaded) {
      return;
    }
    _body_loaded = true;
    const size_t total = _req->content_len;
    if (total == 0) {
      return;
    }
    const size_t cap = static_cast<size_t>(CONFIG_AG_HTTP_MAX_BODY_SIZE);
    if (total > cap) {
      ESP_LOGW(_TAG, "request body %u bytes exceeds cap %u, truncating",
               static_cast<unsigned>(total), static_cast<unsigned>(cap));
    }
    const size_t to_read = total > cap ? cap : total;
    _body.resize(to_read);
    size_t off = 0;
    while (off < to_read) {
      const int r = httpd_req_recv(_req, _body.data() + off, to_read - off);
      if (r <= 0) {
        // Read failure or socket closed; keep whatever was buffered.
        _body.resize(off);
        return;
      }
      off += static_cast<size_t>(r);
    }
  }

  httpd_req_t *_req;
  HttpMethod _method;
  mutable std::string _body;
  mutable bool _body_loaded = false;
};

#endif // DRIVERS_IDF_HTTP_REQUEST_H
