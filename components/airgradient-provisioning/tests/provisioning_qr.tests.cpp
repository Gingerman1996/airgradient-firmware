/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "services/provisioning_qr.h"

using AirgradientProvisioning::encode_go_to_app_qr;
using AirgradientProvisioning::encode_wifi_qr;
using AirgradientProvisioning::QrCode;
using AirgradientProvisioning::WifiAuth;

namespace {

// Finder-pattern corners must be dark in any valid QR code — cheap
// smoke that the buffer holds a real matrix.
void require_finder_patterns(const QrCode &q) {
  const int s = q.size();
  REQUIRE(s > 0);
  CHECK(q.module_on(0, 0));
  CHECK(q.module_on(6, 0));
  CHECK(q.module_on(0, 6));
  CHECK(q.module_on(6, 6));
  CHECK(q.module_on(s - 1, 0));
  CHECK(q.module_on(s - 7, 0));
  CHECK(q.module_on(s - 1, 6));
  CHECK(q.module_on(0, s - 1));
  CHECK(q.module_on(0, s - 7));
  CHECK(q.module_on(6, s - 1));
}

} // namespace

TEST_CASE("encode_go_to_app_qr produces a valid QR code", "[provisioning_qr]") {
  QrCode q{};
  REQUIRE(encode_go_to_app_qr(&q));
  require_finder_patterns(q);

  CHECK_FALSE(q.module_on(-1, 0));
  CHECK_FALSE(q.module_on(0, q.size()));
}

TEST_CASE("encode_wifi_qr encodes a typical AirGradient SoftAP descriptor", "[provisioning_qr]") {
  QrCode q{};
  REQUIRE(encode_wifi_qr("airgradient-AABBCCDDEEFF", "cleanair", WifiAuth::Wpa, &q));
  require_finder_patterns(q);
}

TEST_CASE("encode_wifi_qr rejects oversized payload cleanly", "[provisioning_qr]") {
  // 180-byte SSID inflates the "WIFI:" payload past v6-L byte-mode
  // capacity (134 B) — qrcodegen rejects, wrapper must zero the buffer.
  char ssid[181];
  for (size_t i = 0; i < sizeof(ssid) - 1; ++i) {
    ssid[i] = 'A';
  }
  ssid[sizeof(ssid) - 1] = '\0';

  QrCode q{};
  for (auto &b : q.buffer) {
    b = 0xFF;
  }
  REQUIRE_FALSE(encode_wifi_qr(ssid, "cleanair", WifiAuth::Wpa, &q));
  CHECK(q.size() == 0);
}

TEST_CASE("encode_wifi_qr rejects empty SSID", "[provisioning_qr]") {
  QrCode q{};
  CHECK_FALSE(encode_wifi_qr("", "cleanair", WifiAuth::Wpa, &q));
  CHECK_FALSE(encode_wifi_qr(nullptr, "cleanair", WifiAuth::Wpa, &q));
  CHECK(q.size() == 0);
}
