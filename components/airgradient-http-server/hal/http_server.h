/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef HAL_HTTP_SERVER_H
#define HAL_HTTP_SERVER_H

#include <cstddef>
#include <cstdint>

#include "hal/http_request.h"
#include "hal/http_response.h"
#include "types/http_types.h"

// Abstract HTTP server. The concrete IdfHttpServer wraps esp_http_server;
// host tests can substitute a stub if they exercise wiring logic.
class HttpServer {
public:
  virtual ~HttpServer() = default;

  // Bind and start listening on the given port. Must be called after all
  // routes have been registered. Returns false on bind/start failure.
  // ISR-safe: no | Thread-safe: no | Blocking: yes
  virtual bool start(uint16_t port) = 0;

  // Stop the server. Safe to call when not started.
  // ISR-safe: no | Thread-safe: no | Blocking: yes
  virtual void stop() = 0;

  // Register a handler for an exact method + path combination. Must be
  // called before start(). Returns false if registration fails (out of
  // route slots, duplicate, etc.).
  // ISR-safe: no | Thread-safe: no | Blocking: no | Allocates: yes
  virtual bool register_route(HttpMethod method, const char *path, HttpHandler handler) = 0;

  // Convenience: register a GET handler that serves a flash-embedded
  // asset. data_start / data_end come from ESP-IDF EMBED_FILES linker
  // symbols (or any data with static lifetime).
  bool register_static(const char *uri_path, const uint8_t *data_start, const uint8_t *data_end,
                       const char *content_type) {
    if (data_start == nullptr || data_end == nullptr || data_end < data_start) {
      return false;
    }
    const size_t len = static_cast<size_t>(data_end - data_start);
    return register_route(HttpMethod::Get, uri_path,
                          [data_start, len, content_type](const HttpRequest &, HttpResponse &resp) {
                            resp.body_static(HttpStatus::Ok, data_start, len, content_type);
                          });
  }
};

#endif // HAL_HTTP_SERVER_H
