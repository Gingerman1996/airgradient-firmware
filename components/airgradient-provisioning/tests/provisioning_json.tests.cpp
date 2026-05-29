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

#include "internal/provisioning_json.h"

namespace {

// Helper: parse a JSON string and call parse_provisioning_json().
ProvisioningJsonError parse(const char *json, ProvisioningData &out) {
  cJSON *root = cJSON_Parse(json);
  if (root == nullptr) {
    // Return a distinguishable error — MissingSsid is what parse would
    // return for a null root anyway.
    return ProvisioningJsonError::MissingSsid;
  }
  ProvisioningJsonError err = parse_provisioning_json(root, out);
  cJSON_Delete(root);
  return err;
}

} // namespace

// ============================================================================
// Valid credentials
// ============================================================================

TEST_CASE("provisioning_json: parse valid credentials with all fields", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"HomeWiFi","password":"secret12","disableCloud":true})", data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::string(data.ssid) == "HomeWiFi");
  REQUIRE(std::string(data.password) == "secret12");
  REQUIRE(data.disable_cloud == true);
}

TEST_CASE("provisioning_json: parse ssid only (backward compat)", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"OpenNet"})", data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::string(data.ssid) == "OpenNet");
  REQUIRE(std::string(data.password) == "");
  REQUIRE(data.disable_cloud == false);
}

TEST_CASE("provisioning_json: parse ssid and password, no disableCloud", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"Net","password":"pass1234"})", data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::string(data.ssid) == "Net");
  REQUIRE(std::string(data.password) == "pass1234");
  REQUIRE(data.disable_cloud == false);
}

// ============================================================================
// Static IP
// ============================================================================

TEST_CASE("provisioning_json: parse with valid staticIp", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(
      R"({"ssid":"HomeWiFi","password":"secret12","staticIp":{"ip":"192.168.110.251","netmask":"255.255.255.0","gateway":"192.168.110.1","dns":"8.8.8.8"}})",
      data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::string(data.ssid) == "HomeWiFi");
  REQUIRE(data.has_static_ip());
  REQUIRE(data.static_ip.ip == (192u | (168u << 8) | (110u << 16) | (251u << 24)));
  REQUIRE(data.static_ip.netmask == (255u | (255u << 8) | (255u << 16) | (0u << 24)));
  REQUIRE(data.static_ip.gateway == (192u | (168u << 8) | (110u << 16) | (1u << 24)));
  REQUIRE(data.static_ip.dns_primary == (8u | (8u << 8) | (8u << 16) | (8u << 24)));
}

TEST_CASE("provisioning_json: without staticIp defaults to DHCP", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"HomeWiFi","password":"secret12"})", data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE_FALSE(data.has_static_ip());
  REQUIRE(data.static_ip.ip == 0);
}

TEST_CASE("provisioning_json: malformed static IP address is rejected", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(
      R"({"ssid":"Net","staticIp":{"ip":"not.an.ip","netmask":"255.255.255.0","gateway":"192.168.1.1","dns":"8.8.8.8"}})",
      data);
  REQUIRE(err == ProvisioningJsonError::InvalidStaticIp);
}

TEST_CASE("provisioning_json: staticIp with ip=0.0.0.0 is rejected", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(
      R"({"ssid":"Net","staticIp":{"ip":"0.0.0.0","netmask":"255.255.255.0","gateway":"192.168.1.1","dns":"8.8.8.8"}})",
      data);
  REQUIRE(err == ProvisioningJsonError::InvalidStaticIp);
}

// ============================================================================
// Rejection cases
// ============================================================================

TEST_CASE("provisioning_json: missing ssid", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(R"({"password":"secret"})", data);
  REQUIRE(err == ProvisioningJsonError::MissingSsid);
}

TEST_CASE("provisioning_json: empty ssid", "[provisioning_json]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"","password":"secret"})", data);
  REQUIRE(err == ProvisioningJsonError::MissingSsid);
}

TEST_CASE("provisioning_json: null root returns MissingSsid", "[provisioning_json]") {
  ProvisioningData data;
  REQUIRE(parse_provisioning_json(nullptr, data) == ProvisioningJsonError::MissingSsid);
}

// ============================================================================
// Password length (WPA-PSK range: 8..63; empty allowed for open networks)
// ============================================================================

TEST_CASE("provisioning_json: empty password is accepted (open network)",
          "[provisioning_json][password]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"OpenNet","password":""})", data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::string(data.password) == "");
}

TEST_CASE("provisioning_json: password shorter than 8 chars is rejected",
          "[provisioning_json][password]") {
  ProvisioningData data;
  // 1-char and 7-char passwords are both outside the WPA-PSK range and
  // would trip ESP_ERR_WIFI_PASSWORD synchronously inside the HAL.
  REQUIRE(parse(R"({"ssid":"Net","password":"x"})", data) ==
          ProvisioningJsonError::InvalidPassword);
  REQUIRE(parse(R"({"ssid":"Net","password":"short77"})", data) ==
          ProvisioningJsonError::InvalidPassword);
}

TEST_CASE("provisioning_json: password of exactly 8 chars is accepted (lower bound)",
          "[provisioning_json][password]") {
  ProvisioningData data;
  auto err = parse(R"({"ssid":"Net","password":"longpass"})", data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::string(data.password) == "longpass");
}

TEST_CASE("provisioning_json: password of exactly 63 chars is accepted (upper bound)",
          "[provisioning_json][password]") {
  ProvisioningData data;
  // 63 'a's — the WPA-PSK maximum.
  auto err = parse(
      R"({"ssid":"Net","password":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})",
      data);
  REQUIRE(err == ProvisioningJsonError::Ok);
  REQUIRE(std::strlen(data.password) == 63);
}

TEST_CASE("provisioning_json: password of 64 or more chars is rejected",
          "[provisioning_json][password]") {
  ProvisioningData data;
  // 64 'a's — one past the WPA-PSK maximum. Rejected rather than
  // silently truncated so the caller can fix it.
  auto err = parse(
      R"({"ssid":"Net","password":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})",
      data);
  REQUIRE(err == ProvisioningJsonError::InvalidPassword);
}
