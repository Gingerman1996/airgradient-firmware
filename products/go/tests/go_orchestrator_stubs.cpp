/**
 * AirGradient Go — Orchestrator test stubs
 *
 * Provides observable stub implementations for all services the orchestrator
 * calls.  Each stub records its calls into the test_spy namespace so tests
 * can assert on orchestrator–service interactions without constructing real
 * hardware-backed service objects.
 *
 * The stubs replace the real .cpp files at link time — only the orchestrator
 * .cpp (the code under test) and UIManager .cpp (real, pure-logic) are
 * compiled from production sources.
 */

#include "go_ble.h"
#include "go_buzzer.h"
#include "go_display.h"
#include "go_led.h"
#include "gps/gps_service.h"
#include "go_input.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_storage.h"
#include "go_ulp.h"

#include <algorithm>
#include <cstring>

// ============================================================================
// test_spy — observable state written by stubs, read by test assertions
// ============================================================================

namespace test_spy {

// --- SensorProducer ---
bool sensor_started = false;
bool sensor_stopped = false;
bool measurement_requested = false;
uint8_t last_iterations = 0;
SensorGroup last_groups = SensorGroup::None;
bool co2_calibration_requested = false;
bool prepare_requested = false;

// --- GpsService ---
bool gps_started = false;
bool gps_stopped = false;
bool gps_stop_and_idle_called = false;
bool gps_idle_called = false;
int gps_posting_interval_ms = 0;
bool gps_aiding_set = false;
GpsAidingData gps_aiding_data{};

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
// Models the real _route_file != nullptr state.  Survives reset() so that
// end_route() after reset() still behaves like the real implementation.
bool route_file_open = false;
bool cache_backed_up = false;
bool cache_restored = false;
bool cache_cleared = false;
bool routes_cleared = false;
bool clear_routes_result = true;

// --- BleService ---
bool ble_init_called = false;
bool ble_deinit_called = false;
bool ble_initialized = false;
bool ble_connected = false;
bool ble_notify_measures_called = false;
bool ble_update_status_called = false;
bool ble_update_config_called = false;
bool ble_notify_config_called = false;
bool ble_notify_command_progress_called = false;
BleCommand ble_progress_command = BleCommand::Unknown;
bool ble_notify_command_result_called = false;
BleCommand ble_last_command = BleCommand::Unknown;
bool ble_last_command_success = false;
bool ble_delete_all_bonds_called = false;
bool ble_delete_all_bonds_result = true;
bool ble_history_list_called = false;
bool ble_history_start_called = false;
uint32_t ble_history_start_session = 0;
bool ble_history_fill_called = false;
bool ble_history_end_called = false;
bool ble_history_delete_called = false;
uint32_t ble_history_delete_session = 0;
bool ble_notify_history_error_called = false;
const char *ble_last_history_error = nullptr;
size_t ble_pending_config_len = 0;
BleConfigDecodeResult ble_config_decode_result{};
bool ble_decode_updates_settings = false;
GoSettings ble_decoded_settings{};
BleHistoryDecodeResult ble_history_decode_result{};

// --- PowerService ---
bool bms_polled = false;
bool shutdown_called = false;
bool state_saved = false;
RtcAppState last_saved_state{};
RtcAppState state_to_load{};        // tests set this before init(Button)
PowerSnapshot snapshot_to_return{}; // tests set this before poll_bms
PowerService::SleepType sleep_type_to_return = PowerService::SleepType::None;
bool pm_power_set = false;
bool pm_power_on = false;

// --- BuzzerService ---
bool buzzer_played = false;
size_t buzzer_last_note_count = 0;

// --- LedService ---
bool led_flashed = false;
LedService::Led led_last_led = LedService::Led::Select;
uint8_t led_last_value = 0;
uint32_t led_last_duration_ms = 0;

void reset() {
  sensor_started = false;
  sensor_stopped = false;
  measurement_requested = false;
  last_iterations = 0;
  co2_calibration_requested = false;
  prepare_requested = false;

  gps_started = false;
  gps_stopped = false;
  gps_stop_and_idle_called = false;
  gps_idle_called = false;
  gps_posting_interval_ms = 0;
  gps_aiding_set = false;
  gps_aiding_data = GpsAidingData{};

  input_started = false;
  input_stopped = false;

  cache_measurement_called = false;
  last_cached_measurement = MeasuresAGo{};
  route_started = false;
  route_session_id = 0;
  route_point_appended = false;
  last_route_point = RoutePoint{};
  route_ended = false;
  cache_backed_up = false;
  cache_restored = false;
  cache_cleared = false;
  routes_cleared = false;
  clear_routes_result = true;

  ble_init_called = false;
  ble_deinit_called = false;
  ble_initialized = false;
  ble_connected = false;
  ble_notify_measures_called = false;
  ble_update_status_called = false;
  ble_update_config_called = false;
  ble_notify_config_called = false;
  ble_notify_command_progress_called = false;
  ble_progress_command = BleCommand::Unknown;
  ble_notify_command_result_called = false;
  ble_last_command = BleCommand::Unknown;
  ble_last_command_success = false;
  ble_delete_all_bonds_called = false;
  ble_delete_all_bonds_result = true;
  ble_history_list_called = false;
  ble_history_start_called = false;
  ble_history_start_session = 0;
  ble_history_fill_called = false;
  ble_history_end_called = false;
  ble_history_delete_called = false;
  ble_history_delete_session = 0;
  ble_notify_history_error_called = false;
  ble_last_history_error = nullptr;
  ble_pending_config_len = 0;
  ble_config_decode_result = BleConfigDecodeResult{};
  ble_decode_updates_settings = false;
  ble_decoded_settings = GoSettings{};
  ble_history_decode_result = BleHistoryDecodeResult{};

  bms_polled = false;
  shutdown_called = false;
  state_saved = false;
  last_saved_state = RtcAppState{};
  state_to_load = RtcAppState{};
  snapshot_to_return = PowerSnapshot{};
  sleep_type_to_return = PowerService::SleepType::None;
  pm_power_set = false;
  pm_power_on = false;

  buzzer_played = false;
  buzzer_last_note_count = 0;

  led_flashed = false;
  led_last_led = LedService::Led::Select;
  led_last_value = 0;
  led_last_duration_ms = 0;

  DisplayService::spy_deep_sleep_called = false;
  DisplayService::spy_update_count = 0;
}

} // namespace test_spy

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

