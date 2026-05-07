/**
 * AirGradient Go — GoApp test stubs
 *
 * Provides observable stub implementations for services and free functions
 * that GoApp calls.  Follows the same pattern as go_orchestrator_stubs.cpp.
 *
 * The stubs replace the real .cpp files at link time — only go_app.cpp
 * (the code under test) is compiled from production sources.
 */

#include "go_ble.h"
#include "go_buzzer.h"
#include "go_display.h"
#include "go_input.h"
#include "go_orchestrator.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_storage.h"
#include "go_ulp.h"
#include "gps/gps_service.h"

#include <algorithm>
#include <cstring>

// ============================================================================
// test_spy — observable state written by stubs, read by test assertions
// ============================================================================

namespace test_spy {

// --- RTC state ---
RtcAppState rtc_state{};
RtcDisplaySnapshot rtc_snapshot{};
bool rtc_snapshot_valid = false;

// --- SensorManager ---
int warmup_step_count = 0;
Measures measures_to_return{};

// --- SensorProducer ---
bool sensor_started = false;
bool sensor_stopped = false;

// --- GpsService ---
bool gps_started = false;
bool gps_stopped = false;
bool gps_idle_called = false;
GpsData gps_data_to_return{};

// --- InputService ---
bool input_started = false;
bool input_stopped = false;

// --- StorageService ---
bool cache_measurement_called = false;
MeasuresAGo last_cached_measurement{};
bool route_started = false;
uint32_t route_session_id = 0;
bool route_point_appended = false;
RoutePoint last_route_point{};
bool route_ended = false;
bool route_file_open = false;
bool cache_backed_up = false;
bool cache_restored = false;
bool storage_init_called = false;

// --- PowerService ---
bool bms_polled = false;
bool state_saved = false;
RtcAppState last_saved_state{};
PowerSnapshot snapshot_to_return{};
PowerService::SleepDecision sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
bool enter_sleep_called = false;
uint32_t enter_sleep_duration_ms = 0;
bool should_hold_pm_result = false;

// --- BleService ---
bool ble_init_called = false;

// --- Orchestrator ---
bool orchestrator_init_called = false;
bool orchestrator_run_called = false;
WakeCause orchestrator_wake_cause = WakeCause::PowerOn;
BootHandoff orchestrator_handoff{};

// --- BmsDevice ---
float bms_battery_pct = -1.0f;

void reset() {
  rtc_state = RtcAppState{};
  rtc_snapshot = RtcDisplaySnapshot{};
  rtc_snapshot_valid = false;

  warmup_step_count = 0;
  measures_to_return = Measures{};

  sensor_started = false;
  sensor_stopped = false;

  gps_started = false;
  gps_stopped = false;
  gps_idle_called = false;
  gps_data_to_return = GpsData{};

  input_started = false;
  input_stopped = false;

  cache_measurement_called = false;
  last_cached_measurement = MeasuresAGo{};
  route_started = false;
  route_session_id = 0;
  route_point_appended = false;
  last_route_point = RoutePoint{};
  route_ended = false;
  route_file_open = false;
  cache_backed_up = false;
  cache_restored = false;
  storage_init_called = false;

  bms_polled = false;
  state_saved = false;
  last_saved_state = RtcAppState{};
  snapshot_to_return = PowerSnapshot{};
  sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  enter_sleep_called = false;
  enter_sleep_duration_ms = 0;
  should_hold_pm_result = false;

  ble_init_called = false;

  orchestrator_init_called = false;
  orchestrator_run_called = false;
  orchestrator_wake_cause = WakeCause::PowerOn;
  orchestrator_handoff = BootHandoff{};

  bms_battery_pct = -1.0f;

  DisplayService::spy_deep_sleep_called = false;
  DisplayService::spy_update_count = 0;
}

} // namespace test_spy

// ============================================================================
// SensorManager stubs
// ============================================================================

#include "services/sensor_manager.h"

SensorManager::SensorManager(Sensors &sensors) : _sensors(sensors) {}
SensorManager::~SensorManager() = default;

void SensorManager::warmup_step() { test_spy::warmup_step_count++; }

Measures SensorManager::start_measures(int /*iterations*/, SensorGroup /*groups*/) {
  return test_spy::measures_to_return;
}

