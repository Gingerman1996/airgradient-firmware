/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "../types/wifi_types.h"

TEST_CASE("airgradient-wifi public types expose invalid sentinels", "[airgradient-wifi]") {
  const WifiStatusSnapshot snapshot;
  REQUIRE(snapshot.mode == WifiMode::Off);
  REQUIRE(snapshot.sta_state == WifiStaState::Disconnected);
  REQUIRE(snapshot.ip == WIFI_IP_INVALID);
  REQUIRE(snapshot.rssi == WIFI_RSSI_INVALID);
  REQUIRE(snapshot.ap_client_count == 0);

  const WifiScanEntry entry;
  REQUIRE(entry.rssi == WIFI_RSSI_INVALID);
  REQUIRE(entry.auth_mode == WifiAuthMode::unknown);
  REQUIRE(entry.channel == 0);
  REQUIRE(entry.ssid[0] == '\0');

  const WifiStaConfig sta_config;
  REQUIRE(sta_config.max_retry_count == 5);
  REQUIRE(sta_config.initial_retry_interval_ms == 1000);
  REQUIRE(sta_config.max_retry_interval_ms == 30000);

  const WifiApConfig ap_config;
  REQUIRE(ap_config.channel == 1);
  REQUIRE(ap_config.max_connections == 4);
}