void SensorProducer::request_measurement(uint8_t iterations, SensorGroup groups) {
  test_spy::measurement_requested = true;
  test_spy::last_iterations = iterations;
  test_spy::last_groups = groups;
}

void SensorProducer::request_co2_calibration() { test_spy::co2_calibration_requested = true; }

void SensorProducer::request_prepare() { test_spy::prepare_requested = true; }

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

void GpsService::stop() {
  test_spy::gps_stopped = true;
  _driver.end();
}

void GpsService::stop_and_idle_gnss() { test_spy::gps_stop_and_idle_called = true; }

void GpsService::idle_gnss() { test_spy::gps_idle_called = true; }

GpsData GpsService::get_latest_fix() const { return GpsData{}; }

void GpsService::set_posting_interval_ms(int interval_ms) {
  test_spy::gps_posting_interval_ms = interval_ms;
}

void GpsService::set_aiding_data(const GpsAidingData &data) {
  test_spy::gps_aiding_set = true;
  test_spy::gps_aiding_data = data;
}

GpsData gps_read_once(GpsDriver & /*driver*/, int /*baud_rate*/, uint32_t /*timeout_ms*/,
                      const volatile bool & /*abort*/) {
  return GpsData{};
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
void BuzzerService::play(const Note * /*notes*/, size_t count) {
  test_spy::buzzer_played = true;
  test_spy::buzzer_last_note_count = count;
}
void BuzzerService::beep(uint32_t /*duration_ms*/) {
  test_spy::buzzer_played = true;
  test_spy::buzzer_last_note_count = 1;
}
void BuzzerService::stop() {}
void BuzzerService::_task_entry(void * /*arg*/) {}
void BuzzerService::_run() {}
void BuzzerService::_set_freq(uint32_t /*freq_hz*/) {}

// ============================================================================
// LP5036 + LedService stubs
// ============================================================================

LP5036::LP5036(LedI2cBusHandle bus, const Config &config) : _config(config), _bus(bus) {}
LP5036::~LP5036() = default;
bool LP5036::init() { return true; }
bool LP5036::set_channel(uint8_t /*channel*/, uint8_t /*value*/) { return true; }
bool LP5036::set_rgb(uint8_t /*b_channel*/, uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {
  return true;
}
bool LP5036::_write_reg(uint8_t /*reg*/, uint8_t /*value*/) { return true; }
bool LP5036::_write_block(uint8_t /*reg*/, const uint8_t * /*data*/, size_t /*len*/) {
  return true;
}

LedService::LedService(const Config &config) : _config(config) {}
LedService::~LedService() = default;
bool LedService::init() { return true; }
bool LedService::start() { return true; }
void LedService::flash_white(Led led, uint8_t value, uint32_t duration_ms) {
  test_spy::led_flashed = true;
  test_spy::led_last_led = led;
  test_spy::led_last_value = value;
  test_spy::led_last_duration_ms = duration_ms;
}
void LedService::off(Led /*led*/) {}
void LedService::all_off() {}
void LedService::_task_entry(void * /*arg*/) {}
void LedService::_run() {}
void LedService::_map(Led /*led*/, uint8_t & /*b_ch*/, uint8_t & /*g_ch*/, uint8_t & /*r_ch*/) {}
void LedService::_set_led_rgb(Led /*led*/, uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {}

// ============================================================================
// StorageService stubs
// ============================================================================

StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}

bool StorageService::init() { return true; }

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

void StorageService::clear_cache() { test_spy::cache_cleared = true; }

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
  if (!test_spy::route_file_open) {
    return; // no-op, same as real end_route() when _route_file == nullptr
  }
  test_spy::route_file_open = false;
  test_spy::route_ended = true;
}

bool StorageService::is_route_active() const { return test_spy::route_file_open; }

uint32_t StorageService::current_route_point_count() const { return 0; }

bool StorageService::delete_route(uint32_t /*session_id*/) { return true; }

uint32_t StorageService::current_route_session_id() const { return test_spy::route_session_id; }

bool StorageService::clear_routes() {
  test_spy::routes_cleared = true;
  return test_spy::clear_routes_result;
}

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
  if (status.charging_state == BmsChargingState::Unknown) {
    status.charging_state = test_spy::snapshot_to_return.charging_status;
  }
  return true;
}

