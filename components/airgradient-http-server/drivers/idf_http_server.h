/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef DRIVERS_IDF_HTTP_SERVER_H
#define DRIVERS_IDF_HTTP_SERVER_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <esp_http_server.h>

#include "hal/http_server.h"
#include "types/http_types.h"

// esp_http_server-backed implementation of HttpServer. Owns the httpd
// handle and the route table. All routes must be registered before
// calling start(); subsequent register_route() calls after start() will
// fail.
class IdfHttpServer : public HttpServer {
public:
  IdfHttpServer();
  ~IdfHttpServer() override;

  IdfHttpServer(const IdfHttpServer &) = delete;
  IdfHttpServer &operator=(const IdfHttpServer &) = delete;

  bool start(uint16_t port) override;
  void stop() override;
  bool register_route(HttpMethod method, const char *path, HttpHandler handler) override;

private:
  struct Route {
    HttpMethod method;
    std::string path;
    HttpHandler handler;
  };

  static esp_err_t _trampoline(httpd_req_t *req);

  // Route storage uses unique_ptr so user_ctx pointers stay valid even
  // when the underlying vector reallocates.
  std::vector<std::unique_ptr<Route>> _routes;
  httpd_handle_t _handle = nullptr;
  bool _started = false;
};

#endif // DRIVERS_IDF_HTTP_SERVER_H
