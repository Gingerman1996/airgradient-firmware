/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "internal/scan_filter.h"

namespace {

WifiScanEntry make_entry(const char *ssid, int8_t rssi, WifiAuthMode auth = WifiAuthMode::wpa2_psk,
                         uint8_t channel = 1) {
  WifiScanEntry e = {};
  std::strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
  e.rssi = rssi;
  e.auth_mode = auth;
  e.channel = channel;
  return e;
}

} // namespace

TEST_CASE("ScanFilter handles empty input", "[scan_filter]") {
  WifiScanEntry out[8] = {};
  REQUIRE(ScanFilter::apply(nullptr, 0, out, 8) == 0);

  WifiScanEntry in[1] = {};
  REQUIRE(ScanFilter::apply(in, 0, out, 8) == 0);
}

TEST_CASE("ScanFilter drops empty SSIDs", "[scan_filter]") {
  WifiScanEntry in[] = {
      make_entry("", -40),
      make_entry("HomeWiFi", -50),
  };
  WifiScanEntry out[4] = {};
  const size_t n = ScanFilter::apply(in, 2, out, 4);
  REQUIRE(n == 1);
  REQUIRE(std::strcmp(out[0].ssid, "HomeWiFi") == 0);
}

TEST_CASE("ScanFilter drops weak entries below -75 dBm", "[scan_filter]") {
  WifiScanEntry in[] = {
      make_entry("Strong", -50), make_entry("Weak", -80),
      make_entry("Boundary", -75), // exactly at the threshold — kept
  };
  WifiScanEntry out[4] = {};
  const size_t n = ScanFilter::apply(in, 3, out, 4);
  REQUIRE(n == 2);
  // Sorted by RSSI descending.
  REQUIRE(std::strcmp(out[0].ssid, "Strong") == 0);
  REQUIRE(std::strcmp(out[1].ssid, "Boundary") == 0);
}

TEST_CASE("ScanFilter deduplicates by SSID keeping strongest RSSI", "[scan_filter]") {
  WifiScanEntry in[] = {
      make_entry("Home", -60),
      make_entry("Home", -45), // stronger duplicate — should win
      make_entry("Home", -70),
      make_entry("Other", -55),
  };
  WifiScanEntry out[8] = {};
  const size_t n = ScanFilter::apply(in, 4, out, 8);
  REQUIRE(n == 2);
  REQUIRE(std::strcmp(out[0].ssid, "Home") == 0);
  REQUIRE(out[0].rssi == -45);
  REQUIRE(std::strcmp(out[1].ssid, "Other") == 0);
}

TEST_CASE("ScanFilter sorts by RSSI descending", "[scan_filter]") {
  WifiScanEntry in[] = {
      make_entry("Far", -70),
      make_entry("Near", -40),
      make_entry("Mid", -55),
  };
  WifiScanEntry out[8] = {};
  const size_t n = ScanFilter::apply(in, 3, out, 8);
  REQUIRE(n == 3);
  REQUIRE(out[0].rssi == -40);
  REQUIRE(out[1].rssi == -55);
  REQUIRE(out[2].rssi == -70);
}

TEST_CASE("ScanFilter caps at 30 networks", "[scan_filter]") {
  WifiScanEntry in[40];
  for (size_t i = 0; i < 40; ++i) {
    char ssid[16];
    std::snprintf(ssid, sizeof(ssid), "net-%02zu", i);
    // Use distinct SSIDs and varying RSSI so all 40 would pass without
    // the cap.
    in[i] = make_entry(ssid, -40 - static_cast<int8_t>(i % 30));
  }
  WifiScanEntry out[40] = {};
  const size_t n = ScanFilter::apply(in, 40, out, 40);
  REQUIRE(n == ScanFilter::SCAN_MAX_NETWORKS);
}
