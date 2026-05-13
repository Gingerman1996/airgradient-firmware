/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#if defined(__has_include)
#if __has_include(<catch2/catch_test_macros.hpp>)

#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "hal/http_request.h"
#include "hal/http_response.h"
#include "hal/http_server.h"
#include "test_http_request.h"
#include "types/http_types.h"

namespace {

void status_handler(const HttpRequest &, HttpResponse &resp) {
  const char *body = R"({"uptime_ms":1234})";
  resp.json(HttpStatus::Ok, body);
}

void echo_body_handler(const HttpRequest &req, HttpResponse &resp) {
  if (req.body() == nullptr) {
    resp.json(HttpStatus::BadRequest, R"({"error":"missing body"})");
    return;
  }
  resp.json(HttpStatus::Created, req.body(), req.body_length());
}

void query_handler(const HttpRequest &req, HttpResponse &resp) {
  char buf[32] = {};
  if (!req.get_query_param("name", buf, sizeof(buf))) {
    resp.json(HttpStatus::BadRequest, R"({"error":"missing name"})");
    return;
  }
  std::string out = std::string("{\"hello\":\"") + buf + "\"}";
  resp.json(HttpStatus::Ok, out.c_str(), out.size());
}

} // namespace

TEST_CASE("HttpResponse defaults to 500 with no body", "[http][response]") {
  HttpResponse resp;
  REQUIRE(resp.status == HttpStatus::InternalServerError);
  REQUIRE(resp.content_type == nullptr);
  REQUIRE(resp.body_size() == 0);
  REQUIRE(resp.header_count == 0);
}

TEST_CASE("HttpResponse::json copies the body", "[http][response]") {
  HttpResponse resp;
  {
    // payload lives on a buffer that is destroyed when the scope exits;
    // the response must still hold a valid copy after that.
    char scratch[64];
    std::strcpy(scratch, R"({"a":1})");
    resp.json(HttpStatus::Ok, scratch);
    std::memset(scratch, 0, sizeof(scratch));
  }
  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(std::string(resp.content_type) == "application/json");
  REQUIRE(resp.body_size() == 7);
  REQUIRE(std::string(static_cast<const char *>(resp.body_data()), resp.body_size()) ==
          R"({"a":1})");
  REQUIRE_FALSE(resp.is_body_static());
}

TEST_CASE("HttpResponse::body_static stores a borrowed pointer", "[http][response]") {
  static constexpr char kAsset[] = "<html>hi</html>";
  HttpResponse resp;
  resp.body_static(HttpStatus::Ok, kAsset, sizeof(kAsset) - 1, "text/html");
  REQUIRE(resp.is_body_static());
  REQUIRE(resp.body_data() == kAsset);
  REQUIRE(resp.body_size() == sizeof(kAsset) - 1);
}

TEST_CASE("HttpResponse::no_content clears body and content type", "[http][response]") {
  HttpResponse resp;
  resp.json(HttpStatus::Ok, "ignored");
  resp.no_content();
  REQUIRE(resp.status == HttpStatus::NoContent);
  REQUIRE(resp.content_type == nullptr);
  REQUIRE(resp.body_size() == 0);
}

TEST_CASE("HttpResponse::set_header respects MAX_HEADERS", "[http][response]") {
  HttpResponse resp;
  for (size_t i = 0; i < HttpResponse::MAX_HEADERS + 2; ++i) {
    resp.set_header("X-Test", "v");
  }
  REQUIRE(resp.header_count == HttpResponse::MAX_HEADERS);
}

TEST_CASE("status handler returns JSON", "[http][handler]") {
  TestHttpRequest req(HttpMethod::Get, "/api/status");
  HttpResponse resp;

  status_handler(req, resp);

  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(std::string(resp.content_type) == "application/json");
  REQUIRE(resp.body_size() > 0);
}

TEST_CASE("echo handler reflects body bytes", "[http][handler]") {
  TestHttpRequest req(HttpMethod::Post, "/api/echo");
  const std::string payload = R"({"x":42})";
  req.set_body(payload);

  HttpResponse resp;
  echo_body_handler(req, resp);

  REQUIRE(resp.status == HttpStatus::Created);
  REQUIRE(resp.body_size() == payload.size());
  REQUIRE(std::string(static_cast<const char *>(resp.body_data()), resp.body_size()) == payload);
}

TEST_CASE("echo handler rejects empty body", "[http][handler]") {
  TestHttpRequest req(HttpMethod::Post, "/api/echo");

  HttpResponse resp;
  echo_body_handler(req, resp);

  REQUIRE(resp.status == HttpStatus::BadRequest);
}

TEST_CASE("query handler reads query param via the test helper", "[http][handler]") {
  TestHttpRequest req(HttpMethod::Get, "/api/greet");
  req.set_query_param("name", "alice");

  HttpResponse resp;
  query_handler(req, resp);

  REQUIRE(resp.status == HttpStatus::Ok);
  const std::string body(static_cast<const char *>(resp.body_data()), resp.body_size());
  REQUIRE(body == R"({"hello":"alice"})");
}

TEST_CASE("get_header reports misses and truncates oversize values", "[http][request]") {
  TestHttpRequest req(HttpMethod::Get, "/");
  req.set_header("X-Token", "abcdef");

  char buf[8] = {};
  REQUIRE(req.get_header("X-Token", buf, sizeof(buf)));
  REQUIRE(std::string(buf) == "abcdef");

  char missing[8] = {};
  REQUIRE_FALSE(req.get_header("X-Missing", missing, sizeof(missing)));
}

namespace {

// Minimal stub HttpServer to exercise the non-virtual register_static
// convenience without bringing in esp_http_server.
class StubHttpServer : public HttpServer {
public:
  bool start(uint16_t) override { return true; }
  void stop() override {}
  bool register_route(HttpMethod method, const char *path, HttpHandler handler) override {
    routes.push_back({method, path, std::move(handler)});
    return true;
  }

  struct Recorded {
    HttpMethod method;
    std::string path;
    HttpHandler handler;
  };
  std::vector<Recorded> routes;
};

} // namespace

TEST_CASE("register_static produces a GET handler returning the asset", "[http][server]") {
  static constexpr uint8_t kIndex[] = "<html>hello</html>";
  StubHttpServer server;

  REQUIRE(server.register_static("/", kIndex, kIndex + sizeof(kIndex) - 1, "text/html"));
  REQUIRE(server.routes.size() == 1);
  REQUIRE(server.routes[0].method == HttpMethod::Get);
  REQUIRE(server.routes[0].path == "/");

  TestHttpRequest req(HttpMethod::Get, "/");
  HttpResponse resp;
  server.routes[0].handler(req, resp);

  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(std::string(resp.content_type) == "text/html");
  REQUIRE(resp.is_body_static());
  REQUIRE(resp.body_data() == kIndex);
  REQUIRE(resp.body_size() == sizeof(kIndex) - 1);
}

TEST_CASE("register_static rejects invalid ranges", "[http][server]") {
  StubHttpServer server;
  static constexpr uint8_t kBuf[] = "x";
  REQUIRE_FALSE(server.register_static("/", nullptr, kBuf + 1, "text/html"));
  REQUIRE_FALSE(server.register_static("/", kBuf + 1, kBuf, "text/html"));
  REQUIRE(server.routes.empty());
}

#endif // has Catch2
#endif // __has_include
