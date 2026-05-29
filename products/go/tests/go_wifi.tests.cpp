/**
 * AirGradient Go — WifiService unit tests
 *
 * Drives saved-credentials, factory-fallback, callback adapters, the
 * initial-connect deadline, and shutdown behaviour through friend-class
 * access. WifiManager is the real one, linked against a fake WifiHal.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_wifi.h"

#include "go_events.h"
#include "hal/ble_server.h"
#include "hal/http_server.h"
#include "hal/wifi_hal.h"
#include "rtos.h"
#include "services/wifi_manager.h"
#include "types/provisioning_types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// FakeWifiHal — minimal in-memory HAL for WifiManager
// ---------------------------------------------------------------------------

class FakeWifiHal : public WifiHal {
public:
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode mode) override {
    _mode = mode;
    last_mode_set = mode;
    return WifiStatus::Ok;
  }
  WifiMode get_mode() const override { return _mode; }

  WifiStatus connect_sta(const char *ssid, const char *password, bool persist = true) override {
    ++connect_calls;
    last_ssid = ssid != nullptr ? ssid : "";
    last_password = password != nullptr ? password : "";
    last_persist = persist;
    return WifiStatus::Ok;
  }
  WifiStatus disconnect_sta() override {
    ++disconnect_calls;
    return WifiStatus::Ok;
  }
  bool has_saved_credentials() const override { return saved_credentials_present; }

  WifiStatus set_static_ip(const WifiStaticIpConfig &cfg) override {
    last_static_ip = cfg;
    ++set_static_ip_calls;
    return WifiStatus::Ok;
  }
  WifiStatus clear_static_ip() override {
    ++clear_static_ip_calls;
    return WifiStatus::Ok;
  }

  WifiStatus start_scan(const WifiScanConfig &) override { return WifiStatus::Ok; }
  WifiStatus start_ap(const WifiApConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }

  WifiStatusSnapshot get_status() const override {
    WifiStatusSnapshot s;
    s.mode = _mode;
    s.rssi = -55;
    return s;
  }
  WifiStatus set_power_save(WifiPowerSave) override { return WifiStatus::Ok; }
  WifiStatus start_mdns(const WifiMdnsConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_mdns() override { return WifiStatus::Ok; }
  WifiStatus clear_saved_credentials() override {
    ++clear_creds_calls;
    saved_credentials_present = false;
    return WifiStatus::Ok;
  }

  WifiStatus arm_dhcp_timeout(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_dhcp_timeout() override { return WifiStatus::Ok; }
  WifiStatus arm_retry_timer(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_retry_timer() override { return WifiStatus::Ok; }

  void set_on_sta_connected(WifiConnectedCallback cb) override { sta_connected_cb = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    sta_disconnected_cb = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { got_ip_cb = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback) override {}
  void set_on_ap_client_joined(WifiApClientJoinedCallback) override {}
  void set_on_ap_client_left(WifiApClientLeftCallback) override {}
  void set_on_dhcp_timeout(std::function<void()>) override {}
  void set_on_retry_due(std::function<void()>) override {}

  // Observables
  int connect_calls = 0;
  int disconnect_calls = 0;
  int set_static_ip_calls = 0;
  int clear_static_ip_calls = 0;
  int clear_creds_calls = 0;
  bool saved_credentials_present = false;
  WifiMode last_mode_set = WifiMode::Off;
  std::string last_ssid;
  std::string last_password;
  bool last_persist = true;
  WifiStaticIpConfig last_static_ip{};

  WifiConnectedCallback sta_connected_cb;
  std::function<void(int)> sta_disconnected_cb;
  WifiGotIpCallback got_ip_cb;

private:
  WifiMode _mode = WifiMode::Off;
};

// ---------------------------------------------------------------------------
// No-op stubs for the borrowed BLE / HTTP server refs.
// CP2.2 never touches them; CP2.3 wires provisioning.
// ---------------------------------------------------------------------------

class StubBleServer : public AgBleServer {
public:
  bool init(const char *) override { return true; }
  void deinit() override {}
  bool set_security(AgBleIoCapability, uint8_t) override { return true; }
  bool delete_all_bonds() override { return true; }
  AgBleGattService *add_service(const char *) override { return nullptr; }
  bool set_advertising_name(const char *) override { return true; }
  bool add_advertised_service_uuid(const char *) override { return true; }
  bool set_manufacturer_data(const uint8_t *, size_t) override { return true; }
  bool start_advertising() override { return true; }
  bool stop_advertising() override { return true; }
  void set_connect_callback(AgBleConnectCallback) override {}
  void set_disconnect_callback(AgBleDisconnectCallback) override {}
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback) override {}
  void set_auth_complete_callback(AgBleAuthCompleteCallback) override {}
};

class StubHttpServer : public HttpServer {
public:
  bool start(uint16_t) override { return true; }
  void stop() override {}
  bool register_route(HttpMethod, const char *, HttpHandler) override { return true; }
  bool unregister_route(HttpMethod, const char *) override { return true; }
  void unregister_all() override {}
};

// ---------------------------------------------------------------------------
// FakeRTOS — captures queue_send into a vector, controllable get_time_ms.
// ---------------------------------------------------------------------------

class FakeRTOS : public FreeRTOS {
public:
  uint64_t get_time_ms_impl() override { return now_ms; }
  void queue_send_impl(RtosQueueHandle, const void *item, uint32_t) override {
    if (item != nullptr) {
      captured.push_back(*static_cast<const Event *>(item));
    }
  }

  void set_now(uint64_t ms) { now_ms = ms; }
  void advance(uint64_t ms) { now_ms += ms; }
  bool has_event(EventType t) const {
    for (const auto &e : captured) {
      if (e.type == t)
        return true;
    }
    return false;
  }
  const Event *first_event(EventType t) const {
    for (const auto &e : captured) {
      if (e.type == t)
        return &e;
    }
    return nullptr;
  }

  uint64_t now_ms = 0;
  std::vector<Event> captured;
};

} // namespace

// ---------------------------------------------------------------------------
// WifiServiceTestAccess — friend for private state inspection.
// ---------------------------------------------------------------------------

class WifiServiceTestAccess {
public:
  static uint32_t deadline(const WifiService &s) { return s._initial_connect_deadline_ms; }
  static bool clear_pending(const WifiService &s) { return s._clear_deadline_pending.load(); }
  static bool provisioning_active(const WifiService &s) { return s._provisioning_active; }
  static void set_provisioning_active(WifiService &s, bool active) {
    s._provisioning_active = active;
  }
  static void set_switching_transport(WifiService &s, bool on) { s._switching_transport = on; }
  static void set_transport(WifiService &s, ProvisioningTransport t) { s._transport = t; }
  static void on_provisioning_event(WifiService &s, const ProvisioningEventInfo &info) {
    s._on_provisioning_event(info);
  }
};

namespace {

// Common fixture: real WifiManager backed by FakeWifiHal, plus the stubs.
struct Fixture {
  FakeRTOS rtos;
  FakeWifiHal hal;
  WifiManager wifi{hal};
  StubBleServer ble;
  StubHttpServer http;
  WifiService svc;

  Fixture()
      : svc(/*event_queue=*/nullptr, WifiService::Deps{wifi, ble, http}, WifiService::Config{}) {
    RTOS::set_instance(&rtos);
  }

  ~Fixture() { RTOS::set_instance(nullptr); }
};

} // namespace