// ============================================================================
// SensorProducer stubs
// ============================================================================

SensorProducer::SensorProducer(SensorManager &manager, RtosQueueHandle event_queue,
                               const Config &config)
    : _manager(manager), _event_queue(event_queue), _config(config) {}

bool SensorProducer::start() {
  test_spy::sensor_started = true;
  return true;
}

void SensorProducer::stop() { test_spy::sensor_stopped = true; }

void SensorProducer::request_measurement(uint8_t /*iterations*/, SensorGroup /*groups*/) {}
void SensorProducer::request_co2_calibration() {}
void SensorProducer::request_prepare() {}

// ============================================================================
// GpsService stubs
// ============================================================================

GpsService::GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config)
    : _driver(driver), _event_queue(event_queue), _config(config) {}

GpsService::~GpsService() = default;

bool GpsService::start() {
  test_spy::gps_started = true;
  return true;
}

void GpsService::stop() { test_spy::gps_stopped = true; }

void GpsService::stop_and_idle_gnss() {}

void GpsService::idle_gnss() { test_spy::gps_idle_called = true; }

GpsData GpsService::get_latest_fix() const { return GpsData{}; }

void GpsService::set_posting_interval_ms(int /*interval_ms*/) {}

void GpsService::set_aiding_data(const GpsAidingData & /*data*/) {}

GpsData gps_read_once(GpsDriver & /*driver*/, int /*baud_rate*/, uint32_t /*timeout_ms*/,
                      const volatile bool & /*abort*/) {
  return test_spy::gps_data_to_return;
}

// ============================================================================
// InputService stubs
// ============================================================================

InputService::InputService(CapTouchSensor &touch, const gpio::Hal &gpio,
                           RtosQueueHandle event_queue, const Config &config)
    : _touch(touch), _gpio(gpio), _event_queue(event_queue), _config(config) {}

InputService::~InputService() = default;

bool InputService::start() {
  test_spy::input_started = true;
  return true;
}

void InputService::stop() { test_spy::input_stopped = true; }

// ============================================================================
// BuzzerService stubs
// ============================================================================

BuzzerService::BuzzerService(const Config &config) : _config(config) {}
BuzzerService::~BuzzerService() = default;
bool BuzzerService::init() { return true; }
bool BuzzerService::start() { return true; }
void BuzzerService::play(const Note * /*notes*/, size_t /*count*/) {}
void BuzzerService::beep(uint32_t /*duration_ms*/) {}
void BuzzerService::stop() {}
void BuzzerService::_task_entry(void * /*arg*/) {}
void BuzzerService::_run() {}
void BuzzerService::_set_freq(uint32_t /*freq_hz*/) {}

// ============================================================================
// StorageService stubs
// ============================================================================

StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}

bool StorageService::init() {
  test_spy::storage_init_called = true;
  return true;
}

void StorageService::cache_measurement(const MeasuresAGo &m) {
  test_spy::cache_measurement_called = true;
  test_spy::last_cached_measurement = m;
}

uint16_t StorageService::read_cached_field(CacheField /*field*/, float * /*out*/,
                                           uint16_t /*max_count*/) const {
  return 0;
}

uint16_t StorageService::read_cache(MeasuresAGo * /*out*/, uint16_t /*max_count*/) const {
  return 0;
}

uint16_t StorageService::cached_count() const { return 0; }

void StorageService::backup_cache() const { test_spy::cache_backed_up = true; }

void StorageService::restore_cache() { test_spy::cache_restored = true; }

void StorageService::clear_cache() {}

bool StorageService::start_route(uint32_t session_id) {
  test_spy::route_started = true;
  test_spy::route_file_open = true;
  test_spy::route_session_id = session_id;
  return true;
}

bool StorageService::append_route_point(const RoutePoint &point) {
  test_spy::route_point_appended = true;
  test_spy::last_route_point = point;
  return true;
}

void StorageService::end_route() {
  if (!test_spy::route_file_open)
    return;
  test_spy::route_file_open = false;
  test_spy::route_ended = true;
}

bool StorageService::is_route_active() const { return test_spy::route_file_open; }

