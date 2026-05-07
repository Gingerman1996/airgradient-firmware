/**
 * AirGradient Go — Orchestrator implementation
 *
 * Central event loop, event dispatch, timer management, state transitions,
 * display updates, and sleep cycle management.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_orchestrator.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <ctime>
#include <utility>

#include "ag_log.h"
#include "common.h"
#include "go_ble_protocol.h"
#include "rtos.h"

static constexpr const char *TAG = "Orchestrator";

static constexpr uint8_t SESSION_ID_LENGTH = 5;

// ---------------------------------------------------------------------------
// Helper: initialize MeasuresAGo to invalid sentinels
// ---------------------------------------------------------------------------

static MeasuresAGo make_invalid_measures() {
  MeasuresAGo m{};
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
  m.co2.co2 = MeasuresInvalid::CO2;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  m.power.battery_voltage = MeasuresInvalid::VOLT;
  m.power.charging_voltage = MeasuresInvalid::VOLT;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;
  m.pressure.altitude = MeasuresInvalid::ALTITUDE;
  return m;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Orchestrator::Orchestrator(RtosQueueHandle event_queue, const Services &services,
                           GoSettings settings, ConfigStore &config_store, const char *serial)
    : _event_queue(event_queue), _svc(services), _settings(std::move(settings)),
      _config_store(config_store), _serial(serial), _cached_measures(make_invalid_measures()) {}

// ---------------------------------------------------------------------------
// Boot initialization
// ---------------------------------------------------------------------------

void Orchestrator::init(WakeCause cause, const BootHandoff &handoff) {
  AG_LOGI(TAG,
          "init: wake_cause=%d display_painted=%d measurement_done=%d "
          "lock=%d",
          static_cast<int>(cause), static_cast<int>(handoff.display_painted),
          static_cast<int>(handoff.measurement_completed),
          static_cast<int>(handoff.initial_lock_state));

  // Mode always comes from persisted settings (NVS) — single source of truth
  _mode = _settings.operating_mode;

  // --- Restore RTC state for wake-from-sleep cases ---
  if (cause != WakeCause::PowerOn) {
    RtcAppState state = _svc.power_service.load_state();
    _behavior = state.behavior;
    _gps_enabled = state.gps_enabled;
    _tracking_active = state.tracking_active;
    _tracking_session_id = state.tracking_session_id;
  }

  // --- Apply initial lock state ---
  if (handoff.initial_lock_state == LockState::Unlocked) {
    if (handoff.display_painted) {
      // Display already shows unlocked UI — set state directly.
      // Do NOT call unlock() which would trigger update_display().
      _lock_state = LockState::Unlocked;
      _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.show_snackbar("Unlocked");
      uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.clear_expired_snackbar(now_ms);
      _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
    } else {
      // Display not yet showing unlocked UI — unlock triggers
      // update_display() which will paint the unlocked frame.
      unlock();
    }
  }

  // --- Seed cached measures from boot ---
  // Fresh measurements take priority over stale RTC snapshot.
  if (handoff.fast_path_measures != nullptr) {
    _cached_measures = *handoff.fast_path_measures;
  } else if (handoff.display_snapshot != nullptr) {
    _cached_measures.co2.co2 = handoff.display_snapshot->co2_ppm;
    _cached_measures.pm_a.pm_25 = handoff.display_snapshot->pm25_ugm3;
    _cached_measures.temp_hum_a.temperature = handoff.display_snapshot->temperature_c;
    _cached_measures.temp_hum_a.humidity = handoff.display_snapshot->humidity_pct;
    _cached_measures.tvoc_nox.tvoc_index = handoff.display_snapshot->tvoc_index;
    _cached_measures.tvoc_nox.nox_index = handoff.display_snapshot->nox_index;
    _cached_measures.pressure.pressure = handoff.display_snapshot->pressure_hpa;
    _cached_measures.pressure.altitude = handoff.display_snapshot->altitude_m;
  }

  // --- Mark first measurement done if boot already measured ---
  if (handoff.measurement_completed) {
    _first_measurement_done = true;
  }

  // --- Resume route if tracking was active before sleep ---
  if (_tracking_active) {
    _svc.storage_service.start_route(_tracking_session_id);
  }

  // --- Common tail ---
  _svc.ui_manager.sync_settings(_settings);

  if (!handoff.measurement_completed) {
    _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  }

  _latest_power = _svc.power_service.poll_bms();

  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_measurement_ms = now;
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now;
  _last_ext_wdt_ms = now;
  if (handoff.initial_lock_state != LockState::Unlocked || !handoff.display_painted) {
    _last_input_ms = now;
  }

  init_ble_if_portable();
}

// ---------------------------------------------------------------------------
// Main event loop
// ---------------------------------------------------------------------------

void Orchestrator::run() {
  AG_LOGI(TAG, "run: entering main event loop");

  while (true) {
    // Sleep check: enter sleep when locked and first measurement is done
    if (_lock_state == LockState::Locked && _first_measurement_done) {
      try_enter_sleep(); // Returns only when sleep conditions are not met
    }

    uint32_t timeout = compute_queue_timeout_ms();
    Event evt{};
    if (RTOS::queue_receive(_event_queue, &evt, timeout)) {
      dispatch(evt);
    }

    check_timers();
  }
}

// ---------------------------------------------------------------------------
// Timer management
// ---------------------------------------------------------------------------

uint32_t Orchestrator::compute_queue_timeout_ms() const {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t next = UINT32_MAX;

  // Sensor timer deadline
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  {
    uint32_t deadline = _last_measurement_ms + interval_ms;
    uint32_t remaining = deadline - now;
    next = std::min(next, remaining);
  }

  // BMS full-telemetry deadline
  uint32_t bms_remaining = (_last_bms_poll_ms + BMS_POLL_INTERVAL_MS) - now;
  next = std::min(next, bms_remaining);

  // BMS fast charging-status deadline
  uint32_t bms_status_remaining = (_last_bms_status_poll_ms + BMS_STATUS_POLL_INTERVAL_MS) - now;
  next = std::min(next, bms_status_remaining);

  // Inactivity deadline (only when unlocked and auto-lock enabled)
  if (_lock_state == LockState::Unlocked && _settings.auto_lock_seconds > 0) {
    uint32_t inact_interval = static_cast<uint32_t>(_settings.auto_lock_seconds) * 1000;
    uint32_t inact_remaining = (_last_input_ms + inact_interval) - now;
    next = std::min(next, inact_remaining);
  }

  // PM pre-wake deadline (not Offline, interval above threshold, prepare not yet sent)
  bool pm_sleep_eligible =
      _mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval_ms);
  if (pm_sleep_eligible && !_pm_prepare_sent) {
    uint32_t measure_deadline = _last_measurement_ms + interval_ms;
    uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
    uint32_t pm_remaining = prepare_deadline - now;
    next = std::min(next, pm_remaining);
  }

  // Snackbar refresh deadline
  if (_snackbar_refresh_deadline_ms != 0) {
    uint32_t sb_remaining = _snackbar_refresh_deadline_ms - now;
    next = std::min(next, sb_remaining);
  }

  // If any deadline already passed, the unsigned subtraction yields a large
  // number — clamp to 0 so check_timers() fires immediately.
  if (next > MAX_REASONABLE_TIMEOUT_MS) {
    next = 0;
  }

  return next;
}

void Orchestrator::check_timers() {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());

  // --- PM pre-wake timer (fires warmup_duration before next measurement) ---
  uint32_t interval = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  bool pm_sleep_eligible =
      _mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval);
  if (pm_sleep_eligible && !_pm_prepare_sent) {
    uint32_t measure_deadline = _last_measurement_ms + interval;
    uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
    if ((now - prepare_deadline) < MAX_REASONABLE_TIMEOUT_MS) {
      AG_LOGI(TAG, "PM pre-wake: powering on and requesting prepare");
      _svc.power_service.set_pm_power(true);
      _svc.sensor_producer.request_prepare();
      _pm_prepare_sent = true;
    }
  }

  // --- Sensor timer (single) ---
  if ((now - _last_measurement_ms) >= interval) {
    _svc.sensor_producer.request_measurement(1, SensorGroup::All);
    _last_measurement_ms = now;
    _pm_prepare_sent = false;
  }

  // --- BMS full telemetry timer ---
  if ((now - _last_bms_poll_ms) >= BMS_POLL_INTERVAL_MS) {
    on_bms_timer();
  }

  // --- BMS fast charging-status timer (between full polls) ---
  if ((now - _last_bms_status_poll_ms) >= BMS_STATUS_POLL_INTERVAL_MS) {
    on_bms_status_timer();
  }

  // --- External watchdog timer ---
  if ((now - _last_ext_wdt_ms) >= EXT_WDT_INTERVAL_MS) {
    _svc.power_service.reset_ext_watchdog();
    _last_ext_wdt_ms = now;
  }

  // --- Snackbar refresh timer ---
  // Ensures a follow-up display update after the snackbar expires so it is
  // visually cleared even when no other events trigger update_display().
  if (_snackbar_refresh_deadline_ms != 0 &&
      (now - _snackbar_refresh_deadline_ms) < MAX_REASONABLE_TIMEOUT_MS) {
    _snackbar_refresh_deadline_ms = 0;
    request_background_display_update();
  }

  // --- Auto-lock timer ---
  if (_lock_state == LockState::Unlocked && _settings.auto_lock_seconds > 0) {
    uint32_t inact_interval = static_cast<uint32_t>(_settings.auto_lock_seconds) * 1000;
    if ((now - _last_input_ms) >= inact_interval) {
      on_inactivity_timeout();
    }
  }
}

void Orchestrator::on_bms_timer() {
  _latest_power = _svc.power_service.poll_bms();
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now; // Full poll subsumes the fast status check.

  // Update BLE status characteristic with latest power/GPS/tracking state
  if (_svc.ble_service.is_initialized()) {
    _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                   _tracking_session_id);
  }
}

void Orchestrator::on_bms_status_timer() {
  BmsStatus status{};
  if (_svc.power_service.poll_status(status)) {
    bool was_charging = is_bms_charging(_latest_power.charging_status);
    const BmsPowerSource previous_power_source = _latest_power.charger_status.power_source;
    _latest_power.charging_status = status.charging_state;
    _latest_power.charger_status = status;
    bool now_charging = is_bms_charging(status.charging_state);
    if (was_charging != now_charging || previous_power_source != status.power_source) {
      AG_LOGI(
          TAG, "charger status changed: charge=%s -> %s source=%s -> %s",
          was_charging ? "charging" : "not charging", now_charging ? "charging" : "not charging",
          bms_power_source_str(previous_power_source), bms_power_source_str(status.power_source));
      request_background_display_update();
    }
  }

  _last_bms_status_poll_ms = static_cast<uint32_t>(RTOS::get_time_ms());
}

void Orchestrator::on_inactivity_timeout() { lock(); }

void Orchestrator::reschedule_sensor_timer(const GoSettings &previous_settings) {
  if (previous_settings.measure_interval_seconds == _settings.measure_interval_seconds) {
    return;
  }
  _last_measurement_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Reconcile PM power with the new interval.  Idempotent GPIO writes.
  uint32_t new_interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline &&
      _svc.power_service.should_sleep_pm_sensor(new_interval_ms)) {
    _svc.power_service.set_pm_power(false);
  } else {
    _svc.power_service.set_pm_power(true);
  }
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

void Orchestrator::dispatch(const Event &event) {
  switch (event.type) {
  case EventType::SensorDataReady:
    on_sensor_data(event.sensor_data);
    break;
  case EventType::GpsFixUpdate:
    on_gps_fix(event.gps_data);
    break;
  case EventType::InputPress:
    on_input(event.input);
    break;

  // BLE events
  case EventType::BleConnected:
    on_ble_connected();
    break;
  case EventType::BleDisconnected:
    on_ble_disconnected();
    break;
  case EventType::BleConfigWrite:
    on_ble_config_write();
    break;
  case EventType::BleHistoryWrite:
    on_ble_history_write();
    break;
  case EventType::BlePairingRequest:
    on_ble_pairing_request(event.ble_passkey);
    break;
  case EventType::BleAuthComplete:
    on_ble_auth_complete();
    break;

  // Calibration events
  case EventType::Co2CalibrationDone:
    on_co2_calibration_done(static_cast<Co2CalibrationResult>(event.co2_cal_result));
    break;

  // UI action events (reserved for future programmatic triggers)
  case EventType::UserStartTracking:
    start_tracking();
    break;
  case EventType::UserStopTracking:
    stop_tracking();
    break;
  case EventType::UserChangeMode:
    change_mode(event.mode_change);
    break;
  case EventType::UserToggleGps:
    _gps_enabled = event.gps_enabled;
    break;
  case EventType::SettingsChanged:
    apply_settings_change();
    break;
  case EventType::ClearData:
    clear_data();
    break;
  case EventType::SaveTag:
    save_tag(event.tag_index, nullptr);
    break;

  // System events
  case EventType::InactivityTimeout:
    on_inactivity_timeout();
    break;
  case EventType::MeasurementTimer:
    check_timers(); // legacy event: just re-check all timers
    break;
  case EventType::WakeFromSleep:
    break; // wake handled in init()
  }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void Orchestrator::on_sensor_data(const MeasuresAGo &data) {
  // Always overwrite all fields — single interval, no group-based gating.
  _cached_measures.pm_a = data.pm_a;
  _cached_measures.co2 = data.co2;
  _cached_measures.temp_hum_a = data.temp_hum_a;
  _cached_measures.tvoc_nox = data.tvoc_nox;
  _cached_measures.pressure = data.pressure;
  _cached_measures.power = data.power;

  _first_measurement_done = true;

  AG_LOGI(TAG, "sensor_data: temp=%.1f hum=%.1f pm25=%.1f co2=%d tvoc_raw=%d nox_raw=%d pres=%.1f",
          _cached_measures.temp_hum_a.temperature, _cached_measures.temp_hum_a.humidity,
          _cached_measures.pm_a.pm_25, _cached_measures.co2.co2, _cached_measures.tvoc_nox.tvoc_raw,
          _cached_measures.tvoc_nox.nox_raw, _cached_measures.pressure.pressure);

  _svc.storage_service.cache_measurement(_cached_measures);

  if (_tracking_active) {
    RoutePoint point{};
    point.timestamp = time(nullptr);
    point.gps = _latest_gps;
    point.sensors = _cached_measures;
    point.battery_percentage = _latest_power.battery_percentage;
    _svc.storage_service.append_route_point(point);
  }

  // Update BLE measures characteristic (always for READ; notifies when connected)
  _svc.ble_service.notify_measures(_cached_measures, _latest_gps, time(nullptr));

  // Power off PM sensor after measurement when interval justifies power-cycling.
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval_ms)) {
    _svc.power_service.set_pm_power(false);
  }

  request_background_display_update();
}

void Orchestrator::on_co2_calibration_done(Co2CalibrationResult result) {
  switch (result) {
  case Co2CalibrationResult::Success:
    AG_LOGI(TAG, "CO2 calibration succeeded");
    _svc.ui_manager.show_snackbar("CO2 cal. done");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, true);
    break;
  case Co2CalibrationResult::Unsupported:
    AG_LOGW(TAG, "CO2 calibration unsupported by sensor");
    _svc.ui_manager.show_snackbar("CO2 cal. unsupported");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, false,
                                           BLE_VAL_ERR_UNSUPPORTED);
    break;
  case Co2CalibrationResult::Failed:
    AG_LOGW(TAG, "CO2 calibration failed");
    _svc.ui_manager.show_snackbar("CO2 cal. failed");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, false,
                                           BLE_VAL_ERR_CALIBRATION_FAILED);
    break;
  }
  update_display();
}

void Orchestrator::on_gps_fix(const GpsData &data) {
  if (!is_gps_active()) {
    return; // GPS disabled in settings; ignore
  }
  _latest_gps = data;

  AG_LOGI(TAG, "gps_fix: lat=%.6f lon=%.6f alt=%.1f fix=%d sat=%d hdop=%.1f",
          data.position.latitude, data.position.longitude, data.altitude_m,
          static_cast<int>(data.fix.fix_type), data.fix.satellite_count, data.fix.hdop);
}

static const char *input_source_str(InputSource s) {
  switch (s) {
  case InputSource::TouchUp:
    return "TouchUp";
  case InputSource::TouchDown:
    return "TouchDown";
  case InputSource::TouchEnter:
    return "TouchEnter";
  case InputSource::ButtonPower:
    return "BtnPower";
  case InputSource::ButtonBoot:
    return "BtnBoot";
  default:
    return "Unknown";
  }
}

void Orchestrator::on_input(const InputEventData &input) {
  AG_LOGI(TAG, "input: source=%s type=%s lock=%s", input_source_str(input.source),
          input.type == InputType::ShortPress ? "short" : "long",
          _lock_state == LockState::Locked ? "locked" : "unlocked");

  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Shutdown: long press on power button (any lock state)
  if (input.source == InputSource::ButtonPower && input.type == InputType::LongPress) {
    shutdown();
    return;
  }

  // Factory reset: long press on boot button
  if (input.source == InputSource::ButtonBoot && input.type == InputType::LongPress) {
    if (factory_reset()) {
      AG_LOGI(TAG, "Rebooting in 2s");
      RTOS::delay_ms(2000);
      reboot();
    }
    return;
  }

  // Lock toggle: power button short press
  if (input.source == InputSource::ButtonPower && input.type == InputType::ShortPress) {
    if (_lock_state == LockState::Locked) {
      unlock();
    } else {
      lock();
    }
    return;
  }

  // Locked: show hint on touch input, ignore otherwise
  if (_lock_state == LockState::Locked) {
    if (input.source == InputSource::TouchUp || input.source == InputSource::TouchDown ||
        input.source == InputSource::TouchEnter) {
      _svc.ui_manager.show_snackbar("Unlock First");
      update_display();
    }
    return;
  }

  // Unlocked: forward to UI Manager
  UIActionResult result = _svc.ui_manager.handle_input(input.source, input.type);

  switch (result.action) {
  case UIAction::StartTracking:
    start_tracking();
    break;
  case UIAction::StopTracking:
    stop_tracking();
    break;
  case UIAction::ChangeMode:
    change_mode(result.new_mode);
    break;
  case UIAction::SettingsChanged:
    apply_settings_change();
    break;
  case UIAction::ClearData:
    clear_data();
    break;
  case UIAction::CalibrateCo2:
    _svc.sensor_producer.request_co2_calibration();
    _svc.ui_manager.show_snackbar("Calibrating CO2...");
    break;
  case UIAction::SaveTag:
    save_tag(result.tag_index, result.tag_label);
    break;
  case UIAction::None:
    break;
  }

  update_display();
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

void Orchestrator::lock() {
  AG_LOGI(TAG, "lock");
  _svc.ui_manager.show_snackbar("Locked");
  _lock_state = LockState::Locked;
  _svc.ui_manager.reset_to_home();
  update_display();
}

void Orchestrator::unlock() {
  AG_LOGI(TAG, "unlock");
  _svc.ui_manager.show_snackbar("Unlocked");
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  update_display();
}

void Orchestrator::start_tracking() {
  if (_tracking_active) {
    return;
  }

  AG_LOGI(TAG, "start_tracking");
  const bool was_gps_active = is_gps_active();
  _tracking_session_id = generate_session_id();
  _tracking_active = true;
  _behavior = Behavior::Tracking;

  if (!was_gps_active && is_gps_active()) {
    _svc.gps_service.start();
  }

  _svc.storage_service.start_route(_tracking_session_id);
  char msg[48];
  (void)snprintf(msg, sizeof(msg), "Tracking start = %05" PRIu32, _tracking_session_id);
  _svc.ui_manager.show_snackbar(msg);
  update_display();
}

void Orchestrator::stop_tracking() {
  if (!_tracking_active) {
    return;
  }

  AG_LOGI(TAG, "stop_tracking");
  const bool was_gps_active = is_gps_active();
  const uint32_t ended_session_id = _tracking_session_id;
  _svc.storage_service.end_route();
  _tracking_active = false;
  _tracking_session_id = 0;
  _behavior = Behavior::Idle;

  if (was_gps_active && !is_gps_active()) {
    deactivate_gps();
  }

  char msg[48];
  (void)snprintf(msg, sizeof(msg), "Tracking stop = %05" PRIu32, ended_session_id);
  _svc.ui_manager.show_snackbar(msg);
  update_display();
}

void Orchestrator::change_mode(OperatingMode new_mode) {
  OperatingMode old_mode = _mode;
  AG_LOGI(TAG, "change_mode: %d -> %d", static_cast<int>(old_mode), static_cast<int>(new_mode));
  _mode = new_mode;
  _settings.operating_mode = new_mode;
  save_go_settings(_config_store, _settings);

  // BLE lifecycle follows Portable mode
  if (old_mode == OperatingMode::Portable && new_mode != OperatingMode::Portable) {
    _svc.ble_service.deinit();
  } else if (old_mode != OperatingMode::Portable && new_mode == OperatingMode::Portable) {
    init_ble_if_portable();
  }

  // Future: enable/disable WiFi, HTTP server based on mode

  // Ensure PM sensor is powered on — covers mode change away from Portable
  // while PM was power-cycled off.  Idempotent if already on.
  _svc.power_service.set_pm_power(true);

  _svc.ui_manager.show_snackbar("Mode changed");
  update_display();
}

void Orchestrator::apply_settings_change() {
  const bool was_gps_active = is_gps_active();
  const GoSettings previous_settings = _settings;
  _svc.ui_manager.apply_to_settings(_settings);
  save_go_settings(_config_store, _settings);

  // Propagate runtime changes to services
  reschedule_sensor_timer(previous_settings);
  _svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000);

  const bool is_gps_active_now = is_gps_active();
  if (!was_gps_active && is_gps_active_now) {
    _svc.gps_service.start();
  } else if (was_gps_active && !is_gps_active_now) {
    deactivate_gps();
  }

  _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);

  // Notify connected BLE client of config change
  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.notify_config(_settings);
    _svc.ble_service.update_config(_settings);
  }
}

bool Orchestrator::clear_data() {
  if (_tracking_active) {
    stop_tracking();
  }

  _svc.storage_service.clear_cache();
  const bool routes_cleared = _svc.storage_service.clear_routes();

  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                   _tracking_session_id);
  }

  _svc.ui_manager.show_snackbar(routes_cleared ? "Data cleared" : "Data clear failed");
  update_display();

  return routes_cleared;
}

bool Orchestrator::factory_reset() {
  AG_LOGI(TAG, "factory_reset");

  // Erase temporary cache data and delete all persisted route files.
  const bool data_cleared = clear_data();

  const GoSettings defaults{};

  // Overwrite persisted product settings with their default values.
  const bool settings_saved = save_go_settings(_config_store, defaults);

  // Delete all stored BLE bond information.
  const bool bonds_cleared = _svc.ble_service.delete_all_bonds();

  const bool success = data_cleared && settings_saved && bonds_cleared;

  if (!success) {
    _svc.ui_manager.show_snackbar("Factory reset failed");
    update_display();
    return false;
  }

  AG_LOGI(TAG, "Factory reset success");

  _settings = defaults;
  _mode = _settings.operating_mode;
  _behavior = Behavior::Idle;
  _lock_state = LockState::Locked;
  _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);
  _tracking_active = false;
  _tracking_session_id = 0;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  _svc.ui_manager.sync_settings(_settings);
  _svc.ui_manager.reset_to_home();
  update_display();

  return true;
}

void Orchestrator::save_tag(uint8_t tag_index, const char *tag_label) {
  (void)tag_index;
  (void)tag_label;
  // TODO: persist tag association with current route point via StorageService
  _svc.ui_manager.show_snackbar("Tag saved");
  update_display();
}

void Orchestrator::shutdown() {
  AG_LOGI(TAG, "shutdown");

  if (_tracking_active) {
    stop_tracking();
  }

  _svc.storage_service.backup_cache();
  _svc.ui_manager.set_screen(Screen::Shutdown);
  update_display();

  // Allow display to finish e-paper refresh
  RTOS::delay_ms(SHUTDOWN_DISPLAY_DELAY_MS);

  _svc.power_service.shutdown(); // BMS QoN — does not return
}

// ---------------------------------------------------------------------------
// BLE event handlers
// ---------------------------------------------------------------------------

void Orchestrator::on_ble_connected() {
  AG_LOGI(TAG, "BLE client connected");

  // Dismiss pairing passkey screen if it was showing
  _svc.ui_manager.dismiss_pairing_passkey();

  // Push current state to BLE characteristics
  _svc.ble_service.notify_measures(_cached_measures, _latest_gps, time(nullptr));
  _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                 _tracking_session_id);
  _svc.ble_service.update_config(_settings);

  request_background_display_update();
}

void Orchestrator::on_ble_disconnected() {
  AG_LOGI(TAG, "BLE client disconnected");
  _svc.ui_manager.dismiss_pairing_passkey();
  request_background_display_update();
}

void Orchestrator::on_ble_auth_complete() {
  AG_LOGI(TAG, "BLE auth complete");
  _svc.ui_manager.dismiss_pairing_passkey();
  request_background_display_update();
}

void Orchestrator::on_ble_config_write() {
  uint8_t buf[BLE_WRITE_BUF_SIZE];
  size_t len = _svc.ble_service.take_pending_config_write(buf, sizeof(buf));
  if (len == 0) {
    return;
  }

  // Decode into a copy of current settings (merge approach)
  GoSettings temp = _settings;
  BleConfigDecodeResult result = BleService::decode_config_write(buf, len, temp);

  // Reject writes that contain unrecognized config keys
  if (result.op == BleConfigOp::Set && result.has_unknown_keys) {
    AG_LOGW(TAG, "BLE config set rejected: unknown config key");
    _svc.ble_service.notify_command_result(BleCommand::Set, false, BLE_VAL_ERR_UNKNOWN_CONFIG_KEY);
    return;
  }

  switch (result.op) {
  case BleConfigOp::Set: {
    AG_LOGI(TAG, "BLE config set");
    const bool was_gps_active = is_gps_active();
    const GoSettings previous_settings = _settings;
    _settings = temp;
    save_go_settings(_config_store, _settings);

    // Propagate runtime changes
    reschedule_sensor_timer(previous_settings);
    _svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000);
    _svc.ui_manager.sync_settings(_settings);

    // Notify BLE client with updated full config
    _svc.ble_service.notify_config(_settings);
    _svc.ble_service.update_config(_settings);

    const bool is_gps_active_now = is_gps_active();
    if (!was_gps_active && is_gps_active_now) {
      _svc.gps_service.start();
    } else if (was_gps_active && !is_gps_active_now) {
      deactivate_gps();
    }
    _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);

    // Check if operating mode changed via BLE
    if (_settings.operating_mode != _mode) {
      change_mode(_settings.operating_mode);
    }

    request_background_display_update();
    break;
  }
  case BleConfigOp::Command: {
    AG_LOGI(TAG, "BLE command: %d", static_cast<int>(result.cmd));

    switch (result.cmd) {
    case BleCommand::Co2Calibration:
      // Runs asynchronously on the SensorProducer task.
      // Result arrives via Co2CalibrationDone event.
      _svc.ble_service.notify_command_progress(result.cmd);
      _svc.sensor_producer.request_co2_calibration();
      break;
    case BleCommand::ClearData: {
      _svc.ble_service.notify_command_progress(result.cmd);
      const bool cleared = clear_data();
      _svc.ble_service.notify_command_result(result.cmd, cleared,
                                             cleared ? nullptr : BLE_VAL_ERR_CLEAR_FAILED);
    } break;
    case BleCommand::FactoryReset: {
      _svc.ble_service.notify_command_progress(result.cmd);
      const bool reset = factory_reset();
      _svc.ble_service.notify_command_result(result.cmd, reset,
                                             reset ? nullptr : BLE_VAL_ERR_FACTORY_RESET_FAILED);
      if (reset) {
        AG_LOGI(TAG, "Rebooting in 2s");
        RTOS::delay_ms(2000);
        reboot();
      }
    } break;
    case BleCommand::StartTracking: {
      const bool was_idle = !_tracking_active;
      start_tracking();
      _svc.ble_service.notify_command_result(result.cmd, was_idle,
                                             was_idle ? nullptr : BLE_VAL_ERR_ALREADY_TRACKING);
    } break;
    case BleCommand::StopTracking: {
      const bool was_tracking = _tracking_active;
      stop_tracking();
      _svc.ble_service.notify_command_result(result.cmd, was_tracking,
                                             was_tracking ? nullptr : BLE_VAL_ERR_NOT_TRACKING);
    } break;
    case BleCommand::SetAiding: {
      const bool has_time = has_aiding_time(result.aiding);
      const bool has_data = has_aiding_position(result.aiding) || has_time;
      if (has_data) {
        _svc.gps_service.set_aiding_data(result.aiding);
      }
      if (has_time) {
        // GPSService might delay the execution, hence if time available, still can sync it
        RTOS::set_system_time_from_epoch(result.aiding.epoch_s);
      }
      _svc.ble_service.notify_command_result(result.cmd, has_data,
                                             has_data ? nullptr : BLE_VAL_ERR_NO_AIDING_DATA);
    } break;
    case BleCommand::Set:
      // Not reachable via Command op — handled above as config-set rejection
      break;
    case BleCommand::Unknown:
      AG_LOGW(TAG, "BLE unknown command");
      _svc.ble_service.notify_command_result(result.cmd, false, BLE_VAL_ERR_UNKNOWN_COMMAND);
      break;
    }
    break;
  }
  case BleConfigOp::Invalid:
    AG_LOGW(TAG, "BLE config write: invalid CBOR");
    break;
  }
}

void Orchestrator::on_ble_history_write() {
  uint8_t buf[BLE_WRITE_BUF_SIZE];
  size_t len = _svc.ble_service.take_pending_history_write(buf, sizeof(buf));
  if (len == 0) {
    return;
  }

  BleHistoryDecodeResult result = BleService::decode_history_write(buf, len);

  switch (result.op) {
  case BleHistoryOp::List:
    AG_LOGI(TAG, "BLE history: list");
    _svc.ble_service.handle_history_list();
    break;
  case BleHistoryOp::Start:
    AG_LOGI(TAG, "BLE history: start session %" PRIu32, result.session_id);
    _svc.ble_service.handle_history_start(result.session_id);
    break;
  case BleHistoryOp::Fill:
    AG_LOGI(TAG, "BLE history: fill %u points", static_cast<unsigned>(result.point_count));
    _svc.ble_service.handle_history_fill(result.point_indices, result.point_count);
    break;
  case BleHistoryOp::End:
    AG_LOGI(TAG, "BLE history: end");
    _svc.ble_service.handle_history_end();
    break;
  case BleHistoryOp::Delete:
    AG_LOGI(TAG, "BLE history: delete session %" PRIu32, result.session_id);
    if (_tracking_active && _tracking_session_id == result.session_id) {
      _svc.ble_service.notify_history_error(BLE_VAL_ERR_SESSION_ACTIVE);
    } else {
      _svc.ble_service.handle_history_delete(result.session_id);
      _svc.ble_service.update_status(_latest_power, _latest_gps, _tracking_active,
                                     _tracking_session_id);
    }
    break;
  case BleHistoryOp::Invalid:
    AG_LOGW(TAG, "BLE history write: invalid CBOR");
    break;
  }
}

void Orchestrator::on_ble_pairing_request(uint32_t passkey) {
  AG_LOGI(TAG, "BLE pairing request: passkey=%06" PRIu32, passkey);
  _svc.ui_manager.show_pairing_passkey(passkey);
  update_display();
}

void Orchestrator::init_ble_if_portable() {
  if (_mode != OperatingMode::Portable) {
    return;
  }

  if (_svc.ble_service.is_initialized()) {
    return; // already running
  }

  if (!_svc.ble_service.init(_serial)) {
    AG_LOGE(TAG, "BLE init failed");
  }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void Orchestrator::update_display() {
  uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  _svc.ui_manager.clear_expired_snackbar(now_ms);
  BuildContext ctx = build_context();
  DisplayValues values = _svc.ui_manager.build_values(ctx);
  _svc.display_service.update(values);

  // Schedule a follow-up refresh to visually clear the snackbar after it
  // expires.  Only arm once per snackbar — intermediate update_display()
  // calls (from sensor data, input, etc.) that happen before the deadline
  // naturally clear it and the timer fires as a harmless no-op.
  if (values.snackbar_text != nullptr && _snackbar_refresh_deadline_ms == 0) {
    _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
  }

  AG_LOGI(TAG, "display update done");
}

void Orchestrator::request_background_display_update() {
  if (!_svc.ui_manager.is_on_menu_screen()) {
    update_display();
  }
}

BuildContext Orchestrator::build_context() const {
  // Convert MeasuresAGo to Measures for the BuildContext reference
  _display_measures = Measures{};
  _display_measures.temp_hum_a = _cached_measures.temp_hum_a;
  _display_measures.pm_a = _cached_measures.pm_a;
  _display_measures.co2 = _cached_measures.co2;
  _display_measures.tvoc_nox = _cached_measures.tvoc_nox;
  _display_measures.power = _cached_measures.power;
  _display_measures.pressure = _cached_measures.pressure;

  // Read chart data cache
  uint16_t cache_count = _svc.storage_service.read_cache(_cache_buf, UI_CHART_BUF_SIZE);

  // Extract battery data
  uint8_t battery_pct = 0xFF;
  if (_latest_power.battery_percentage >= 0.0f) {
    battery_pct = static_cast<uint8_t>(_latest_power.battery_percentage);
  }

  bool is_charging = is_bms_charging(_latest_power.charging_status);

  return BuildContext{
      .sensor_data = _display_measures,
      .battery_pct = battery_pct,
      .is_battery_charging = is_charging,
      .locked = (_lock_state == LockState::Locked),
      .ble_enabled = (_mode == OperatingMode::Portable),
      .ble_connected = _svc.ble_service.is_connected(),
      .wifi_enabled = false, // WiFi not yet implemented
      .gps_enabled = is_gps_active(),
      .gps_fix = is_fix_valid(_latest_gps.fix),
      .tracking_active = _tracking_active,
      .display_off = false,
      .use_fahrenheit = _settings.use_fahrenheit,
      .pm_use_usaqi = _settings.pm_use_usaqi,
      .cache = _cache_buf,
      .cache_count = static_cast<uint8_t>(cache_count),
      .now_ms = static_cast<uint32_t>(RTOS::get_time_ms()),
  };
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

void Orchestrator::try_enter_sleep() {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t awake_ms = now - _last_measurement_ms;

  auto decision = _svc.power_service.decide_sleep(_settings, _lock_state, _mode, awake_ms);

  if (decision.type == PowerService::SleepType::None) {
    return;
  }

  // decision.type == Deep
  prepare_for_sleep(decision.duration_ms);
  AG_LOGI(TAG, "entering deep sleep (%lu ms)", static_cast<unsigned long>(decision.duration_ms));
  _svc.power_service.enter_sleep(decision.duration_ms);
  // Never returns — CPU reboots on wake.
}

void Orchestrator::prepare_for_sleep(uint32_t sleep_duration_ms) {
  AG_LOGI(TAG, "prepare_for_sleep");

  // Ensure pending display refresh completes before stopping worker
  _svc.ui_manager.clear_expired_snackbar(static_cast<uint32_t>(RTOS::get_time_ms()));
  BuildContext ctx = build_context();
  DisplayValues values = _svc.ui_manager.build_values(ctx);
  _svc.display_service.update(values, true); // wait = true

  // Save RTC display snapshot so the next button wake can render immediately
  // without NVS or sensor reads.
  save_rtc_display_snapshot(values);

  _svc.ble_service.deinit();
  _svc.sensor_producer.stop();

  // Active GPS: stop task only — leave TAU1113 tracking for hot-start.
  // Inactive GPS: stop task and send GNSS stop before sleep.
  if (is_gps_active()) {
    _svc.gps_service.stop();
  } else {
    _svc.gps_service.stop_and_idle_gnss();
  }

  _svc.input_service.stop();
  _svc.display_service.stop();

  // Put SSD1680 into deep sleep mode 1 after stopping the worker task.
  // Reduces quiescent current from ~100 µA to <1 µA during ESP deep sleep.
  // driver_hw_init_full() exits deep sleep on next init() via hardware reset.
  _svc.display_service.deep_sleep();

  // Flush and close the route file so buffered route points are committed to
  // NAND before the CPU reboots.  On the next wake, start_route() reopens the
  // same file in append mode and restores the point count from its size.
  // No-op when no route is active.
  _svc.storage_service.end_route();

  _svc.storage_service.backup_cache();

  // Persist RTC state with the warm-sensor flag for the next wake cycle.
  RtcAppState state = snapshot_state();
  state.sensors_warm = _svc.power_service.should_hold_pm_sensor(sleep_duration_ms);
  _svc.power_service.save_state(state);

  // Reset external watchdog last — gives it the full timeout window during sleep.
  _svc.power_service.reset_ext_watchdog();

  // Start LP Core to keep pulsing the external watchdog during deep sleep.
  ulp_wdt_start();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Orchestrator::deactivate_gps() {
  _svc.gps_service.stop_and_idle_gnss();
  _latest_gps = GpsData{};
}

bool Orchestrator::is_gps_active() const {
  if (_settings.gps_mode == GpsMode::AlwaysOff) {
    return false;
  }
  if (_settings.gps_mode == GpsMode::AlwaysOn) {
    return true;
  }
  // GpsMode::OnWhenTracking
  return _tracking_active;
}

uint32_t Orchestrator::generate_session_id() {
  const uint32_t new_id = generate_random_number(SESSION_ID_LENGTH);
  AG_LOGI(TAG, "generate_session_id: %" PRIu32, new_id);
  return new_id;
}

RtcAppState Orchestrator::snapshot_state() const {
  return RtcAppState{
      .mode = _mode,
      .behavior = _behavior,
      .lock_state = _lock_state,
      .gps_enabled = _gps_enabled,
      .tracking_active = _tracking_active,
      .tracking_session_id = _tracking_session_id,
  };
}