// ---------------------------------------------------------------------------
// Saved-credential connect
// ---------------------------------------------------------------------------

TEST_CASE("has_saved_credentials forwards to WifiManager", "[go_wifi][creds]") {
  Fixture f;
  CHECK_FALSE(f.svc.has_saved_credentials());
  f.hal.saved_credentials_present = true;
  CHECK(f.svc.has_saved_credentials());
}

TEST_CASE("connect_with_saved_credentials sets STA mode, calls connect, arms 30s deadline",
          "[go_wifi][saved]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.rtos.set_now(1000);

  f.svc.connect_with_saved_credentials();

  CHECK(f.hal.last_mode_set == WifiMode::Sta);
  CHECK(f.hal.connect_calls == 1);
  CHECK(f.hal.last_ssid.empty()); // empty SSID = use NVS-saved
  CHECK(f.hal.last_persist == true);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 1000 + 30000);
  CHECK(f.svc.is_connecting());
  CHECK_FALSE(f.svc.is_online());
}

TEST_CASE("connect_with_saved_credentials applies static IP when provided",
          "[go_wifi][saved][static_ip]") {
  Fixture f;
  f.hal.saved_credentials_present = true;

  WifiStaticIpConfig ip{};
  ip.ip = 0x0100A8C0;
  ip.netmask = 0x00FFFFFF;
  f.svc.connect_with_saved_credentials(&ip);

  CHECK(f.hal.set_static_ip_calls == 1);
  CHECK(f.hal.last_static_ip.ip == 0x0100A8C0);
}