bool PowerService::reset_watchdog() { return true; }

bool PowerService::set_charge_current_ma(uint16_t /*current_ma*/) { return true; }

void PowerService::shutdown() { test_spy::shutdown_called = true; }

void PowerService::save_state(const RtcAppState &state) {
  test_spy::state_saved = true;
  test_spy::last_saved_state = state;
}

RtcAppState PowerService::load_state() const { return test_spy::state_to_load; }

PowerService::SleepDecision PowerService::decide_sleep(const GoSettings & /*settings*/,
                                                       LockState /*lock_state*/,
                                                       OperatingMode /*mode*/,
                                                       uint32_t /*awake_ms*/) const {
  return {test_spy::sleep_type_to_return, 10000};
}

bool PowerService::should_hold_pm_sensor(uint32_t sleep_duration_ms) const {
  return _config.pin_pm_power >= 0 && sleep_duration_ms < _config.sensor_hold_max_sleep_ms;
}

bool PowerService::should_sleep_pm_sensor(uint32_t measure_interval_ms) const {
  return _config.pin_pm_power >= 0 && measure_interval_ms >= _config.pm_sleep_threshold_ms;
}

void PowerService::set_pm_power(bool on) {
  test_spy::pm_power_set = true;
  test_spy::pm_power_on = on;
}

void PowerService::enter_sleep(uint32_t /*sleep_duration_ms*/) {}

WakeCause PowerService::get_wake_cause() { return WakeCause::PowerOn; }

bool PowerService::is_fast_path_wake(WakeCause /*cause*/, const RtcAppState & /*state*/) {
  return false;
}

void PowerService::release_sleep_gpio_holds(int /*pin_pm_power*/) {}

void PowerService::init_ext_watchdog() {}

void PowerService::reset_ext_watchdog() {}

// ============================================================================
// Free functions from go_power.h
// ============================================================================

RtcAppState load_rtc_app_state() { return test_spy::state_to_load; }

// ============================================================================
// Private stubs that are never called but must link
// ============================================================================

void PowerService::configure_wake_sources(uint32_t /*timer_ms*/) {}

bool StorageService::ensure_route_dir() const { return true; }

