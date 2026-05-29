/**
 * AirGradient Go — WifiService implementation
 *
 * Stationary Wi-Fi mechanics. Mode policy lives in the orchestrator.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_wifi.h"

#include "ag_log.h"
#include "common.h"
#include "go_events.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"

#include <cstring>

static constexpr const char *TAG = "WifiService";

// Saved-credentials policy. Bounded retry; backoffs use WifiManager defaults.
static constexpr uint8_t STATIONARY_MAX_RETRY_COUNT = 5;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WifiService::WifiService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _http(deps.http), _cfg(cfg),
      _prov(new ProvisioningManager()) {
  _install_wifi_callbacks();
  // Single set_on_event install kept for the lifetime of the service.
  _prov->set_on_event([this](const ProvisioningEventInfo &info) { _on_provisioning_event(info); });
}

WifiService::~WifiService() {
  _detach_wifi_callbacks();
  delete _prov;
}

// ---------------------------------------------------------------------------
// Credential queries
// ---------------------------------------------------------------------------

bool WifiService::has_saved_credentials() const { return _wifi.has_saved_credentials(); }

// ---------------------------------------------------------------------------
// Connection attempts
// ---------------------------------------------------------------------------

void WifiService::connect_with_saved_credentials(const WifiStaticIpConfig *static_ip) {
  _reset_deadline();
  _reset_online_latches();

  if (static_ip != nullptr && static_ip->ip != 0) {
    _wifi.set_static_ip(*static_ip);
  } else {
    _wifi.clear_static_ip();
  }

  _wifi.set_mode(WifiMode::Sta);

  WifiStaConfig sta{}; // ssid empty = NVS-saved credentials path
  sta.max_retry_count = STATIONARY_MAX_RETRY_COUNT;

  const WifiStatus status = _wifi.connect(sta);
  if (status == WifiStatus::NotFound) {
    // No persisted creds. Disconnect-policy router takes it from here.
    AG_LOGW(TAG, "saved-creds connect: no NVS credentials");
    _post_wifi_disconnected(WifiDisconnectReason::no_ap_found);
    return;
  }
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "saved-creds connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }

  _arm_deadline(_cfg.initial_connect_window_ms);
}

void WifiService::try_default_fallback_credentials() {
  _reset_deadline();
  _reset_online_latches();
  _wifi.clear_static_ip();
  _wifi.set_mode(WifiMode::Sta);

  WifiStaConfig sta{};
  std::strncpy(sta.ssid, _cfg.fallback_ssid, sizeof(sta.ssid) - 1);
  std::strncpy(sta.password, _cfg.fallback_password, sizeof(sta.password) - 1);
  sta.max_retry_count = 0; // single-shot; no retry on fallback
  sta.persist = false;     // never write fallback creds to NVS

  const WifiStatus status = _wifi.connect(sta);
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "fallback connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }

  _arm_deadline(_cfg.fallback_connect_window_ms);
}

// ---------------------------------------------------------------------------
// Provisioning
// ---------------------------------------------------------------------------

void WifiService::start_provisioning(ProvisioningTransport transport) {
  AG_LOGI(TAG, "start_provisioning(%u)", static_cast<unsigned>(transport));
  log_heap(TAG, "wifi.start_provisioning:enter");

  // Cancel any in-flight STA connect so prov.start() can take over the
  // Wi-Fi callbacks cleanly. Idempotent when STA is already disconnected.
  _wifi.disconnect();

  // Zero deadlines before prov takes the callback slots — guards against a
  // stale tick() firing a synthetic disconnect mid-provisioning after a
  // fast credential-class fail.
  _reset_deadline();

  if (_start_provisioning_internal(transport)) {
    _provisioning_active = true;
    _transport = transport;
  }
  log_heap(TAG, "wifi.start_provisioning:exit");
}

void WifiService::switch_provisioning_transport() {
  if (!_provisioning_active) {
    AG_LOGW(TAG, "switch_provisioning_transport: not active");
    return;
  }

  const ProvisioningTransport prev = _transport;
  const ProvisioningTransport other = (prev == ProvisioningTransport::BleOnly)
                                          ? ProvisioningTransport::WifiOnly
                                          : ProvisioningTransport::BleOnly;
  AG_LOGI(TAG, "switching transport %u -> %u", static_cast<unsigned>(prev),
          static_cast<unsigned>(other));
  log_heap(TAG, "wifi.switch_transport:enter");

  _switching_transport = true;
  // Keep HTTP server up across the switch — saves a bind/unbind cycle.
  _prov->stop(/*stop_http_server=*/false);
  log_heap(TAG, "wifi.switch_transport:after-teardown");

  // Update _transport before _prov->start so the Started event fired
  // during the inner start carries the destination transport, not the
  // (stale) source. Rolled back below if start fails.
  _transport = other;
  const bool ok = _start_provisioning_internal(other);
  _switching_transport = false;
  log_heap(TAG, "wifi.switch_transport:after-restart");

  if (!ok) {
    AG_LOGE(TAG, "switch start failed; synthesizing Stopped");
    _transport = prev; // rollback
    _provisioning_active = false;
    // Synthesize a Stopped event so the orchestrator's "Stopped before
    // online -> change_mode(Portable)" rule rescues the user.
    ProvisioningEventInfo synth{};
    synth.event = ProvisioningEvent::Stopped;
    synth.stop_reason = ProvisioningStopReason::ProductRequested;
    _post_provisioning_event(synth);
  }
}