TEST_CASE("connect_with_saved_credentials clears static IP when nullptr",
          "[go_wifi][saved][static_ip]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.svc.connect_with_saved_credentials(nullptr);
  CHECK(f.hal.clear_static_ip_calls == 1);
  CHECK(f.hal.set_static_ip_calls == 0);
}

TEST_CASE("connect_with_saved_credentials with no NVS creds posts WifiDisconnected, no deadline",
          "[go_wifi][saved][disconnect]") {
  Fixture f;
  f.hal.saved_credentials_present = false;

  f.svc.connect_with_saved_credentials();

  // WifiManager::connect() returned NotFound — no driver call.
  CHECK(f.hal.connect_calls == 0);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);

  const Event *evt = f.rtos.first_event(EventType::WifiDisconnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_disconnect_reason == static_cast<uint8_t>(WifiDisconnectReason::no_ap_found));
}

// ---------------------------------------------------------------------------
// Factory-default fallback
// ---------------------------------------------------------------------------

TEST_CASE("try_default_fallback_credentials connects with airgradient/cleanair, persist=false, "
          "arms 15s deadline",
          "[go_wifi][fallback]") {
  Fixture f;
  f.rtos.set_now(5000);

  f.svc.try_default_fallback_credentials();

  CHECK(f.hal.last_mode_set == WifiMode::Sta);
  REQUIRE(f.hal.connect_calls == 1);
  CHECK(f.hal.last_ssid == "airgradient");
  CHECK(f.hal.last_password == "cleanair");
  CHECK(f.hal.last_persist == false);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 5000 + 15000);
}

TEST_CASE("fallback path does not request static IP", "[go_wifi][fallback]") {
  Fixture f;
  f.svc.try_default_fallback_credentials();
  CHECK(f.hal.set_static_ip_calls == 0);
  CHECK(f.hal.clear_static_ip_calls == 1);
}

// ---------------------------------------------------------------------------
// Callback adapters
// ---------------------------------------------------------------------------

TEST_CASE("on_got_ip updates online state, latches deadline-clear, posts WifiConnected",
          "[go_wifi][callbacks]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.svc.connect_with_saved_credentials();
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) != 0);

  REQUIRE(f.hal.got_ip_cb);
  f.hal.got_ip_cb(0x0100A8C0);

  CHECK(f.svc.is_online());
  CHECK(f.svc.has_been_online());
  CHECK(f.svc.ip() == 0x0100A8C0);
  CHECK(WifiServiceTestAccess::clear_pending(f.svc));

  const Event *evt = f.rtos.first_event(EventType::WifiConnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_ip == 0x0100A8C0);
}