// SensorProducer private methods
void SensorProducer::task_entry(void * /*arg*/) {}
void SensorProducer::run() {}
void SensorProducer::handle_calibration() {}
void SensorProducer::handle_prepare() {}
void SensorProducer::handle_measurement(uint32_t /*notify_value*/) {}
void SensorProducer::handle_sampler_tick() {}

// GpsService private methods
void GpsService::task_entry(void * /*arg*/) {}
void GpsService::run() {}
void GpsService::update_latest_fix(const GpsData & /*data*/) {}
void GpsService::post_fix_event() {}
void GpsService::sync_system_clock(const GpsTimestamp & /*ts*/) {}

// InputService private methods
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

// ============================================================================
// BleService stubs
// ============================================================================

BleService::BleService(RtosQueueHandle /*event_queue*/, StorageService &storage)
    : _event_queue(nullptr), _storage(storage) {}

bool BleService::init(const char * /*serial*/) {
  test_spy::ble_init_called = true;
  test_spy::ble_initialized = true;
  return true;
}

void BleService::deinit() {
  test_spy::ble_deinit_called = true;
  test_spy::ble_initialized = false;
  test_spy::ble_connected = false;
}

bool BleService::is_initialized() const { return test_spy::ble_initialized; }

bool BleService::is_connected() const { return test_spy::ble_connected; }

void BleService::notify_measures(const MeasuresAGo & /*m*/, const GpsData & /*gps*/,
                                 time_t /*ts*/) {
  test_spy::ble_notify_measures_called = true;
}

void BleService::update_status(const PowerSnapshot & /*power*/, const GpsData & /*gps*/,
                               bool /*tracking*/, uint32_t /*session_id*/) {
  test_spy::ble_update_status_called = true;
}

void BleService::update_config(const GoSettings & /*settings*/) {
  test_spy::ble_update_config_called = true;
}

void BleService::notify_config(const GoSettings & /*settings*/) {
  test_spy::ble_notify_config_called = true;
}

void BleService::notify_command_progress(BleCommand cmd) {
  test_spy::ble_notify_command_progress_called = true;
  test_spy::ble_progress_command = cmd;
}

void BleService::notify_command_result(BleCommand cmd, bool success, const char * /*error*/) {
  test_spy::ble_notify_command_result_called = true;
  test_spy::ble_last_command = cmd;
  test_spy::ble_last_command_success = success;
}

bool BleService::delete_all_bonds() {
  test_spy::ble_delete_all_bonds_called = true;
  return test_spy::ble_delete_all_bonds_result;
}

size_t BleService::take_pending_config_write(uint8_t * /*buf*/, size_t /*buf_size*/) {
  return test_spy::ble_pending_config_len;
}

size_t BleService::take_pending_history_write(uint8_t * /*buf*/, size_t /*buf_size*/) { return 0; }

void BleService::handle_history_list() { test_spy::ble_history_list_called = true; }

void BleService::handle_history_start(uint32_t session_id) {
  test_spy::ble_history_start_called = true;
  test_spy::ble_history_start_session = session_id;
}

void BleService::handle_history_fill(const uint32_t * /*indices*/, size_t /*count*/) {
  test_spy::ble_history_fill_called = true;
}

void BleService::handle_history_end() { test_spy::ble_history_end_called = true; }

void BleService::handle_history_delete(uint32_t session_id) {
  test_spy::ble_history_delete_called = true;
  test_spy::ble_history_delete_session = session_id;
}

void BleService::notify_history_error(const char *err) {
  test_spy::ble_notify_history_error_called = true;
  test_spy::ble_last_history_error = err;
}

BleConfigDecodeResult BleService::decode_config_write(const uint8_t * /*buf*/, size_t /*len*/,
                                                      GoSettings &settings) {
  if (test_spy::ble_decode_updates_settings) {
    settings = test_spy::ble_decoded_settings;
  }
  return test_spy::ble_config_decode_result;
}

BleHistoryDecodeResult BleService::decode_history_write(const uint8_t * /*buf*/, size_t /*len*/) {
  return test_spy::ble_history_decode_result;
}

// BleService private methods (never called in orchestrator tests)
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
// ULP stubs (go_ulp.h — LP Core not available on host)
// ============================================================================

void ulp_wdt_start() {}
void ulp_wdt_stop() {}

// StorageService read methods (declared in go_storage.h, stubbed for linker)
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
