/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <cstring>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "cJSON.h"

#include "ag_client_test_access.h"
#include "mock_http_client.h"
#include "services/ag_client.h"

namespace {

struct ClientFixture {
  AgClient client;
  AgClientTestAccess access{client};
  MockHttpClient mock_http;

  ClientFixture() {
    access.set_serial_number("aabbccddeeff");
    access.set_network(NetworkType::Wifi);
    access.inject_http_client(&mock_http);
  }
};

// All fields invalid; used by transport-focused tests.
MeasuresBasic make_invalid_basic() {
  MeasuresBasic m{};
  m.co2.co2 = MeasuresInvalid::CO2;
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_01 = MeasuresInvalid::PM;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.pm_a.pm_10 = MeasuresInvalid::PM;
  m.pm_a.pm_01_sp = MeasuresInvalid::PM;
  m.pm_a.pm_25_sp = MeasuresInvalid::PM;
  m.pm_a.pm_10_sp = MeasuresInvalid::PM;
  m.pm_a.pm_03_pc = MeasuresInvalid::PM;
  m.pm_a.pm_05_pc = MeasuresInvalid::PM;
  m.pm_a.pm_01_pc = MeasuresInvalid::PM;
  m.pm_a.pm_25_pc = MeasuresInvalid::PM;
  m.pm_a.pm_5_pc = MeasuresInvalid::PM;
  m.pm_a.pm_10_pc = MeasuresInvalid::PM;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  return m;
}

} // namespace

TEST_CASE("http_post_measures builds correct URL and content type", "[ag_client]") {
  ClientFixture f;
  const auto m = make_invalid_basic();

  f.mock_http.next_transport_ok = true;
  f.mock_http.next_status = 200;

  const auto result = f.client.http_post_measures(m, -55);
  REQUIRE(result == AgClientResult::Ok);
  REQUIRE(f.mock_http.post_call_count == 1);
  REQUIRE(f.mock_http.last_url ==
          "https://hw.airgradient.com/sensors/airgradient:aabbccddeeff/measures");
  REQUIRE(f.mock_http.last_content_type == "application/json");

  std::string body(f.mock_http.last_post_body.begin(), f.mock_http.last_post_body.end());
  cJSON *doc = cJSON_Parse(body.c_str());
  REQUIRE(doc != nullptr);
  REQUIRE(cJSON_GetObjectItem(doc, "wifi") != nullptr);
  cJSON_Delete(doc);
}

TEST_CASE("http_post_measures maps 429 to Ok", "[ag_client]") {
  ClientFixture f;
  const auto m = make_invalid_basic();
  f.mock_http.next_status = 429;
  REQUIRE(f.client.http_post_measures(m, 0) == AgClientResult::Ok);
}

TEST_CASE("http_post_measures returns ServerError on 500", "[ag_client]") {
  ClientFixture f;
  const auto m = make_invalid_basic();
  f.mock_http.next_status = 500;
  REQUIRE(f.client.http_post_measures(m, 0) == AgClientResult::ServerError);
}

TEST_CASE("http_post_measures returns TransportError when HTTP fails", "[ag_client]") {
  ClientFixture f;
  const auto m = make_invalid_basic();
  f.mock_http.next_transport_ok = false;
  REQUIRE(f.client.http_post_measures(m, 0) == AgClientResult::TransportError);
}

TEST_CASE("http_post_measures accepts MeasuresAGo overload", "[ag_client]") {
  ClientFixture f;
  MeasuresAGo m{};
  m.co2.co2 = MeasuresInvalid::CO2;
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_01 = MeasuresInvalid::PM;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.pm_a.pm_10 = MeasuresInvalid::PM;
  m.pm_a.pm_03_pc = MeasuresInvalid::PM;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  m.power.battery_voltage = 4.0f;

  f.mock_http.next_status = 200;
  REQUIRE(f.client.http_post_measures(m, -50) == AgClientResult::Ok);

  std::string body(f.mock_http.last_post_body.begin(), f.mock_http.last_post_body.end());
  cJSON *doc = cJSON_Parse(body.c_str());
  REQUIRE(doc != nullptr);
  REQUIRE(cJSON_GetObjectItem(doc, "volt") != nullptr);
  cJSON_Delete(doc);
}

TEST_CASE("http_post_measures accepts full Measures overload", "[ag_client]") {
  ClientFixture f;
  Measures m{};
  m.co2.co2 = 450;
  m.temp_hum_a.temperature = 20.0f;
  m.temp_hum_b.temperature = 22.0f;
  // Rest zero-initialised -- transport-only test.

  f.mock_http.next_status = 200;
  REQUIRE(f.client.http_post_measures(m, -40) == AgClientResult::Ok);

  std::string body(f.mock_http.last_post_body.begin(), f.mock_http.last_post_body.end());
  cJSON *doc = cJSON_Parse(body.c_str());
  REQUIRE(doc != nullptr);
  cJSON *atmp = cJSON_GetObjectItem(doc, "atmp");
  REQUIRE(atmp != nullptr);
  REQUIRE(atmp->valuedouble == 21.0); // dual-channel average
  cJSON_Delete(doc);
}