uint32_t StorageService::current_route_point_count() const { return 0; }

bool StorageService::delete_route(uint32_t /*session_id*/) { return true; }

uint32_t StorageService::current_route_session_id() const { return test_spy::route_session_id; }

bool StorageService::clear_routes() { return true; }

uint16_t StorageService::session_count() const { return 0; }
uint16_t StorageService::list_sessions(uint32_t * /*out*/, uint16_t /*max*/) const { return 0; }
uint32_t StorageService::get_session_point_count(uint32_t /*id*/) const { return 0; }
uint16_t StorageService::read_route_points(uint32_t /*id*/, uint32_t /*off*/, RoutePoint * /*out*/,
                                           uint16_t /*cnt*/) const {
  return 0;
}
time_t StorageService::get_session_start_time(uint32_t /*id*/) const { return 0; }
uint32_t StorageService::total_capacity_kb() const { return 0; }
uint32_t StorageService::used_kb() const { return 0; }

// ============================================================================
// PowerService stubs
// ============================================================================

PowerService::PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

PowerSnapshot PowerService::poll_bms() {
  test_spy::bms_polled = true;
  return test_spy::snapshot_to_return;
}

bool PowerService::poll_charging_status(BmsChargingState &state) {
  state = test_spy::snapshot_to_return.charging_status;
  return true;
}

bool PowerService::poll_status(BmsStatus &status) {
  status = test_spy::snapshot_to_return.charger_status;
  return true;
}

bool PowerService::reset_watchdog() { return true; }

void PowerService::shutdown() {}

void PowerService::save_state(const RtcAppState &state) {
  test_spy::state_saved = true;
  test_spy::last_saved_state = state;
}

RtcAppState PowerService::load_state() const { return test_spy::rtc_state; }

PowerService::SleepDecision PowerService::decide_sleep(const GoSettings & /*settings*/,
                                                       LockState /*lock_state*/,
                                                       OperatingMode /*mode*/,
                                                       uint32_t /*awake_ms*/) const {
  return test_spy::sleep_decision_to_return;
}

bool PowerService::should_hold_pm_sensor(uint32_t /*sleep_duration_ms*/) const {
  return test_spy::should_hold_pm_result;
}

bool PowerService::should_sleep_pm_sensor(uint32_t /*measure_interval_ms*/) const { return false; }

void PowerService::set_pm_power(bool /*on*/) {}

void PowerService::enter_sleep(uint32_t sleep_duration_ms) {
  test_spy::enter_sleep_called = true;
  test_spy::enter_sleep_duration_ms = sleep_duration_ms;
}

WakeCause PowerService::get_wake_cause() { return WakeCause::PowerOn; }

bool PowerService::is_fast_path_wake(WakeCause cause, const RtcAppState &state) {
  return cause == WakeCause::Timer && state.lock_state == LockState::Locked;
}

void PowerService::release_sleep_gpio_holds(int /*pin_pm_power*/) {}

void PowerService::init_ext_watchdog() {}

void PowerService::reset_ext_watchdog() {}

void PowerService::configure_wake_sources(uint32_t /*timer_ms*/) {}

bool PowerService::sync_pmid_mode(BmsPowerSource /*power_source*/) { return true; }

// ============================================================================
// Free functions from go_power.h
// ============================================================================

RtcAppState load_rtc_app_state() { return test_spy::rtc_state; }

// ============================================================================
// BleService stubs
// ============================================================================

BleService::BleService(RtosQueueHandle /*event_queue*/, StorageService &storage)
    : _event_queue(nullptr), _storage(storage) {}

bool BleService::init(const char * /*serial*/) {
  test_spy::ble_init_called = true;
  return true;
}

void BleService::deinit() {}
bool BleService::is_initialized() const { return false; }
bool BleService::is_connected() const { return false; }
void BleService::notify_measures(const MeasuresAGo & /*m*/, const GpsData & /*gps*/,
                                 time_t /*ts*/) {}
void BleService::update_status(const PowerSnapshot & /*power*/, const GpsData & /*gps*/,
                               bool /*tracking*/, uint32_t /*session_id*/) {}
