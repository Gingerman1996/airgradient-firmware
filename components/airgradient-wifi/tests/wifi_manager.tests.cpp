/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>

#include <cstring>

#include "../hal/wifi_hal.h"
#include "../services/wifi_manager.h"

namespace {

class FakeWifiHal : public WifiHal {
public:
  // -- Lifecycle --
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode mode) override {
    set_mode_calls += 1;
    last_mode_set = mode;
    _mode = mode;
    return WifiStatus::Ok;
  }
  WifiMode get_mode() const override { return _mode; }

  WifiStatus connect_sta(const char *ssid, const char *password) override {
    connect_calls += 1;
    last_ssid = ssid != nullptr ? ssid : "";
    last_password = password != nullptr ? password : "";
    return connect_status;
  }
  WifiStatus disconnect_sta() override {
    disconnect_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus set_static_ip(const WifiStaticIpConfig &) override {
    set_static_ip_calls += 1;
    return WifiStatus::Ok;
  }
  WifiStatus clear_static_ip() override {
    clear_static_ip_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus start_scan(const WifiScanConfig &) override {
    scan_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus start_ap(const WifiApConfig &) override {
    start_ap_calls += 1;
    return WifiStatus::Ok;
  }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }

  WifiStatusSnapshot get_status() const override {
    WifiStatusSnapshot snap;
    snap.mode = _mode;
    return snap;
  }

  WifiStatus set_power_save(WifiPowerSave) override { return WifiStatus::Ok; }

  WifiStatus start_mdns(const WifiMdnsConfig &cfg) override {
    start_mdns_calls += 1;
    last_mdns_hostname = (cfg.hostname != nullptr) ? cfg.hostname : "";
    return WifiStatus::Ok;
  }
  WifiStatus stop_mdns() override {
    stop_mdns_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus clear_saved_credentials() override {
    clear_creds_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus arm_dhcp_timeout(uint32_t timeout_ms) override {
    dhcp_armed_calls += 1;
    last_dhcp_timeout_ms = timeout_ms;
    return WifiStatus::Ok;
  }
  WifiStatus cancel_dhcp_timeout() override {
    dhcp_cancel_calls += 1;
    return WifiStatus::Ok;
  }
  WifiStatus arm_retry_timer(uint32_t delay_ms) override {
    retry_armed_calls += 1;
    last_retry_delay_ms = delay_ms;
    return WifiStatus::Ok;
  }
  WifiStatus cancel_retry_timer() override {
    retry_cancel_calls += 1;
    return WifiStatus::Ok;
  }

  void set_on_sta_connected(WifiConnectedCallback cb) override { sta_connected_cb = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    sta_disconnected_cb = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { got_ip_cb = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback cb) override {
    scan_complete_cb = std::move(cb);
  }
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb) override {
    ap_join_cb = std::move(cb);
  }
  void set_on_ap_client_left(WifiApClientLeftCallback cb) override { ap_leave_cb = std::move(cb); }
  void set_on_dhcp_timeout(std::function<void()> cb) override { dhcp_timeout_cb = std::move(cb); }
  void set_on_retry_due(std::function<void()> cb) override { retry_due_cb = std::move(cb); }

  // Counters
  int set_mode_calls = 0;
  int connect_calls = 0;
  int disconnect_calls = 0;
  int set_static_ip_calls = 0;
  int clear_static_ip_calls = 0;
  int scan_calls = 0;
  int start_ap_calls = 0;
  int start_mdns_calls = 0;
  int stop_mdns_calls = 0;
  int clear_creds_calls = 0;
  int dhcp_armed_calls = 0;
  int dhcp_cancel_calls = 0;
  int retry_armed_calls = 0;
  int retry_cancel_calls = 0;

  WifiStatus connect_status = WifiStatus::Ok;
  WifiMode last_mode_set = WifiMode::Off;
  std::string last_ssid;
  std::string last_password;
  std::string last_mdns_hostname;
  uint32_t last_dhcp_timeout_ms = 0;
  uint32_t last_retry_delay_ms = 0;

  // Captured callbacks (so tests can drive events)
  WifiConnectedCallback sta_connected_cb;
  std::function<void(int)> sta_disconnected_cb;
  WifiGotIpCallback got_ip_cb;
  WifiScanCompleteCallback scan_complete_cb;
  WifiApClientJoinedCallback ap_join_cb;
  WifiApClientLeftCallback ap_leave_cb;
  std::function<void()> dhcp_timeout_cb;
  std::function<void()> retry_due_cb;

private:
  WifiMode _mode = WifiMode::Off;
};

WifiStaConfig make_sta_config(const char *ssid, uint8_t max_retry = 3) {
  WifiStaConfig cfg;
  std::strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
  std::strncpy(cfg.password, "secret", sizeof(cfg.password) - 1);
  cfg.max_retry_count = max_retry;
  cfg.initial_retry_interval_ms = 1000;
  cfg.max_retry_interval_ms = 8000;
  return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Disconnect reason mapping
// ---------------------------------------------------------------------------

TEST_CASE("map_disconnect_reason normalises raw ESP-IDF codes", "[wifi-manager][mapping]") {
  using R = WifiDisconnectReason;
  REQUIRE(WifiManager::map_disconnect_reason(2) == R::auth_failed);        // AUTH_EXPIRE
  REQUIRE(WifiManager::map_disconnect_reason(202) == R::auth_failed);      // AUTH_FAIL
  REQUIRE(WifiManager::map_disconnect_reason(201) == R::no_ap_found);      // NO_AP_FOUND
  REQUIRE(WifiManager::map_disconnect_reason(210) == R::no_ap_found);      // _W_COMPATIBLE_SECURITY
  REQUIRE(WifiManager::map_disconnect_reason(5) == R::assoc_failed);       // ASSOC_TOOMANY
  REQUIRE(WifiManager::map_disconnect_reason(8) == R::ap_disconnected);    // ASSOC_LEAVE
  REQUIRE(WifiManager::map_disconnect_reason(206) == R::ap_disconnected);  // AP_TSF_RESET
  REQUIRE(WifiManager::map_disconnect_reason(200) == R::connection_lost);  // BEACON_TIMEOUT
  REQUIRE(WifiManager::map_disconnect_reason(208) == R::connection_lost);  // ASSOC_COMEBACK_TIME...
  REQUIRE(WifiManager::map_disconnect_reason(15) == R::handshake_failed);  // 4WAY_HANDSHAKE_TIMEOUT
  REQUIRE(WifiManager::map_disconnect_reason(204) == R::handshake_failed); // HANDSHAKE_TIMEOUT
  REQUIRE(WifiManager::map_disconnect_reason(9999) == R::unknown);
}

TEST_CASE("is_retriable matches the spec policy", "[wifi-manager][mapping]") {
  using R = WifiDisconnectReason;
  REQUIRE(WifiManager::is_retriable(R::ap_disconnected));
  REQUIRE(WifiManager::is_retriable(R::connection_lost));
  REQUIRE(WifiManager::is_retriable(R::handshake_failed));
  REQUIRE(WifiManager::is_retriable(R::unknown));

  REQUIRE_FALSE(WifiManager::is_retriable(R::auth_failed));
  REQUIRE_FALSE(WifiManager::is_retriable(R::no_ap_found));
  REQUIRE_FALSE(WifiManager::is_retriable(R::assoc_failed));
  REQUIRE_FALSE(WifiManager::is_retriable(R::dhcp_failed));
  REQUIRE_FALSE(WifiManager::is_retriable(R::requested_by_user));
}

TEST_CASE("compute_backoff_ms doubles and caps", "[wifi-manager][backoff]") {
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 0) == 1000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 1) == 2000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 2) == 4000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 3) == 8000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 4) == 8000); // capped
  REQUIRE(WifiManager::compute_backoff_ms(0, 8000, 3) == 0);       // disabled
}

