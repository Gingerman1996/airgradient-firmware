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
#include "go_melody.h"
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
  // charge_current_ma and battery_learning_enabled are intentionally not
  // persisted: production firmware always boots with safe defaults (500 mA /
  // learning off).  Force-reset here so any stale NVS entry from an earlier
  // firmware version cannot leak into the new build.  Admin must opt back in
  // per session.  The manual charge-disable override is runtime-only on
  // PowerService — explicit clear here so the field starts clean on every
  // boot regardless of how the previous session exited.
  _settings.charge_current_ma = 500;
  _settings.battery_learning_enabled = false;
  _svc.ui_manager.sync_settings(_settings);
  _svc.ui_manager.set_admin_mode(_settings.admin_mode);
  _svc.power_service.set_charge_cutoff_at_full(_settings.charge_cutoff_at_full);
  _svc.power_service.set_manual_charge_disabled(false);
  _svc.power_service.set_charge_current_ma(_settings.charge_current_ma);
  apply_led_brightness();

  if (!handoff.measurement_completed) {
    _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  }

  _latest_power = _svc.power_service.poll_bms();
  handle_edv_cutoff();

  // Boot-resume the automated battery-learning FSM (design §6).  Runs once,
  // after settings are loaded and one poll_bms() has refreshed the FG flags +
  // power source.  No-op for a normal field unit (stage Idle/Complete/Failed).
  resume_blearn_on_boot();

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

  // Post-melody AQI restore deadline
  if (_melody_pm25_restore_deadline_ms != 0) {
    uint32_t m_remaining = _melody_pm25_restore_deadline_ms - now;
    next = std::min(next, m_remaining);
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
  // In Battery Learning LOW_POWER the SPS30 is parked in Sleep and the next
  // measurement skips PM anyway — pre-waking it would spike idle current.
  // Drop the whole pre-wake path while low-power is active.
  bool pm_sleep_eligible =
      _mode != OperatingMode::Offline && !_in_learning_low_power &&
      _svc.power_service.should_sleep_pm_sensor(interval);
  if (pm_sleep_eligible && !_pm_prepare_sent) {
    uint32_t measure_deadline = _last_measurement_ms + interval;
    uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
    if ((now - prepare_deadline) < MAX_REASONABLE_TIMEOUT_MS) {
      // prepare wakes the SPS30 from Sleep (pm_wake at the start of warmup)
      // then warms it up — no GPIO power toggle, the sensor stays powered.
      AG_LOGI(TAG, "PM pre-wake: requesting prepare (wake + warmup)");
      _svc.sensor_producer.request_prepare();
      _pm_prepare_sent = true;
    }
  }

  // --- Sensor timer (single) ---
  if ((now - _last_measurement_ms) >= interval) {
    // Battery Learning LOW_POWER: PMID +5 V is intentionally dead, so SPS30
    // reads would just time out on the I²C bus.  Drop PM_A from the request
    // mask; CO2 / TVOC / TempHum keep running for the live display.
    const SensorGroup groups = _in_learning_low_power
                                   ? static_cast<SensorGroup>(
                                         static_cast<uint8_t>(SensorGroup::Other) |
                                         static_cast<uint8_t>(SensorGroup::TvocNox))
                                   : SensorGroup::All;
    _svc.sensor_producer.request_measurement(1, groups);
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

  // --- Admin-entry deadline ---
  // The input handler enforces the per-step deadline on each touch event,
  // but if the user simply does nothing after arming we'd be stuck.  This
  // tick catches the no-input timeout case.
  if (_admin_entry_state != AdminEntryState::None && _admin_entry_deadline_ms != 0 &&
      static_cast<int32_t>(now - _admin_entry_deadline_ms) >= 0) {
    abort_admin_entry("timeout");
  }

  // --- Snackbar refresh timer ---
  // Ensures a follow-up display update after the snackbar expires so it is
  // visually cleared even when no other events trigger update_display().
  if (_snackbar_refresh_deadline_ms != 0 &&
      (now - _snackbar_refresh_deadline_ms) < MAX_REASONABLE_TIMEOUT_MS) {
    _snackbar_refresh_deadline_ms = 0;
    request_background_display_update();
  }

  // --- Post-melody AQI LED restore ---
  // The Play Sound melody visual leaves the back LEDs off when it
  // finishes; this timer restores the normal PM2.5 AQI indicator.
  if (_melody_pm25_restore_deadline_ms != 0 &&
      (now - _melody_pm25_restore_deadline_ms) < MAX_REASONABLE_TIMEOUT_MS) {
    _melody_pm25_restore_deadline_ms = 0;
    apply_pm25_indicator();
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
  handle_edv_cutoff();
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

    // Automated battery-learning FSM owns charge / load / cue / screen while a
    // run is in progress (design Part 2).  When it's active it fully supersedes
    // the legacy admin "battery learning" UX below, so skip that block.
    if (tick_blearn()) {
      _last_bms_status_poll_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      return;
    }

    // Charge-done UX sequence — only runs in admin mode (calibration
    // affordance, hidden from production users):
    //   1. BMS transitions to NotCharging while USB is plugged in →
    //      play a short "charge done" melody once, start a 500 s rest
    //      countdown (matches the BQ27427 ResRelax Time so the fuel gauge
    //      can capture its OCV-at-rest reading before the cell is
    //      disturbed by being unplugged).
    //   2. 500 s elapses while still in that state → blink LED8 and play
    //      an "unplug me now" beep.  This is the user's cue.
    //   3. Anything else (charging resumes, USB removed) cancels both
    //      and clears state.
    const bool plugged_in = bms_power_source_has_external_input(status.power_source);
    // Battery-learning UX gate: admin mode must be active AND the
    // per-feature toggle must be enabled.  Production users (admin off)
    // never see this; admins who have completed calibration can turn the
    // notifications off without leaving admin mode.
    const bool learning_ux_enabled =
        _settings.admin_mode && _settings.battery_learning_enabled;
    const bool in_rest_state = learning_ux_enabled && !now_charging && plugged_in;
    const bool was_in_rest_state = (_charge_done_start_ms != 0);
    const uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());

    if (in_rest_state && !was_in_rest_state) {
      // Entered: BMS just finished charging while still plugged in.
      _charge_done_start_ms = now_ms;
      _charge_done_alerted = false;
      AG_LOGI(TAG, "charge done — starting %u s rest countdown", CHARGE_REST_TIMEOUT_MS / 1000);
      // Force EN_CHG off so the cell drops off BMS float and enters true
      // relax with USB still plugged.  Per sluucd5 §7.4.2.2.1 the gauge
      // needs |I| < Quit Current (~53 mA for a 1340 mAh cell) for
      // Dsg Relax Time (60 s) to enter RELAXATION, then OCV Wait Time
      // (60 s) before OCV1 — total ~120 s.  With charge disabled and the
      // load fed from PMID, ibat ≈ 0 mA clears this by a wide margin.
      _svc.power_service.set_manual_charge_disabled(true);
      AG_LOGI(TAG, "blearn auto: disabling charge for RELAX_1, expect OCV1 at T+120s");
      static constexpr BuzzerService::Note kChargeDoneMelody[] = {
          {1500, 100}, {0, 50}, {2000, 100}, {0, 50}, {2500, 150},
      };
      _svc.buzzer.play(kChargeDoneMelody,
                       sizeof(kChargeDoneMelody) / sizeof(kChargeDoneMelody[0]));
    } else if (!in_rest_state && was_in_rest_state) {
      // Exited: charging resumed or USB unplugged.
      _charge_done_start_ms = 0;
      _charge_done_alerted = false;
      _svc.led.set_charge_done_alert(false);
    } else if (in_rest_state && !_charge_done_alerted &&
               (now_ms - _charge_done_start_ms) >= CHARGE_REST_TIMEOUT_MS) {
      // Rest period complete — wake the user.
      _charge_done_alerted = true;
      AG_LOGI(TAG, "rest period complete — alerting user to unplug");
      _svc.led.set_charge_done_alert(true);
      static constexpr BuzzerService::Note kUnplugAlert[] = {
          {2700, 100}, {0, 80}, {2700, 100}, {0, 80}, {2700, 100}, {0, 80}, {3500, 250},
      };
      _svc.buzzer.play(kUnplugAlert, sizeof(kUnplugAlert) / sizeof(kUnplugAlert[0]));
    }

    // --- Battery Learning: charge-current override + LOW_POWER transition ---
    //
    // ICHG override: while charging in learning mode, force 1500 mA so the
    // charge phase doesn't dominate the (already 18 h) full learning cycle.
    // Restored to the user-configured value on every tick once charging
    // stops, USB is removed, or learning is disabled.  PowerService's
    // setter is idempotent, so the per-tick push only costs one comparison.
    //
    // LOW_POWER mode gates SPS30 / GPS / BLE / SCD4x so the cell sees minimal
    // load.  It is active only while learning AND plugged in, and released on
    // unplug or when learning is disabled.  See the want_low_power assignment
    // below for why the unplugged discharge must run un-gated.
    const uint16_t desired_ichg = (learning_ux_enabled && plugged_in && now_charging)
                                      ? LEARNING_CHARGE_CURRENT_MA
                                      : _settings.charge_current_ma;
    _svc.power_service.set_charge_current_ma(desired_ichg);

    // Gate LOW_POWER on plugged_in.  While plugged (charge + post-charge OCV1
    // relax) the load is fed from PMID-PassThrough = +5 V off VBUS, so the cell
    // already sits near 0 mA; gating the rails just keeps them quiet for a clean
    // OCV1.  Once UNPLUGGED the cycle must run the full load on cell power: the
    // fuel gauge only registers a DISCHARGE — and learns the Ra resistance grid
    // plus end-of-discharge Fast-Qmax — when cell current exceeds its Dsg
    // Current Threshold (~120 mA @ DC 2000; BQ27427 TRM sluucd5 §7.4.2.2.1).
    // Holding LOW_POWER through the discharge pins idle at ~50–70 mA, below that
    // threshold, so the gauge stays in RELAXATION and never learns Ra.  Running
    // SPS30 + GPS + BLE + SCD4x drives a real discharge down to the EDV
    // ship-mode cutoff (2.9 V) where the relaxed OCV2 is taken.
    const bool want_low_power = learning_ux_enabled && plugged_in;
    if (want_low_power && !_in_learning_low_power) {
      AG_LOGI(TAG, "battery learning: entering LOW_POWER (SPS30 sleep, PM skip, "
                   "GPS sleep %u h, BLE deinit, SCD4x idle, SGP41 sampler off)",
              LEARNING_GPS_SLEEP_MS / 3600000U);
      // On v0.3 the +5 V PMID rail can't be collapsed in firmware (BMS keeps
      // EN_OTG armed), so the SPS30 itself drops to ~38 µA via its I2C Sleep
      // command.  check_timers() still strips PM from the request mask while
      // _in_learning_low_power, so nothing wakes it until learning exits.
      _svc.sensor_producer.request_pm_sleep();
      _svc.gps_service.sleep_for_ms(LEARNING_GPS_SLEEP_MS);
      _svc.sensor_producer.request_low_power(true);
      _svc.ble_service.deinit();
      _in_learning_low_power = true;
    } else if (!want_low_power && _in_learning_low_power) {
      AG_LOGI(TAG, "battery learning: exiting LOW_POWER (SPS30 wake, GPS restore, "
                   "BLE re-init, SCD4x periodic, SGP41 sampler on)");
      // Clear the flag first so the prepare's wake+warmup isn't gated out by
      // the LOW_POWER pre-wake guard in check_timers(), and so the GPS helpers
      // below act instead of deferring to learning.
      _in_learning_low_power = false;
      // Restore the GPS to whatever the current mode/tracking dictates: wake +
      // start when it should be active, or re-sleep/idle when it should not.
      // Learning held the module in CFG-SLEEP, so the not-active branch parks
      // it correctly rather than leaving it awake (the old unconditional wake
      // left an OnWhenTracking-idle device drawing active GPS current).
      if (is_gps_active()) {
        activate_gps();
      } else {
        deactivate_gps();
      }
      _svc.sensor_producer.request_low_power(false);
      _svc.sensor_producer.request_prepare();
      _pm_prepare_sent = true;
      init_ble_if_portable();
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

  // Reconcile PM sleep/wake with the new interval.  The SPS30 stays powered;
  // only its Sleep state changes.
  //   - New interval >= threshold (>= 20 s): the sensor will be parked after
  //     the next measurement (post-measure request_pm_sleep), and pre-wake
  //     fires CONFIG_SENSOR_WARMUP_DURATION_MS ahead of it.  No action here.
  //   - New interval < threshold (continuous): measurements are too close
  //     together to absorb the ~10 s warmup, so the sensor must stay awake.
  //     Wake it now via prepare (wake + warmup) in case it was sleeping.
  uint32_t new_interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline && !_in_learning_low_power &&
      !_svc.power_service.should_sleep_pm_sensor(new_interval_ms)) {
    _svc.sensor_producer.request_prepare();
  }
  _pm_prepare_sent = false;
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

  apply_pm25_indicator();

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

  // Park the PM sensor in Sleep after measurement when the interval justifies
  // it.  On v0.3 the +5 V PMID rail cannot be collapsed in firmware (BMS keeps
  // EN_OTG armed), so the SPS30 itself drops to ~38 µA via its I2C Sleep
  // command — the sensor stays powered, no GPIO toggle.  The next pre-wake
  // (request_prepare) wakes and warms it before the following measurement.
  // Skipped while learning owns the PM sensor (it parks it via request_low_power).
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline && !_in_learning_low_power &&
      _svc.power_service.should_sleep_pm_sensor(interval_ms)) {
    _svc.sensor_producer.request_pm_sleep();
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

  // Admin-mode entry gesture takes priority once armed — it consumes touch
  // input until success / abort.  This must run before the UI dispatch.
  if (consume_admin_entry_input(input)) {
    update_display();
    return;
  }

  // Unlocked: white LED flash on accepted touch, gated by the Touch LED
  // setting.  0=Off skips the flash entirely; 1=Dim ≈10 % (25/255);
  // 2=Bright ≈30 % (76/255, the historical default).
  static constexpr uint8_t TOUCH_LED_PWM[3] = {0, 25, 76};
  const uint8_t touch_pwm =
      TOUCH_LED_PWM[(_settings.touch_led_brightness < 3) ? _settings.touch_led_brightness : 2];
  if (touch_pwm != 0) {
    switch (input.source) {
    case InputSource::TouchDown:
      _svc.led.flash_white(LedService::Led::Right, touch_pwm, 100);
      break;
    case InputSource::TouchUp:
      _svc.led.flash_white(LedService::Led::Left, touch_pwm, 100);
      break;
    case InputSource::TouchEnter:
      _svc.led.flash_white(LedService::Led::Select, touch_pwm, 100);
      break;
    default:
      break;
    }
  }

  // Detect the 5-rapid-Select-taps admin-entry pattern.  Runs in parallel
  // with normal UI dispatch — the taps still navigate the UI; the gesture
  // is detected on top of that.  Arming only happens when we're not
  // already mid-sequence (the consume_admin_entry_input branch above
  // covers that case).
  if (input.source == InputSource::TouchEnter && input.type == InputType::ShortPress) {
    const uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
    if (record_select_tap_check_pattern(now_ms)) {
      arm_admin_entry();
    }
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
  case UIAction::ExitAdminMode:
    _settings.admin_mode = false;
    // Reset admin-only overrides that production users must never inherit.
    // charge_current_ma and battery_learning_enabled are in-memory only (not
    // persisted), so a future boot already defaults to safe values — but
    // enforce here too so the BMS and the learning state machine are
    // reconciled immediately on exit without waiting for a reboot.  The
    // manual charge-disable override is runtime-only on PowerService; clear
    // it here so a stale "disabled" can never carry across the admin exit.
    _settings.charge_current_ma = 500;
    _settings.battery_learning_enabled = false;
    save_go_settings(_config_store, _settings);
    _svc.power_service.set_charge_current_ma(_settings.charge_current_ma);
    _svc.power_service.set_manual_charge_disabled(false);
    _svc.ui_manager.sync_settings(_settings);
    _svc.ui_manager.set_admin_mode(false);
    _svc.ui_manager.show_snackbar("Admin mode off");
    AG_LOGI(TAG, "admin mode exited via Settings menu");
    break;
  case UIAction::TestGpsSleep: {
    // Admin-mode diagnostic: send CFG-SLEEP for 15 s; the GpsService run
    // loop arms an auto-resync at deadline+1.5 s so the UART link is
    // restored automatically when the receiver self-wakes at 9600 baud.
    // No host-pulse early wake — keep this trigger atomic.
    constexpr uint32_t TEST_SLEEP_MS = 15000;
    _svc.gps_service.sleep_for_ms(TEST_SLEEP_MS);
    _svc.ui_manager.show_snackbar("GPS sleep 15s...");
    AG_LOGI(TAG, "admin: GPS sleep test triggered (%u ms)",
            static_cast<unsigned>(TEST_SLEEP_MS));
    break;
  }
  case UIAction::StartBatteryLearning: {
    // Arm the from-scratch learning change limits (§10.3) BEFORE entering the
    // FSM's cycle-1 Charge, so the first cycle can move Qmax/Ra freely.
    if (!_svc.power_service.set_learning_update_status(true)) {
      _svc.ui_manager.show_snackbar("Learning arm failed");
      AG_LOGE(TAG, "blearn: failed to set Update Status learning bits — not starting");
      break;
    }
    _blearn.start();
    persist_blearn_state();
    // Leave the admin menu so the phase screen is visible — apply_blearn_action()
    // suppresses repaints while on a menu screen, so a run armed from the menu
    // would otherwise never show "Charging…".  start() always begins at Charge.
    _svc.ui_manager.set_screen(Screen::BlearnCharging);
    _svc.ui_manager.show_snackbar("Battery learning started");
    AG_LOGI(TAG, "blearn: run started by admin (cycle 1)");
    break;
  }
  case UIAction::ResetBatteryLearning: {
    _blearn.reset();
    persist_blearn_state();
    // Release any learning rails + cue so the device returns to normal.
    _svc.power_service.set_manual_charge_disabled(false);
    _svc.led.set_charge_done_alert(false);
    _svc.ui_manager.show_snackbar("Battery learning reset");
    AG_LOGI(TAG, "blearn: run reset to Idle by admin");
    break;
  }
  case UIAction::PlaySound: {
    apply_settings_change();
    const uint32_t duration_ms = play_sound_select(
        _svc.buzzer, _svc.led, static_cast<SoundSelect>(result.sound_index));
    if (duration_ms > 0) {
      // Schedule an AQI-indicator restore once the melody finishes; small
      // pad covers RTOS jitter between the visual task and this deadline.
      _melody_pm25_restore_deadline_ms = RTOS::get_time_ms() + duration_ms + 200;
    }
    break;
  }
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
    activate_gps();
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

  // Ensure the PM sensor is awake and measuring after a mode change — it may
  // have been parked in Sleep under the previous interval.  prepare wakes
  // (pm_wake at warmup start) + warms it.  No-op effect if already awake.
  // Skipped while learning owns the PM sensor.
  if (_mode != OperatingMode::Offline && !_in_learning_low_power) {
    _svc.sensor_producer.request_prepare();
    _pm_prepare_sent = true;
  }

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
  _svc.power_service.set_charge_cutoff_at_full(_settings.charge_cutoff_at_full);
  _svc.power_service.set_charge_current_ma(_settings.charge_current_ma);
  apply_led_brightness();

  // Edge: turning Battery Learning off clears any auto-disable that the
  // charge-done hook may have armed (Phase 0 → Phase 2 RELAX_1 entry).  The
  // setter is idempotent, so a redundant clear when blearn was never on is a
  // no-op.
  if (previous_settings.battery_learning_enabled &&
      !_settings.battery_learning_enabled) {
    _svc.power_service.set_manual_charge_disabled(false);
    AG_LOGI(TAG, "blearn auto: re-enabling charge (blearn turned off)");
  }

  const bool is_gps_active_now = is_gps_active();
  if (!was_gps_active && is_gps_active_now) {
    activate_gps();
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

void Orchestrator::apply_led_brightness() {
  // Display LED: 6-level 0–10 % scale.
  //   0=Off, 1=2%, 2=4%, 3=6%, 4=8%, 5=10%   →   PWM 0, 5, 10, 15, 20, 26
  static constexpr uint8_t PWM_MAP[6] = {0, 5, 10, 15, 20, 26};
  const uint8_t idx = (_settings.led_brightness < 6) ? _settings.led_brightness : 5;
  _svc.led.set_indicator_brightness(PWM_MAP[idx]);
  // Re-apply the back-LED PM2.5 colour so a brightness change takes effect
  // immediately without waiting for the next sensor update.
  apply_pm25_indicator();
}

void Orchestrator::apply_pm25_indicator() {
  // Back AQI LEDs: 5-level 0–100 % scale (independent of Display LED scale).
  //   0=Off, 1=25%, 2=50%, 3=75%, 4=100%   →   PWM 0, 64, 128, 191, 255
  static constexpr uint8_t PWM_MAP[5] = {0, 64, 128, 191, 255};
  const uint8_t scale =
      PWM_MAP[(_settings.back_led_brightness < 5) ? _settings.back_led_brightness : 4];

  // Off setting or invalid PM2.5 → LEDs dark.
  const float pm = _cached_measures.pm_a.pm_25;
  if (scale == 0 || !_cached_measures.pm_a.is_pm_25_valid()) {
    _svc.led.set_back_leds_rgb(0, 0, 0);
    return;
  }

  // US EPA PM2.5 → AQI category colours (µg/m³ breakpoints).
  uint8_t r = 0, g = 0, b = 0;
  if (pm < 12.0f) {            // Good
    r = 0;   g = 255; b = 0;
  } else if (pm < 35.0f) {     // Moderate
    r = 255; g = 255; b = 0;
  } else if (pm < 55.0f) {     // Unhealthy for Sensitive Groups
    r = 255; g = 128; b = 0;
  } else if (pm < 150.0f) {    // Unhealthy
    r = 255; g = 0;   b = 0;
  } else if (pm < 250.0f) {    // Very Unhealthy
    r = 128; g = 0;   b = 128;
  } else {                      // Hazardous — saddle brown
    r = 139; g = 69;  b = 19;
  }

  // Scale each channel by the brightness setting (0–255).
  const uint8_t sr = static_cast<uint8_t>((static_cast<uint16_t>(r) * scale) / 255);
  const uint8_t sg = static_cast<uint8_t>((static_cast<uint16_t>(g) * scale) / 255);
  const uint8_t sb = static_cast<uint8_t>((static_cast<uint16_t>(b) * scale) / 255);
  _svc.led.set_back_leds_rgb(sr, sg, sb);
}

// ---------------------------------------------------------------------------
// Admin-mode entry gesture
// ---------------------------------------------------------------------------

bool Orchestrator::record_select_tap_check_pattern(uint32_t now_ms) {
  // Store this tap in the ring buffer (oldest entry is overwritten).
  _admin_tap_times[_admin_tap_idx] = now_ms;
  _admin_tap_idx = static_cast<uint8_t>((_admin_tap_idx + 1) % ADMIN_TAP_COUNT);

  // Pattern: the OLDEST entry in the buffer is within ADMIN_TAP_WINDOW_MS
  // of `now_ms`.  Because the ring buffer always holds the last N timestamps
  // (after at least N taps), the oldest is at the next-to-write slot.
  const uint32_t oldest = _admin_tap_times[_admin_tap_idx];
  if (oldest == 0) {
    return false; // buffer not full yet — haven't seen N taps total
  }
  return (now_ms - oldest) <= ADMIN_TAP_WINDOW_MS;
}

void Orchestrator::arm_admin_entry() {
  _admin_entry_state = AdminEntryState::ArmedExpectingLeft;
  _admin_entry_deadline_ms =
      static_cast<uint32_t>(RTOS::get_time_ms()) + ADMIN_STEP_TIMEOUT_MS;
  _svc.led.set_charge_done_alert(true); // reuse the LED8 blinker
  AG_LOGI(TAG, "admin entry armed — press Left then Right within %u s",
          ADMIN_STEP_TIMEOUT_MS / 1000);
}

void Orchestrator::complete_admin_entry() {
  _admin_entry_state = AdminEntryState::None;
  _admin_entry_deadline_ms = 0;
  _svc.led.set_charge_done_alert(false);
  _svc.led.flash_led8_green(ADMIN_SUCCESS_FLASH_MS);

  _settings.admin_mode = true;
  save_go_settings(_config_store, _settings);
  _svc.ui_manager.set_admin_mode(true);
  _svc.ui_manager.show_snackbar("Admin mode on");
  AG_LOGI(TAG, "admin entry complete — admin_mode=true");
}

void Orchestrator::abort_admin_entry(const char *reason) {
  if (_admin_entry_state == AdminEntryState::None) {
    return;
  }
  AG_LOGI(TAG, "admin entry aborted: %s", reason);
  _admin_entry_state = AdminEntryState::None;
  _admin_entry_deadline_ms = 0;
  _svc.led.set_charge_done_alert(false);
  // Clear tap history so a partial gesture doesn't bleed into a future one.
  for (uint8_t i = 0; i < ADMIN_TAP_COUNT; ++i) {
    _admin_tap_times[i] = 0;
  }
  _admin_tap_idx = 0;
}

bool Orchestrator::consume_admin_entry_input(const InputEventData &input) {
  if (_admin_entry_state == AdminEntryState::None) {
    return false;
  }
  // Check the per-step deadline first — late inputs abort the sequence.
  const uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  if (_admin_entry_deadline_ms != 0 &&
      static_cast<int32_t>(now_ms - _admin_entry_deadline_ms) >= 0) {
    abort_admin_entry("timeout");
    return true; // consumed (the late input gets dropped along with the abort)
  }
  // Only short-press touch events drive the sequence.  Button events fall
  // through to the normal handler.
  if (input.type != InputType::ShortPress) {
    return false;
  }
  if (input.source != InputSource::TouchUp && input.source != InputSource::TouchDown &&
      input.source != InputSource::TouchEnter) {
    return false;
  }
  switch (_admin_entry_state) {
  case AdminEntryState::ArmedExpectingLeft:
    if (input.source == InputSource::TouchUp) {
      _admin_entry_state = AdminEntryState::ArmedExpectingRight;
      _admin_entry_deadline_ms = now_ms + ADMIN_STEP_TIMEOUT_MS;
      AG_LOGI(TAG, "admin entry: Left ok — press Right");
    } else {
      abort_admin_entry("wrong key (expected Left)");
    }
    return true;
  case AdminEntryState::ArmedExpectingRight:
    if (input.source == InputSource::TouchDown) {
      complete_admin_entry();
    } else {
      abort_admin_entry("wrong key (expected Right)");
    }
    return true;
  case AdminEntryState::None:
  default:
    return false;
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

void Orchestrator::handle_edv_cutoff() {
  if (!_latest_power.edv_cutoff_reached) {
    return;
  }

  // EDV ordering (design §5): when a learning run is mid-Discharge, the
  // "cycle done" marker MUST be persisted+committed to NVS BEFORE ship-mode —
  // otherwise the cycle is silently lost across the power-off.  If the commit
  // fails, do NOT ship: return and retry on the next poll (the cell still has
  // headroom above DW01).
  if (_blearn.stage() == BlearnStage::Discharge) {
    _blearn.tick(_latest_power, static_cast<uint32_t>(RTOS::get_time_ms())); // → CycleDone
    if (!persist_blearn_state()) {
      AG_LOGE(TAG, "EDV: blearn CycleDone commit FAILED — NOT shipping, will retry next poll");
      return;
    }
    AG_LOGI(TAG, "EDV: blearn cycle %u done, committed — proceeding to ship mode",
            _blearn.cycle());
  }

  AG_LOGI(TAG, "EDV cutoff reached — painting discharge-complete screen before ship mode");
  _svc.ui_manager.set_screen(Screen::DischargeComplete);
  update_display();

  // Let the e-paper finish its refresh before the BATFET opens.  Same
  // budget the power-button shutdown path uses.
  RTOS::delay_ms(SHUTDOWN_DISPLAY_DELAY_MS);

  _svc.power_service.trigger_edv_ship_mode(); // BMS QoN — does not return on success
}

// ---------------------------------------------------------------------------
// Automated battery learning (design Part 2)
// ---------------------------------------------------------------------------

bool Orchestrator::persist_blearn_state() {
  _settings.blearn_stage = _blearn.stage();
  _settings.blearn_cycle = _blearn.cycle();
  _settings.blearn_itpor_losses = _blearn.itpor_losses();
  const bool ok = save_blearn_state(_config_store, _blearn.stage(), _blearn.cycle(),
                                    _blearn.itpor_losses());
  if (!ok) {
    AG_LOGE(TAG, "failed to persist blearn state (stage=%d cycle=%u)",
            static_cast<int>(_blearn.stage()), _blearn.cycle());
  }
  return ok;
}

void Orchestrator::run_blearn_verify() {
  PowerService::BlearnVerifyReadout r = _svc.power_service.read_blearn_verify();
  VerifyInputs in{};
  in.reads_ok = r.ok;
  in.itpor = r.itpor;
  in.qmax_up = r.qmax_up;
  in.qmax_mah = r.qmax_mah;
  in.design_capacity_mah = r.design_capacity_mah;
  for (int i = 0; i < BlearnController::RA_TABLE_SIZE; ++i) {
    in.ra[i] = r.ra[i];
  }
  if (_blearn.on_verify_result(in)) {
    // On Complete, restore the gauge's normal bounded change limits (§10.3);
    // a failed verify leaves them lifted for the next cycle.
    if (_blearn.stage() == BlearnStage::Complete) {
      _svc.power_service.set_learning_update_status(false);
    }
    persist_blearn_state();
  }
}

void Orchestrator::apply_blearn_action(const BlearnAction &action) {
  // Charge control: enable/disable + current.  set_manual_charge_disabled is
  // the charge-off knob already used by the legacy learning UX.
  _svc.power_service.set_manual_charge_disabled(!action.set_charge_enabled);
  if (action.set_charge_enabled && action.charge_current_ma != 0) {
    _svc.power_service.set_charge_current_ma(action.charge_current_ma);
  }

  // Load polarity (design §4): Rest wants quiet load (LOW_POWER on), Discharge
  // wants full load (LOW_POWER off).  On v0.3 the SPS30 is quieted via its I2C
  // Sleep command (the +5 V PMID rail can't be collapsed in firmware), keyed on
  // the FSM's explicit per-stage request rather than on plug state.
  if (action.low_power && !_in_learning_low_power) {
    AG_LOGI(TAG, "blearn: entering LOW_POWER (quiet load for OCV)");
    _svc.sensor_producer.request_pm_sleep();
    _svc.gps_service.sleep_for_ms(LEARNING_GPS_SLEEP_MS);
    _svc.sensor_producer.request_low_power(true);
    _svc.ble_service.deinit();
    _in_learning_low_power = true;
  } else if (!action.low_power && _in_learning_low_power) {
    AG_LOGI(TAG, "blearn: exiting LOW_POWER (full load for discharge)");
    // Clear the flag first so the prepare's wake+warmup isn't gated out by
    // the LOW_POWER pre-wake guard in check_timers(), and so the GPS helpers
    // below act instead of deferring to learning.
    _in_learning_low_power = false;
    // Restore the GPS to the current mode/tracking state rather than blindly
    // waking it: an OnWhenTracking-idle device must stay parked in CFG-SLEEP.
    if (is_gps_active()) {
      activate_gps();
    } else {
      deactivate_gps();
    }
    _svc.sensor_producer.request_low_power(false);
    _svc.sensor_producer.request_prepare();
    _pm_prepare_sent = true;
    init_ble_if_portable();
  }

  // Unplug cue — reuse the LED8 charge-done blinker.
  _svc.led.set_charge_done_alert(action.unplug_cue);

  // Phase screen — only when on a non-menu screen so we don't fight the user
  // navigating the admin menu.
  if (!_svc.ui_manager.is_on_menu_screen()) {
    _svc.ui_manager.set_screen(action.screen);
  }

  if (action.persist_stage) {
    persist_blearn_state();
  }

  if (action.run_verify) {
    run_blearn_verify();
  }

  // commit_then_ship is handled by handle_edv_cutoff() (the EDV poll path),
  // which owns the persist-before-ship ordering (§5).
}

bool Orchestrator::tick_blearn() {
  const uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  BlearnAction action = _blearn.tick(_latest_power, now_ms);
  if (!action.active && !action.persist_stage) {
    return false;
  }
  apply_blearn_action(action);
  return action.active;
}

void Orchestrator::resume_blearn_on_boot() {
  _blearn.load(_settings.blearn_stage, _settings.blearn_cycle, _settings.blearn_itpor_losses);
  if (_blearn.resume_on_boot(_latest_power)) {
    AG_LOGI(TAG, "blearn boot-resume: stage=%d cycle=%u itpor_losses=%u",
            static_cast<int>(_blearn.stage()), _blearn.cycle(), _blearn.itpor_losses());
    persist_blearn_state();
  }
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
      activate_gps();
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

  // Power dashboard shows the live FG readout (SOC, V, I, FCC drift) during
  // learning.  Visible for the legacy admin opt-in AND for any active
  // automated-learning phase, so the operator sees the live values rather than
  // a bare phase label.  Production users (admin off, no run) never see it.
  const BlearnStage bstage = _blearn.stage();
  const bool blearn_active = bstage == BlearnStage::Charge || bstage == BlearnStage::Rest ||
                             bstage == BlearnStage::Discharge || bstage == BlearnStage::Verify;
  const bool show_power_dashboard =
      (_settings.admin_mode && _settings.battery_learning_enabled) || blearn_active;
  PowerDashboardData dash{};
  if (show_power_dashboard) {
    const auto &t = _latest_power.telemetry;
    dash.valid = _latest_power.fg_valid;
    dash.soc_pct = _latest_power.fg_soc_pct;
    dash.voltage_mv = _latest_power.fg_voltage_mv;
    dash.current_ma = _latest_power.fg_current_ma;
    dash.remaining_mah = _latest_power.fg_remaining_mah;
    dash.full_charge_mah = _latest_power.fg_full_charge_mah;
    dash.temperature_c = _latest_power.fg_temperature_c;
    dash.flag_fc = _latest_power.fg_flag_fc;
    dash.flag_chg = _latest_power.fg_flag_chg;
    dash.flag_dsg = _latest_power.fg_flag_dsg;
    dash.vsys_mv = t.system_voltage_mv;
    dash.vpmid_mv = t.pmid_voltage_mv;
    dash.charge_current_ma = _settings.charge_current_ma;
    dash.bms_charging_state = static_cast<uint8_t>(_latest_power.charging_status);
    dash.low_power_active = _in_learning_low_power;
    const bool plugged =
        bms_power_source_has_external_input(_latest_power.charger_status.power_source);
    dash.plugged_in = plugged;
    switch (bstage) {
    case BlearnStage::Charge:
      dash.blearn_phase = "CHARGING";
      break;
    case BlearnStage::Rest:
      dash.blearn_phase = "RESTING";
      break;
    case BlearnStage::Discharge:
      // Discharge stage raises the unplug cue; once the operator has unplugged,
      // show the actual discharge state instead of a stale prompt.
      dash.blearn_phase = plugged ? "UNPLUG NOW" : "DISCHARGING";
      break;
    case BlearnStage::Verify:
      dash.blearn_phase = "VERIFYING";
      break;
    default:
      break;
    }
  }

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
      .show_power_dashboard = show_power_dashboard,
      .power_dashboard = dash,
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

void Orchestrator::activate_gps() {
  // Learning owns the GPS while it holds the module in CFG-SLEEP for its own
  // OCV-quiet-load reasons.  Don't fight it — the learning-exit path restores
  // the correct GPS state for the current mode/tracking once it releases.
  if (_in_learning_low_power) {
    AG_LOGI(TAG, "activate_gps: deferred (learning owns GPS)");
    return;
  }

  // If the module is parked in CFG-SLEEP (entered via deactivate_gps in the
  // OnWhenTracking-idle case), pulse the I/O-expander wake line first so it is
  // listening before we (re)open the UART.  wake_from_sleep() is a no-op when
  // no wake handler is registered (e.g. TCA9536 init failed) — in that case the
  // module can only self-wake on its CFG-SLEEP timer, so start() below may sit
  // on a 9600-baud link until the GpsService task's deadline resync recovers
  // it.  No silent failure: the no-handler case is logged by the driver.
  _svc.gps_service.wake_from_sleep();
  _svc.gps_service.start();
}

void Orchestrator::deactivate_gps() {
  _latest_gps = GpsData{};

  // Learning owns the GPS while _in_learning_low_power: it has already put the
  // module into its own CFG-SLEEP and will restore state on exit.  Issuing our
  // own stop/sleep here would race the learning sleep/wake bookkeeping.
  if (_in_learning_low_power) {
    AG_LOGI(TAG, "deactivate_gps: deferred (learning owns GPS)");
    return;
  }

  // Power-state matrix lives here so it can't drift across the four transition
  // sites (start/stop_tracking, apply_settings_change, change_mode, BLE set).
  if (_settings.gps_mode == GpsMode::OnWhenTracking) {
    // Not tracking but may track again soon — park the receiver in CFG-SLEEP so
    // the expander pulse can wake it cheaply, rather than a full GNSS stop.
    AG_LOGI(TAG, "deactivate_gps: OnWhenTracking idle -> CFG-SLEEP (%u h)",
            GPS_NOT_TRACKING_SLEEP_MS / 3600000U);
    _svc.gps_service.sleep_for_ms(GPS_NOT_TRACKING_SLEEP_MS);
  } else {
    // AlwaysOff (the only other mode that reaches here) — fully stop the GNSS
    // receiver. AlwaysOn never deactivates (is_gps_active() stays true).
    AG_LOGI(TAG, "deactivate_gps: GPS mode off -> stop_and_idle_gnss");
    _svc.gps_service.stop_and_idle_gnss();
  }
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