void BleService::update_config(const GoSettings & /*settings*/) {}
void BleService::notify_config(const GoSettings & /*settings*/) {}
void BleService::notify_command_progress(BleCommand /*cmd*/) {}
void BleService::notify_command_result(BleCommand /*cmd*/, bool /*success*/,
                                       const char * /*error*/) {}
bool BleService::delete_all_bonds() { return true; }
size_t BleService::take_pending_config_write(uint8_t * /*buf*/, size_t /*buf_size*/) { return 0; }
size_t BleService::take_pending_history_write(uint8_t * /*buf*/, size_t /*buf_size*/) { return 0; }
void BleService::handle_history_list() {}
void BleService::handle_history_start(uint32_t /*session_id*/) {}
void BleService::handle_history_fill(const uint32_t * /*indices*/, size_t /*count*/) {}
void BleService::handle_history_end() {}
void BleService::handle_history_delete(uint32_t /*session_id*/) {}
void BleService::notify_history_error(const char * /*err*/) {}
BleConfigDecodeResult BleService::decode_config_write(const uint8_t * /*buf*/, size_t /*len*/,
                                                      GoSettings & /*settings*/) {
  return {};
}
BleHistoryDecodeResult BleService::decode_history_write(const uint8_t * /*buf*/, size_t /*len*/) {
  return {};
}

// BleService private methods (never called in app tests)
void BleService::on_connect(uint16_t /*handle*/) {}
void BleService::on_disconnect(uint16_t /*handle*/, int /*reason*/) {}
void BleService::on_config_write(const uint8_t * /*data*/, size_t /*len*/) {}
void BleService::on_history_write(const uint8_t * /*data*/, size_t /*len*/) {}
void BleService::on_passkey_request(uint32_t /*passkey*/) {}
bool BleService::send_history_cbor(const uint8_t * /*data*/, size_t /*len*/) { return false; }
bool BleService::send_history_binary(uint16_t /*idx*/, const uint8_t * /*data*/, size_t /*len*/) {
  return false;
}
void BleService::route_point_to_wire(const RoutePoint & /*point*/, uint8_t * /*out*/) {}
size_t BleService::encode_measures(uint8_t * /*buf*/, size_t /*sz*/, const MeasuresAGo & /*m*/,
                                   const GpsData & /*gps*/, time_t /*ts*/) {
  return 0;
}
size_t BleService::encode_status(uint8_t * /*buf*/, size_t /*sz*/, const PowerSnapshot & /*p*/,
                                 const GpsData & /*g*/, bool /*t*/, uint32_t /*s*/) {
  return 0;
}
size_t BleService::encode_config(uint8_t * /*buf*/, size_t /*sz*/, const GoSettings & /*s*/) {
  return 0;
}
const char *BleService::charging_state_to_str(BmsChargingState /*s*/) { return "unknown"; }
const char *BleService::gps_mode_to_str(GpsMode /*m*/) { return "tracking"; }
const char *BleService::operating_mode_to_str(OperatingMode /*m*/) { return "offline"; }

// ============================================================================
// UIManager stubs
// ============================================================================

#include "go_ui.h"

UIManager::UIManager(const Config & /*config*/) {}

UIActionResult UIManager::handle_input(InputSource /*source*/, InputType /*type*/) { return {}; }

DisplayValues UIManager::build_values(const BuildContext & /*ctx*/) const { return {}; }

void UIManager::set_screen(Screen /*screen*/) {}
Screen UIManager::current_screen() const { return Screen::Home; }
bool UIManager::is_on_menu_screen() const { return false; }
void UIManager::show_snackbar(const char * /*text*/) {}
void UIManager::clear_expired_snackbar(uint32_t /*now_ms*/) {}
void UIManager::sync_settings(const GoSettings & /*settings*/) {}
void UIManager::apply_to_settings(GoSettings & /*settings*/) const {}
void UIManager::reset_to_home() {}
void UIManager::show_pairing_passkey(uint32_t /*passkey*/) {}
void UIManager::dismiss_pairing_passkey() {}

// ============================================================================
// Orchestrator stubs
// ============================================================================

Orchestrator::Orchestrator(RtosQueueHandle event_queue, const Services &services,
                           GoSettings settings, ConfigStore &config_store, const char *serial)
    : _event_queue(event_queue), _svc(services), _settings(settings), _config_store(config_store),
      _serial(serial) {}