// ---------------------------------------------------------------------------
// Mode state machine
// ---------------------------------------------------------------------------

TEST_CASE("set_mode is idempotent", "[wifi-manager][mode]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  REQUIRE(mgr.set_mode(WifiMode::Off) == WifiStatus::Ok);
  REQUIRE(hal.set_mode_calls == 0); // already Off
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(hal.set_mode_calls == 1);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(hal.set_mode_calls == 1); // still 1 — idempotent
}

TEST_CASE("set_mode tears down mDNS and timers when leaving STA", "[wifi-manager][mode]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);

  WifiMdnsConfig mdns;
  mdns.hostname = "test-host";
  mgr.set_mdns_config(mdns);

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x0100007FU);
  REQUIRE(hal.start_mdns_calls == 1);

  mgr.set_mode(WifiMode::Off);
  REQUIRE(hal.stop_mdns_calls == 1);
  REQUIRE(hal.dhcp_cancel_calls >= 1);
  REQUIRE(hal.retry_cancel_calls >= 1);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("set_mode(Ap) while GotIp emits requested_by_user once",
          "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(fired == 0);

  const int prior_retry_armed = hal.retry_armed_calls;
  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok);
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::requested_by_user);
  REQUIRE(hal.retry_armed_calls == prior_retry_armed);
}

