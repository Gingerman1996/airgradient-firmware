/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include <cJSON.h>

#include "hal/http_response.h"
#include "internal/wifi_portal_transport.h"
#include "test_http_request.h"
#include "types/wifi_types.h"

namespace {

std::string body_as_string(const HttpResponse &r) {
  return std::string(static_cast<const char *>(r.body_data()), r.body_size());
}

// Helper that pulls a string field out of an HttpResponse JSON body.
std::string json_string_field(const HttpResponse &r, const char *key) {
  cJSON *root = cJSON_ParseWithLength(static_cast<const char *>(r.body_data()), r.body_size());
  REQUIRE(root != nullptr);
  cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
  REQUIRE(cJSON_IsString(node));
  std::string out(node->valuestring);
  cJSON_Delete(root);
  return out;
}

} // namespace

TEST_CASE("Portal GET /api/scan reports idle before any scan triggered", "[portal]") {
  WifiPortalTransport portal;
  TestHttpRequest req(HttpMethod::Get, "/api/scan");
  HttpResponse resp;
  portal.handle_scan_get(req, resp);

  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(json_string_field(resp, "status") == "idle");
}

TEST_CASE("Portal POST /api/scan invokes scan callback and reports scanning", "[portal]") {
  WifiPortalTransport portal;
  bool called = false;
  portal.set_on_scan_request([&]() {
    called = true;
    return true;
  });

  TestHttpRequest req(HttpMethod::Post, "/api/scan");
  HttpResponse resp;
  portal.handle_scan_post(req, resp);

  REQUIRE(called);
  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(json_string_field(resp, "status") == "scanning");
  REQUIRE(portal.scan_in_progress());

  // GET while scanning returns scanning, not idle.
  HttpResponse resp2;
  portal.handle_scan_get(req, resp2);
  REQUIRE(json_string_field(resp2, "status") == "scanning");
}

TEST_CASE("Portal returns scan results after update_scan_results", "[portal]") {
  WifiPortalTransport portal;
  portal.set_on_scan_request([]() { return true; });

  HttpResponse trigger;
  TestHttpRequest post(HttpMethod::Post, "/api/scan");
  portal.handle_scan_post(post, trigger);

  WifiScanEntry entries[2] = {};
  std::strcpy(entries[0].ssid, "HomeWiFi");
  entries[0].rssi = -45;
  entries[0].auth_mode = WifiAuthMode::wpa2_psk;
  entries[0].channel = 6;
  std::strcpy(entries[1].ssid, "Guest");
  entries[1].rssi = -62;
  entries[1].auth_mode = WifiAuthMode::open;
  entries[1].channel = 1;
  portal.update_scan_results(entries, 2);
  REQUIRE_FALSE(portal.scan_in_progress());

  TestHttpRequest get(HttpMethod::Get, "/api/scan");
  HttpResponse resp;
  portal.handle_scan_get(get, resp);

  cJSON *root =
      cJSON_ParseWithLength(static_cast<const char *>(resp.body_data()), resp.body_size());
  REQUIRE(root != nullptr);
  cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
  REQUIRE(cJSON_IsString(status));
  REQUIRE(std::string(status->valuestring) == "done");

  cJSON *networks = cJSON_GetObjectItemCaseSensitive(root, "networks");
  REQUIRE(cJSON_IsArray(networks));
  REQUIRE(cJSON_GetArraySize(networks) == 2);

  cJSON *first = cJSON_GetArrayItem(networks, 0);
  cJSON *first_ssid = cJSON_GetObjectItemCaseSensitive(first, "ssid");
  REQUIRE(std::string(first_ssid->valuestring) == "HomeWiFi");
  cJSON_Delete(root);
}

TEST_CASE("Portal POST /api/provision parses credentials and invokes callback", "[portal]") {
  WifiPortalTransport portal;
  ProvisioningData captured = {};
  bool called = false;
  portal.set_on_credentials([&](const ProvisioningData &d) {
    captured = d;
    called = true;
    return true;
  });

  const std::string body = R"({"ssid":"HomeWiFi","password":"secret12","disableCloud":true})";
  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(body);

  HttpResponse resp;
  portal.handle_provision_post(req, resp);

  REQUIRE(called);
  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(json_string_field(resp, "status") == "connecting");
  REQUIRE(std::string(captured.ssid) == "HomeWiFi");
  REQUIRE(std::string(captured.password) == "secret12");
  REQUIRE(captured.disable_cloud);
  REQUIRE_FALSE(captured.has_static_ip());
}