void Orchestrator::init(WakeCause cause, const BootHandoff &handoff) {
  test_spy::orchestrator_init_called = true;
  test_spy::orchestrator_wake_cause = cause;
  test_spy::orchestrator_handoff = handoff;
}

void Orchestrator::run() { test_spy::orchestrator_run_called = true; }

// ============================================================================
// ULP stubs
// ============================================================================

void ulp_wdt_start() {}
void ulp_wdt_stop() {}

// ============================================================================
// Private stubs that must link
// ============================================================================

void SensorProducer::task_entry(void * /*arg*/) {}
void SensorProducer::run() {}

void GpsService::task_entry(void * /*arg*/) {}
void GpsService::run() {}
void GpsService::update_latest_fix(const GpsData & /*data*/) {}
void GpsService::post_fix_event() {}
void GpsService::sync_system_clock(const GpsTimestamp & /*ts*/) {}

void InputService::process_touch_interrupt() {}
void InputService::process_button_event(InputSource /*source*/, uint64_t /*timestamp_ms*/) {}
void InputService::check_pending_long_press() {}
uint32_t InputService::compute_queue_timeout_ms() const { return UINT32_MAX; }
void InputService::post_input_event(InputSource /*source*/, InputType /*type*/) {}
void InputService::cap_int_isr(void * /*arg*/) {}
void InputService::button_power_isr(void * /*arg*/) {}
void InputService::button_boot_isr(void * /*arg*/) {}
void InputService::task_entry(void * /*arg*/) {}
void InputService::run() {}
int InputService::pin_for_button_index(int /*idx*/) const { return -1; }

bool StorageService::ensure_route_dir() const { return true; }

// Orchestrator private stubs (never called in GoApp tests)
void Orchestrator::dispatch(const Event & /*event*/) {}
void Orchestrator::on_sensor_data(const MeasuresAGo & /*data*/) {}
void Orchestrator::on_gps_fix(const GpsData & /*data*/) {}
void Orchestrator::on_input(const InputEventData & /*input*/) {}
void Orchestrator::on_co2_calibration_done(Co2CalibrationResult /*result*/) {}
void Orchestrator::on_ble_connected() {}
void Orchestrator::on_ble_disconnected() {}
void Orchestrator::on_ble_config_write() {}
void Orchestrator::on_ble_history_write() {}
void Orchestrator::on_ble_pairing_request(uint32_t /*passkey*/) {}
void Orchestrator::on_ble_auth_complete() {}
void Orchestrator::lock() {}
void Orchestrator::unlock() {}
void Orchestrator::start_tracking() {}
void Orchestrator::stop_tracking() {}
void Orchestrator::change_mode(OperatingMode /*new_mode*/) {}
void Orchestrator::apply_settings_change() {}
bool Orchestrator::clear_data() { return true; }
bool Orchestrator::factory_reset() { return true; }
void Orchestrator::save_tag(uint8_t /*tag_index*/, const char * /*tag_label*/) {}
void Orchestrator::shutdown() {}
uint32_t Orchestrator::compute_queue_timeout_ms() const { return UINT32_MAX; }
void Orchestrator::check_timers() {}
void Orchestrator::on_bms_timer() {}
void Orchestrator::on_bms_status_timer() {}
void Orchestrator::on_inactivity_timeout() {}
void Orchestrator::reschedule_sensor_timer(const GoSettings & /*previous_settings*/) {}
void Orchestrator::update_display() {}
void Orchestrator::request_background_display_update() {}
BuildContext Orchestrator::build_context() const {
  static Measures dummy_measures{};
  return BuildContext{.sensor_data = dummy_measures};
}
void Orchestrator::try_enter_sleep() {}
void Orchestrator::prepare_for_sleep(uint32_t /*sleep_duration_ms*/) {}
void Orchestrator::init_ble_if_portable() {}
bool Orchestrator::is_gps_active() const { return false; }
void Orchestrator::deactivate_gps() {}
uint32_t Orchestrator::generate_session_id() { return 10000; }
RtcAppState Orchestrator::snapshot_state() const { return RtcAppState{}; }