TEST_CASE("set_mode(Ap) while GotIp swallows the driver-echo disconnect",
          "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  mgr.set_mode(WifiMode::Ap);
  REQUIRE(fired == 1);

  // Driver echo after esp_wifi_set_mode must be swallowed, not retried.
  const int prior_retry_armed = hal.retry_armed_calls;
  hal.sta_disconnected_cb(8); // ASSOC_LEAVE, normally retriable
  REQUIRE(fired == 1);
  REQUIRE(hal.retry_armed_calls == prior_retry_armed);
}

TEST_CASE("set_mode(Off) from GotIp emits requested_by_user once",
          "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  REQUIRE(mgr.set_mode(WifiMode::Off) == WifiStatus::Ok);
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::requested_by_user);
}

TEST_CASE("set_mode(Ap) from Disconnected emits nothing", "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok);
  REQUIRE(fired == 0);
}

TEST_CASE("status_snapshot zeros STA-only fields when Disconnected", "[wifi-manager][snapshot]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  mgr.set_mode(WifiMode::Off);
  const WifiStatusSnapshot snap = mgr.status_snapshot();
  REQUIRE(snap.sta_state == WifiStaState::Disconnected);
  REQUIRE(snap.ip == WIFI_IP_INVALID);
  REQUIRE(snap.rssi == WIFI_RSSI_INVALID);
  REQUIRE(snap.channel == 0);
  REQUIRE(snap.ssid[0] == '\0');
  for (uint8_t i = 0; i < 6; ++i) {
    REQUIRE(snap.bssid[i] == 0);
  }
}

// ---------------------------------------------------------------------------
// Mode enforcement
// ---------------------------------------------------------------------------

TEST_CASE("connect requires STA or APSTA mode", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Ap);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
}

TEST_CASE("connect works in ApSta mode", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::ApSta);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
}

TEST_CASE("connect rejects empty SSID", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);
  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::InvalidArgument);
}

TEST_CASE("start_ap requires AP or APSTA mode", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  WifiApConfig cfg;
  std::strncpy(cfg.ssid, "MyAP", sizeof(cfg.ssid) - 1);
  REQUIRE(mgr.start_ap(cfg) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.start_ap(cfg) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Ap);
  REQUIRE(mgr.start_ap(cfg) == WifiStatus::Ok);
}

TEST_CASE("start_scan rejected in Off and AP modes", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Ap);
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.start_scan() == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 1);
}

TEST_CASE("start_scan rejected while STA is connected (spec answer 3)", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  // While Connecting
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  hal.sta_connected_cb();
  // While Connected (no IP yet)
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  hal.got_ip_cb(0x01010101);
  // While GotIp
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
}

// ---------------------------------------------------------------------------
// Connect flow + got-IP + mDNS
// ---------------------------------------------------------------------------

TEST_CASE("got_ip arms mDNS and dhcp timeout cancellation", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_dhcp_timeout_ms(5000);

  WifiMdnsConfig mdns;
  mdns.hostname = "ag-1";
  mgr.set_mdns_config(mdns);

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  REQUIRE(hal.connect_calls == 1);

  bool connected_fired = false;
  bool got_ip_fired = false;
  uint32_t ip_seen = 0;
  mgr.set_on_connected([&] { connected_fired = true; });
  mgr.set_on_got_ip([&](uint32_t ip) {
    got_ip_fired = true;
    ip_seen = ip;
  });

  hal.sta_connected_cb();
  REQUIRE(connected_fired);
  REQUIRE(hal.dhcp_armed_calls == 1);
  REQUIRE(hal.last_dhcp_timeout_ms == 5000);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Connected);

  hal.got_ip_cb(0xC0A80164U);
  REQUIRE(got_ip_fired);
  REQUIRE(ip_seen == 0xC0A80164U);
  REQUIRE(hal.dhcp_cancel_calls >= 1);
  REQUIRE(hal.start_mdns_calls == 1);
  REQUIRE(hal.last_mdns_hostname == "ag-1");
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::GotIp);
}

TEST_CASE("disconnect after got_ip stops mDNS and reports RequestedByUser",
          "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  WifiMdnsConfig mdns;
  mdns.hostname = "ag-1";
  mgr.set_mdns_config(mdns);

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  bool fired = false;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired = true;
  });

  REQUIRE(mgr.disconnect() == WifiStatus::Ok);
  REQUIRE(fired);
  REQUIRE(last_reason == WifiDisconnectReason::requested_by_user);
  REQUIRE(hal.stop_mdns_calls == 1);
  REQUIRE(hal.disconnect_calls == 1);

  // Driver echo should be swallowed.
  fired = false;
  hal.sta_disconnected_cb(8); // ASSOC_LEAVE
  REQUIRE_FALSE(fired);
}

// ---------------------------------------------------------------------------
// Retry policy
// ---------------------------------------------------------------------------