TEST_CASE("Portal rejects provision without ssid", "[portal]") {
  WifiPortalTransport portal;
  portal.set_on_credentials([](const ProvisioningData &) { return true; });

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"password":"secret"})");
  HttpResponse resp;
  portal.handle_provision_post(req, resp);

  REQUIRE(resp.status == HttpStatus::BadRequest);
}

TEST_CASE("Portal rejects malformed JSON body", "[portal]") {
  WifiPortalTransport portal;
  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body("{not json");
  HttpResponse resp;
  portal.handle_provision_post(req, resp);

  REQUIRE(resp.status == HttpStatus::BadRequest);
}

TEST_CASE("Portal rejects WPA password shorter than 8 chars", "[portal][password]") {
  WifiPortalTransport portal;
  bool called = false;
  portal.set_on_credentials([&](const ProvisioningData &) {
    called = true;
    return true;
  });

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"Net","password":"short77"})");
  HttpResponse resp;
  portal.handle_provision_post(req, resp);

  REQUIRE(resp.status == HttpStatus::BadRequest);
  REQUIRE_FALSE(called); // never reaches the state machine
}

TEST_CASE("Portal parses static IP payload", "[portal]") {
  WifiPortalTransport portal;
  ProvisioningData captured = {};
  portal.set_on_credentials([&](const ProvisioningData &d) {
    captured = d;
    return true;
  });

  const std::string body = R"({
    "ssid": "HomeWiFi",
    "staticIp": {
      "ip": "192.168.1.100",
      "netmask": "255.255.255.0",
      "gateway": "192.168.1.1",
      "dns": "8.8.8.8"
    }
  })";
  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(body);
  HttpResponse resp;
  portal.handle_provision_post(req, resp);

  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(captured.has_static_ip());
  // 192.168.1.100 little-endian: 0x6401a8c0
  REQUIRE(captured.static_ip.ip == ((192u) | (168u << 8) | (1u << 16) | (100u << 24)));
  REQUIRE(captured.static_ip.dns_primary == ((8u) | (8u << 8) | (8u << 16) | (8u << 24)));
}

TEST_CASE("Portal captive-probe handler returns 302 to absolute AP URL", "[portal]") {
  TestHttpRequest req(HttpMethod::Get, "/hotspot-detect.html");
  HttpResponse resp;
  WifiPortalTransport::handle_captive_probe(req, resp);

  REQUIRE(resp.status == HttpStatus::Found);

  // The driver pulls headers from response.headers[]. Locate Location
  // and verify it's an absolute URL pointing at the AP — iOS CNA needs
  // the fully qualified target to treat the response as a captive
  // signal rather than a normal redirect.
  bool location_found = false;
  for (size_t i = 0; i < resp.header_count; ++i) {
    if (resp.headers[i].name != nullptr && std::string(resp.headers[i].name) == "Location") {
      REQUIRE(std::string(resp.headers[i].value) == "http://192.168.4.1/");
      location_found = true;
    }
  }
  REQUIRE(location_found);
}

TEST_CASE("Portal GET /api/status reflects state changes", "[portal]") {
  WifiPortalTransport portal;
  TestHttpRequest req(HttpMethod::Get, "/api/status");

  HttpResponse r1;
  portal.handle_status_get(req, r1);
  REQUIRE(json_string_field(r1, "state") == "waiting");

  portal.set_state(WifiPortalTransport::PortalState::Connecting);
  HttpResponse r2;
  portal.handle_status_get(req, r2);
  REQUIRE(json_string_field(r2, "state") == "connecting");

  portal.set_state(WifiPortalTransport::PortalState::Connected);
  HttpResponse r3;
  portal.handle_status_get(req, r3);
  REQUIRE(json_string_field(r3, "state") == "connected");

  portal.set_state(WifiPortalTransport::PortalState::Failed);
  HttpResponse r4;
  portal.handle_status_get(req, r4);
  REQUIRE(json_string_field(r4, "state") == "failed");
}
