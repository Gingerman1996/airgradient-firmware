/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TESTS_TEST_HTTP_REQUEST_H
#define TESTS_TEST_HTTP_REQUEST_H

#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>

#include "hal/http_request.h"
#include "types/http_types.h"

// Canned HttpRequest implementation for host tests. Lives in the
// component's tests/ directory and is reused by consumer components and
// products through the test support library.
class TestHttpRequest : public HttpRequest {
public:
  TestHttpRequest(HttpMethod method, const char *uri)
      : _method(method), _uri(uri == nullptr ? "" : uri) {}

  void set_body(const char *data, size_t len) {
    if (data == nullptr) {
      _body.clear();
    } else {
      _body.assign(data, len);
    }
  }

  void set_body(const std::string &data) { _body = data; }

  void set_header(const char *name, const char *value) {
    if (name != nullptr && value != nullptr) {
      _headers[name] = value;
    }
  }

  void set_query_param(const char *key, const char *value) {
    if (key != nullptr && value != nullptr) {
      _params[key] = value;
    }
  }

  HttpMethod method() const override { return _method; }
  const char *uri() const override { return _uri.c_str(); }
  const char *body() const override { return _body.empty() ? nullptr : _body.c_str(); }
  size_t body_length() const override { return _body.size(); }

  bool get_header(const char *name, char *buf, size_t buf_len) const override {
    return _lookup(_headers, name, buf, buf_len);
  }

  bool get_query_param(const char *key, char *buf, size_t buf_len) const override {
    return _lookup(_params, key, buf, buf_len);
  }

private:
  static bool _lookup(const std::unordered_map<std::string, std::string> &map, const char *key,
                      char *buf, size_t buf_len) {
    if (key == nullptr || buf == nullptr || buf_len == 0) {
      return false;
    }
    const auto it = map.find(key);
    if (it == map.end()) {
      return false;
    }
    std::strncpy(buf, it->second.c_str(), buf_len);
    buf[buf_len - 1] = '\0';
    return true;
  }

  HttpMethod _method;
  std::string _uri;
  std::string _body;
  std::unordered_map<std::string, std::string> _headers;
  std::unordered_map<std::string, std::string> _params;
};

#endif // TESTS_TEST_HTTP_REQUEST_H