TEST_CASE("on_disconnected clears online and posts WifiDisconnected with reason",
          "[go_wifi][callbacks]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x0100A8C0);
  f.rtos.captured.clear();

  // Drive WifiManager's disconnect path through raw ESP-IDF code 2
  // (WIFI_REASON_AUTH_EXPIRE) — maps to auth_failed.
  REQUIRE(f.hal.sta_disconnected_cb);
  f.hal.sta_disconnected_cb(/*WIFI_REASON_AUTH_EXPIRE*/ 2);

  CHECK_FALSE(f.svc.is_online());

  const Event *evt = f.rtos.first_event(EventType::WifiDisconnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_disconnect_reason == static_cast<uint8_t>(WifiDisconnectReason::auth_failed));
}

TEST_CASE("on_disconnected suppresses requested_by_user (no event)", "[go_wifi][callbacks]") {
  Fixture f;
  // disconnect() before any connect emits requested_by_user synthetically;
  // service must swallow it because the service's own teardown drives it.
  f.svc.connect_with_saved_credentials(); // arms internal state
  f.rtos.captured.clear();
  f.wifi.disconnect();
  CHECK_FALSE(f.rtos.has_event(EventType::WifiDisconnected));
}

// ---------------------------------------------------------------------------
// tick() — deadline lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("tick clears deadline after on_got_ip latches the pending flag", "[go_wifi][tick]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.rtos.set_now(1000);
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(WifiServiceTestAccess::clear_pending(f.svc));

  f.svc.tick(2000);

  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
  CHECK_FALSE(WifiServiceTestAccess::clear_pending(f.svc));
}

TEST_CASE("tick fires synthetic WifiDisconnected{connection_lost} on deadline expiry",
          "[go_wifi][tick]") {
  Fixture f;
  f.rtos.set_now(0);
  f.svc.try_default_fallback_credentials(); // arms 15s deadline at t=0
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) == 15000);
  f.rtos.captured.clear();

  f.svc.tick(14999);
  CHECK_FALSE(f.rtos.has_event(EventType::WifiDisconnected));

  f.svc.tick(15000);
  const Event *evt = f.rtos.first_event(EventType::WifiDisconnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_disconnect_reason == static_cast<uint8_t>(WifiDisconnectReason::connection_lost));
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
}

TEST_CASE("tick is a no-op when no deadline armed", "[go_wifi][tick]") {
  Fixture f;
  f.svc.tick(60000);
  CHECK(f.rtos.captured.empty());
}

// ---------------------------------------------------------------------------
// shutdown / clear_credentials
// ---------------------------------------------------------------------------

TEST_CASE("shutdown zeros the deadline and resets online state", "[go_wifi][shutdown]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(f.svc.is_online());
  REQUIRE(f.svc.has_been_online());

  f.svc.shutdown();

  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
  CHECK_FALSE(f.svc.is_online());
  CHECK_FALSE(f.svc.has_been_online());
  CHECK(f.hal.last_mode_set == WifiMode::Off);
}

TEST_CASE("clear_credentials wipes NVS and resets has_been_online",
          "[go_wifi][clear_credentials]") {
  Fixture f;
  f.hal.saved_credentials_present = true;
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(f.svc.has_been_online());

  f.svc.clear_credentials();

  CHECK(f.hal.clear_creds_calls == 1);
  CHECK_FALSE(f.svc.has_been_online());
}

// ---------------------------------------------------------------------------
// Provisioning event mapping + transport-switch latch
// ---------------------------------------------------------------------------