TEST_CASE("http_fetch_config builds correct URL", "[ag_client]") {
  ClientFixture f;
  f.mock_http.next_status = 200;
  f.mock_http.next_get_body = "{\"model\":\"O1\"}";

  char buf[256];
  size_t written = 0;
  const auto result = f.client.http_fetch_config(buf, sizeof(buf), &written);
  REQUIRE(result == AgClientResult::Ok);
  REQUIRE(f.mock_http.last_url ==
          "https://hw.airgradient.com/sensors/airgradient:aabbccddeeff/one/config");
  REQUIRE(written == std::strlen("{\"model\":\"O1\"}"));
  REQUIRE(std::string(buf) == "{\"model\":\"O1\"}");
}

TEST_CASE("http_fetch_config interprets 400 as NotRegistered", "[ag_client]") {
  ClientFixture f;
  f.mock_http.next_status = 400;
  f.mock_http.next_get_body = "";

  char buf[256];
  size_t written = 0;
  REQUIRE(f.client.http_fetch_config(buf, sizeof(buf), &written) == AgClientResult::NotRegistered);
}

TEST_CASE("http_fetch_config reports BufferTooSmall on truncation", "[ag_client]") {
  ClientFixture f;
  f.mock_http.next_status = 200;
  f.mock_http.next_get_body = "this body is definitely much larger than the tiny buffer";

  char small_buf[16];
  size_t written = 0;
  const auto result = f.client.http_fetch_config(small_buf, sizeof(small_buf), &written);
  REQUIRE(result == AgClientResult::BufferTooSmall);
  REQUIRE(written > 0);
  REQUIRE(written < sizeof(small_buf)); // excludes NUL
}

TEST_CASE("http_fetch_config returns TransportError on HTTP failure", "[ag_client]") {
  ClientFixture f;
  f.mock_http.next_transport_ok = false;
  char buf[256];
  size_t written = 0;
  REQUIRE(f.client.http_fetch_config(buf, sizeof(buf), &written) == AgClientResult::TransportError);
}

TEST_CASE("http_fetch_config maps 500 to ServerError", "[ag_client]") {
  ClientFixture f;
  f.mock_http.next_status = 500;
  char buf[256];
  size_t written = 0;
  REQUIRE(f.client.http_fetch_config(buf, sizeof(buf), &written) == AgClientResult::ServerError);
}

TEST_CASE("set_http_domain overrides the host", "[ag_client]") {
  ClientFixture f;
  f.client.set_http_domain("staging.example.com");
  f.mock_http.next_status = 200;
  f.mock_http.next_get_body = "{}";

  char buf[256];
  size_t written = 0;
  f.client.http_fetch_config(buf, sizeof(buf), &written);
  REQUIRE(f.mock_http.last_url ==
          "https://staging.example.com/sensors/airgradient:aabbccddeeff/one/config");

  f.client.reset_http_domain();
  f.client.http_fetch_config(buf, sizeof(buf), &written);
  REQUIRE(f.mock_http.last_url ==
          "https://hw.airgradient.com/sensors/airgradient:aabbccddeeff/one/config");
}

TEST_CASE("begin with cellular returns false", "[ag_client]") {
  AgClient client;
  REQUIRE_FALSE(client.begin("aabbccddeeff", NetworkType::Cellular));
}

TEST_CASE("begin with null serial number returns false", "[ag_client]") {
  AgClient client;
  REQUIRE_FALSE(client.begin(nullptr, NetworkType::Wifi));
}

TEST_CASE("http_post_measures maps 201 to Ok", "[ag_client]") {
  ClientFixture f;
  const auto m = make_invalid_basic();
  f.mock_http.next_status = 201;
  REQUIRE(f.client.http_post_measures(m, 0) == AgClientResult::Ok);
}

TEST_CASE("http_fetch_config: truncation beats 400 status", "[ag_client]") {
  ClientFixture f;
  f.mock_http.next_status = 400;
  f.mock_http.next_get_body = "this body is definitely much larger than the tiny buffer";

  char small_buf[16];
  size_t written = 0;
  REQUIRE(f.client.http_fetch_config(small_buf, sizeof(small_buf), &written) ==
          AgClientResult::BufferTooSmall);
  REQUIRE(written > 0);
}