TEST_CASE("transient disconnect arms exponential retry timer", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net", /*max_retry=*/3));

  hal.sta_connected_cb();
  // Beacon timeout -> retriable
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 1);
  REQUIRE(hal.last_retry_delay_ms == 1000);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Connecting);

  // Fire the retry; manager attempts to reconnect.
  hal.retry_due_cb();
  REQUIRE(hal.connect_calls == 2);

  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 2);
  REQUIRE(hal.last_retry_delay_ms == 2000); // doubled

  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 3);
  REQUIRE(hal.last_retry_delay_ms == 4000);
}

TEST_CASE("retry exhaustion fires on_disconnected with normalised reason",
          "[wifi-manager][retry]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net", /*max_retry=*/2));

  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // attempt 1 -> retry
  REQUIRE(fired == 0);
  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // attempt 2 -> retry
  REQUIRE(fired == 0);
  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // exhausted -> emit
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::connection_lost);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("non-retriable reasons emit immediately", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net", /*max_retry=*/5));
  hal.sta_disconnected_cb(202); // AUTH_FAIL -> AuthFailed
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::auth_failed);
  REQUIRE(hal.retry_armed_calls == 0);
}

TEST_CASE("max_retry_count == 0 disables auto-retry", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.connect(make_sta_config("Net", /*max_retry=*/0));
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // retriable, but retries disabled
  REQUIRE(hal.retry_armed_calls == 0);
  REQUIRE(fired == 1);
}

TEST_CASE("disconnect() cancels pending retry timer", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  mgr.connect(make_sta_config("Net", /*max_retry=*/3));
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 1);

  mgr.disconnect();
  REQUIRE(hal.retry_cancel_calls >= 1);
  // Late-firing retry callback should be a no-op.
  hal.retry_due_cb();
  REQUIRE(hal.connect_calls == 1); // unchanged
}

// ---------------------------------------------------------------------------
// DHCP timeout policy
// ---------------------------------------------------------------------------

TEST_CASE("DHCP timeout disconnects with DhcpFailed reason (non-retriable)",
          "[wifi-manager][dhcp]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  REQUIRE(hal.dhcp_armed_calls == 1);

  hal.dhcp_timeout_cb();
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::dhcp_failed);
  REQUIRE(hal.disconnect_calls == 1);
  REQUIRE_FALSE(WifiManager::is_retriable(WifiDisconnectReason::dhcp_failed));
}

TEST_CASE("Stale DHCP timeout after got_ip is ignored", "[wifi-manager][dhcp]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  hal.dhcp_timeout_cb(); // late-arriving callback
  REQUIRE(fired == 0);
}

// ---------------------------------------------------------------------------
// Scan + AP client events
// ---------------------------------------------------------------------------

TEST_CASE("scan results pass through to product callback", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);

  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "AP1", sizeof(entries[0].ssid) - 1);
  std::strncpy(entries[1].ssid, "AP2", sizeof(entries[1].ssid) - 1);

  uint16_t seen_count = 0;
  std::string first_ssid;
  mgr.set_on_scan_complete([&](const WifiScanEntry *r, uint16_t c) {
    seen_count = c;
    if (c > 0)
      first_ssid = r[0].ssid;
  });

  REQUIRE(mgr.start_scan() == WifiStatus::Ok);
  hal.scan_complete_cb(entries, 2);
  REQUIRE(seen_count == 2);
  REQUIRE(first_ssid == "AP1");
}

TEST_CASE("AP client join/leave callbacks pass through", "[wifi-manager][ap]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Ap);

  int joined = 0;
  int left = 0;
  mgr.set_on_ap_client_joined([&](const uint8_t *) { joined += 1; });
  mgr.set_on_ap_client_left([&](const uint8_t *) { left += 1; });

  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  hal.ap_join_cb(mac);
  hal.ap_leave_cb(mac);
  REQUIRE(joined == 1);
  REQUIRE(left == 1);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_CASE("connect while connecting returns AlreadyInProgress", "[wifi-manager][edge]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.connect(make_sta_config("A")) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("B")) == WifiStatus::AlreadyInProgress);
}

TEST_CASE("set_mdns_config rejects empty hostname", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  WifiMdnsConfig mdns;
  REQUIRE(mgr.set_mdns_config(mdns) == WifiStatus::InvalidArgument);
}

TEST_CASE("clear_saved_credentials forwards to HAL", "[wifi-manager][creds]") {
  FakeWifiHal hal;
  WifiManager mgr(hal);
  REQUIRE(mgr.clear_saved_credentials() == WifiStatus::Ok);
  REQUIRE(hal.clear_creds_calls == 1);
}