TEST_CASE("on_provisioning_event Connected maps disable_cloud and static_ip into payload",
          "[go_wifi][provisioning]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_transport(f.svc, ProvisioningTransport::WifiOnly);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  info.ip = 0xC0A80164; // 100.168.192.192 (any LE value)
  info.data.disable_cloud = true;
  info.data.static_ip.ip = 0x0100A8C0;
  info.data.static_ip.netmask = 0x00FFFFFF;

  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  const Event *evt = f.rtos.first_event(EventType::ProvisioningStateChanged);
  REQUIRE(evt != nullptr);
  CHECK(evt->prov.event == static_cast<uint8_t>(ProvisioningEvent::Connected));
  CHECK(evt->prov.transport == static_cast<uint8_t>(ProvisioningTransport::WifiOnly));
  CHECK(evt->prov.ip == 0xC0A80164);
  CHECK(evt->prov.disable_cloud == true);
  CHECK(evt->prov.static_ip.ip == 0x0100A8C0);
  CHECK(evt->prov.static_ip.netmask == 0x00FFFFFF);
}

TEST_CASE("on_provisioning_event swallows Stopped during transport switch",
          "[go_wifi][provisioning][switch]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_switching_transport(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::ProductRequested;

  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK_FALSE(f.rtos.has_event(EventType::ProvisioningStateChanged));
}

TEST_CASE("on_provisioning_event forwards Started during transport switch",
          "[go_wifi][provisioning][switch]") {
  // Started on the new transport must reach the orchestrator so it can
  // update the UI even though the latch is held.
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_switching_transport(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Started;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK(f.rtos.has_event(EventType::ProvisioningStateChanged));
}

TEST_CASE("on_provisioning_event Stopped outside switch forwards normally",
          "[go_wifi][provisioning]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::ProductRequested;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  const Event *evt = f.rtos.first_event(EventType::ProvisioningStateChanged);
  REQUIRE(evt != nullptr);
  CHECK(evt->prov.event == static_cast<uint8_t>(ProvisioningEvent::Stopped));
}

TEST_CASE("start_provisioning calls _wifi.disconnect and zeros the deadline",
          "[go_wifi][provisioning]") {
  Fixture f;
  // Arm a deadline via a saved-creds connect.
  f.hal.saved_credentials_present = true;
  f.rtos.set_now(1000);
  f.svc.connect_with_saved_credentials();
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) != 0);
  const int hal_disconnects_before = f.hal.disconnect_calls;

  f.svc.start_provisioning(ProvisioningTransport::BleOnly);

  CHECK(f.hal.disconnect_calls > hal_disconnects_before);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
}

// ---------------------------------------------------------------------------
// Bug fixes from on-device CP2.4 trace
// ---------------------------------------------------------------------------

TEST_CASE("on_provisioning_event Connected latches online and has_been_online",
          "[go_wifi][provisioning][bugfix]") {
  // Provisioning owns the WifiManager callback slot during a session, so
  // on_got_ip never fires on WifiService. The Connected event must mirror
  // those state updates itself, otherwise has_been_online() stays false
  // and the orchestrator falls back to Portable on the subsequent Stopped.
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  REQUIRE_FALSE(f.svc.is_online());
  REQUIRE_FALSE(f.svc.has_been_online());

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  info.ip = 0x0100A8C0;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK(f.svc.is_online());
  CHECK(f.svc.has_been_online());
  CHECK(f.svc.ip() == 0x0100A8C0);
}

TEST_CASE("on_provisioning_event Started during a transport switch reports the destination",
          "[go_wifi][provisioning][bugfix]") {
  // Regression for the on-device trace: while switching BLE -> Wi-Fi, the
  // Started event fired by the inner _prov->start() must reflect the
  // destination transport. Previously _transport was updated only after
  // start() returned, so Started carried the stale source transport.
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_transport(f.svc, ProvisioningTransport::WifiOnly);
  WifiServiceTestAccess::set_switching_transport(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Started;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  const Event *evt = f.rtos.first_event(EventType::ProvisioningStateChanged);
  REQUIRE(evt != nullptr);
  CHECK(evt->prov.event == static_cast<uint8_t>(ProvisioningEvent::Started));
  CHECK(evt->prov.transport == static_cast<uint8_t>(ProvisioningTransport::WifiOnly));
}