void WifiService::stop_provisioning() {
  if (!_provisioning_active) {
    return;
  }
  AG_LOGI(TAG, "stop_provisioning");
  log_heap(TAG, "wifi.stop_provisioning:enter");
  // Blocks for ~1.5 s when called after Connected (component-side hold).
  _prov->stop();
  _provisioning_active = false;
  // ProvisioningManager::stop() cleared the WifiManager callback slots;
  // restore ours so post-online disconnects and shutdown work.
  _install_wifi_callbacks();
  log_heap(TAG, "wifi.stop_provisioning:exit");
}

bool WifiService::_start_provisioning_internal(ProvisioningTransport transport) {
  ProvisioningConfig config{};
  if (_cfg.ap_ssid != nullptr) {
    std::strncpy(config.ap.ssid, _cfg.ap_ssid, sizeof(config.ap.ssid) - 1);
  }
  if (_cfg.ap_password != nullptr) {
    std::strncpy(config.ap.password, _cfg.ap_password, sizeof(config.ap.password) - 1);
  }
  config.ap.channel = _cfg.ap_channel;

  config.ble.device_name = _cfg.ble_device_name;
  config.ble.manufacturer_data = _cfg.ble_manufacturer_data;
  config.ble.model_name = _cfg.ble_model_name;
  config.ble.serial_number = _cfg.ble_serial_number;
  config.ble.firmware_version = _cfg.ble_firmware_version;
  config.ble.io_capability = AgBleIoCapability::NO_INPUT_NO_OUTPUT;
  config.ble.auth_flags = _cfg.ble_auth_flags;

  config.transport = transport;
  config.overall_timeout_ms = 0; // disabled per AGo policy

  return _prov->start(_wifi, _ble, _http, config);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WifiService::shutdown() {
  _reset_deadline();
  if (_provisioning_active) {
    _prov->stop();
    _provisioning_active = false;
  }
  _wifi.disconnect();
  _wifi.set_mode(WifiMode::Off);
  _detach_wifi_callbacks();
  _reset_online_latches();
}

void WifiService::clear_credentials() {
  _wifi.clear_saved_credentials();
  _reset_online_latches();
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool WifiService::is_online() const { return _online.load(); }

bool WifiService::is_connecting() const {
  return _initial_connect_deadline_ms != 0 && !_online.load();
}

bool WifiService::is_provisioning() const { return _provisioning_active; }

ProvisioningTransport WifiService::current_transport() const { return _transport; }

uint32_t WifiService::ip() const { return _ip.load(); }

int WifiService::rssi() const { return _rssi.load(); }

WifiDisconnectReason WifiService::last_disconnect_reason() const {
  return static_cast<WifiDisconnectReason>(_last_disconnect_reason.load());
}

bool WifiService::has_been_online() const { return _has_been_online.load(); }

// ---------------------------------------------------------------------------
// Timer integration
// ---------------------------------------------------------------------------

uint32_t WifiService::next_deadline_ms() const { return _initial_connect_deadline_ms; }

void WifiService::tick(uint32_t now_ms) {
  // Latched clear from on_got_ip (kept off the deadline writer thread).
  if (_clear_deadline_pending.exchange(false)) {
    _initial_connect_deadline_ms = 0;
  }

  if (_initial_connect_deadline_ms != 0 && now_ms >= _initial_connect_deadline_ms) {
    _initial_connect_deadline_ms = 0;
    AG_LOGW(TAG, "initial connect window expired");
    _post_wifi_disconnected(WifiDisconnectReason::connection_lost);
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void WifiService::_install_wifi_callbacks() {
  _wifi.set_on_got_ip([this](uint32_t ip) { _on_got_ip(ip); });
  _wifi.set_on_disconnected([this](WifiDisconnectReason r) { _on_disconnected(r); });
}

void WifiService::_detach_wifi_callbacks() {
  _wifi.set_on_got_ip(nullptr);
  _wifi.set_on_disconnected(nullptr);
}

void WifiService::_on_got_ip(uint32_t ip) {
  AG_LOGI(TAG, "wifi online: ip=0x%08x", static_cast<unsigned>(ip));
  _ip.store(ip);
  _online.store(true);
  _has_been_online.store(true);
  // Defer deadline clear to tick() so deadline stays single-writer.
  _clear_deadline_pending.store(true);
  _rssi.store(_wifi.status_snapshot().rssi);

  Event evt{};
  evt.type = EventType::WifiConnected;
  evt.wifi_ip = ip;
  RTOS::queue_send(_event_queue, &evt);
}

void WifiService::_on_disconnected(WifiDisconnectReason reason) {
  AG_LOGI(TAG, "wifi disconnected: reason=%u", static_cast<unsigned>(reason));
  _online.store(false);
  _last_disconnect_reason.store(static_cast<uint8_t>(reason));
  if (reason == WifiDisconnectReason::requested_by_user) {
    return; // synthetic teardown reason; not an external failure
  }
  _post_wifi_disconnected(reason);
}

void WifiService::_reset_deadline() {
  _initial_connect_deadline_ms = 0;
  _clear_deadline_pending.store(false);
}

void WifiService::_arm_deadline(uint32_t window_ms) {
  _initial_connect_deadline_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + window_ms;
}

void WifiService::_reset_online_latches() {
  _online.store(false);
  _has_been_online.store(false);
  _ip.store(0);
  _rssi.store(WIFI_RSSI_INVALID);
}

void WifiService::_post_wifi_disconnected(WifiDisconnectReason reason) {
  Event evt{};
  evt.type = EventType::WifiDisconnected;
  evt.wifi_disconnect_reason = static_cast<uint8_t>(reason);
  RTOS::queue_send(_event_queue, &evt);
}

void WifiService::_on_provisioning_event(const ProvisioningEventInfo &info) {
  // During a transport switch, swallow the intermediate Stopped so the
  // orchestrator does not interpret it as a user abort.
  if (_switching_transport && info.event == ProvisioningEvent::Stopped) {
    return;
  }

  // Connected is the online transition while provisioning owns the
  // Wi-Fi callback slot — mirror what on_got_ip would have latched so
  // has_been_online() reads true when the orchestrator processes the
  // subsequent Stopped from stop_provisioning's teardown.
  if (info.event == ProvisioningEvent::Connected) {
    _ip.store(info.ip);
    _online.store(true);
    _has_been_online.store(true);
    _rssi.store(_wifi.status_snapshot().rssi);
  }

  _post_provisioning_event(info);
}

void WifiService::_post_provisioning_event(const ProvisioningEventInfo &info) {
  Event evt{};
  evt.type = EventType::ProvisioningStateChanged;
  evt.prov.event = static_cast<uint8_t>(info.event);
  evt.prov.transport = static_cast<uint8_t>(_transport);
  evt.prov.stop_reason = static_cast<uint8_t>(info.stop_reason);
  evt.prov.ip = info.ip;
  evt.prov.disable_cloud = info.data.disable_cloud;
  evt.prov.static_ip = info.data.static_ip;
  RTOS::queue_send(_event_queue, &evt);
}
