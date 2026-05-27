/**
 * AirGradient Go — Power Management Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

// ---------------------------------------------------------------------------
// Platform compatibility — RTC_DATA_ATTR
//
// On ESP-IDF, RTC_DATA_ATTR places a variable in RTC slow memory so it
// survives deep sleep.  In host test builds the attribute does not exist;
// define it away so the translation unit compiles as a regular static.
// ---------------------------------------------------------------------------

#if !defined(TEST_HOST) && defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif

// ---------------------------------------------------------------------------
// Platform compatibility — ESP-IDF sleep APIs
// ---------------------------------------------------------------------------

#ifndef TEST_HOST
#include "driver/gpio.h"
#include "esp_sleep.h"
#endif

#include "drivers/bq27427/bq27427.h"
#include "go_power.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "ag_log.h"
#include "common.h"

static constexpr const char *TAG = "PowerService";

// ---------------------------------------------------------------------------
// RTC state storage
//
// These two variables live in RTC slow memory (survives deep sleep).
// Under TEST_HOST the RTC_DATA_ATTR is defined away, making them ordinary
// file-scope statics with the same semantics for unit testing.
// ---------------------------------------------------------------------------

RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PowerService::PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

// ---------------------------------------------------------------------------
// BMS operations
// ---------------------------------------------------------------------------

PowerSnapshot PowerService::poll_bms() {
  PowerSnapshot status{};

  // --- BMS telemetry (BQ25629) ---
  BmsTelemetry telemetry{};
  if (_bms.read_telemetry(telemetry)) {
    if (telemetry.is_battery_voltage_valid()) {
      status.battery_voltage = telemetry.battery_voltage;
    }
    if (telemetry.is_charging_voltage_valid()) {
      status.charging_voltage = telemetry.charging_voltage;
    }
    status.telemetry = telemetry;
  } else {
    AG_LOGW(TAG, "BMS read_telemetry() failed");
  }

  // --- Fuel gauge snapshot (BQ27427) — single batch of reads ---
  struct FgSnapshot {
    bool present = false;
    bool soc_ok = false, v_ok = false, i_ok = false, p_ok = false;
    bool rem_ok = false, fcc_ok = false, t_ok = false, flags_ok = false;
    uint8_t soc = 0;
    uint16_t v_mv = 0;
    int16_t i_ma = 0, p_mw = 0;
    uint16_t rem_mah = 0, fcc_mah = 0;
    float t_c = 0.0f;
    uint16_t flags = 0;
    // BQ27427 TRM §5.5 Flags() bit positions (low byte first, then high):
    //   bit 0 = DSG (discharging)
    //   bit 1 = SOCF, bit 2 = SOC1, bit 3 = BAT_DET
    //   bit 4 = CFGUPMODE (gauge is in CONFIG UPDATE mode — NOT a charging bit;
    //               a previous bug misread this bit as "charging")
    //   bit 5 = ITPOR, bit 7 = OCVTAKEN
    //   bit 8 = CHG ((fast) charging ALLOWED — cell not full; a charger hint,
    //               NOT a measurement that current is flowing in.  Tracks ~!FC,
    //               can be set while DSG=1.  Use DSG for current direction.)
    //   bit 9 = FC  (Full Charge)
    bool fc() const { return flags_ok && (flags & (1u << 9)); }
    bool chg() const { return flags_ok && (flags & (1u << 8)); }
    bool dsg() const { return flags_ok && (flags & (1u << 0)); }
    // OCVTAKEN — sticky bit set by the gauge once it has captured an OCV
    // sample during RELAXATION.  Used to verify Phase 2 (OCV1) and Phase 4
    // (OCV2) of the BQ27427 learning cycle without bqStudio.
    bool ocv_taken() const { return flags_ok && (flags & (1u << 7)); }
    bool itpor() const { return flags_ok && (flags & (1u << 5)); }
    bool cfgupmode() const { return flags_ok && (flags & (1u << 4)); }
    bool bat_det() const { return flags_ok && (flags & (1u << 3)); }
    // CONTROL_STATUS() via Control(0x0000); same byte layout as Flags().
    // TRM §5.1.1 Table 5-3.  QMAX_UP/RES_UP are both cleared after a POR, so
    // both set = Qmax + Ra learned AND no POR since last learn.
    bool ctrl_ok = false;
    uint16_t ctrl = 0;
    bool qmax_up() const { return ctrl_ok && (ctrl & (1u << 9)); }
    bool res_up() const { return ctrl_ok && (ctrl & (1u << 8)); }
    bool vok() const { return ctrl_ok && (ctrl & (1u << 1)); }
  } fg;
  if (_fuel_gauge != nullptr) {
    fg.present = true;
    fg.soc_ok = _fuel_gauge->read_soc_percent(fg.soc);
    fg.v_ok = _fuel_gauge->read_voltage_mv(fg.v_mv);
    fg.i_ok = _fuel_gauge->read_average_current_ma(fg.i_ma);
    fg.p_ok = _fuel_gauge->read_average_power_mw(fg.p_mw);
    fg.rem_ok = _fuel_gauge->read_remaining_capacity_mah(fg.rem_mah);
    fg.fcc_ok = _fuel_gauge->read_full_charge_capacity_mah(fg.fcc_mah);
    fg.t_ok = _fuel_gauge->read_internal_temperature_c(fg.t_c);
    fg.flags_ok = _fuel_gauge->read_flags(fg.flags);
    fg.ctrl_ok = _fuel_gauge->control_subcommand(0x0000, fg.ctrl);
  }

  // --- SOC source: prefer FG; fall back to BMS voltage curve ---
  float pct = -1.0f;
  bool soc_from_fg = false;
  if (fg.soc_ok) {
    pct = static_cast<float>(fg.soc);
    soc_from_fg = true;
  } else if (!_bms.get_battery_percentage(&pct)) {
    AG_LOGW(TAG, "battery percentage unavailable (fg=%s)",
            fg.present ? "read_failed" : "absent");
  }
  if (pct >= 0.0f) {
    status.battery_percentage = pct;
    status.critical = (pct < BATTERY_CRITICAL_PERCENT);
  }

  // --- BMS status (charge state, power source, regulation flags) ---
  BmsStatus bms_status{};
  bool on_battery = false;
  if (_bms.read_status(bms_status)) {
    status.charging_status = bms_status.charging_state;
    status.charger_status = bms_status;
    on_battery = !bms_power_source_has_external_input(bms_status.power_source);
    if (!sync_pmid_mode(bms_status.power_source)) {
      AG_LOGW(TAG, "failed to sync PMID mode for source %s",
              bms_power_source_str(bms_status.power_source));
    }
  }

  // --- Battery over-temperature protection (TS-pin NTC, software-driven) ---
  // The driver gives us a Steinhart-Hart-converted cell temperature in
  // telemetry.battery_temperature_c.  Sentinel (-999.0f) means the TS read
  // was out of range or unavailable — never act on it.  Hysteresis avoids
  // chattering at the 50 °C boundary; ship-mode latch avoids retriggering
  // ship mode I²C writes during the BATFET_DLY (~12.5 s) shutdown window.
  bool thermal_charge_disable_now = _thermal_charge_disabled;
  if (status.telemetry.is_battery_temperature_valid()) {
    const float t_batt = status.telemetry.battery_temperature_c;

    if (!_thermal_ship_mode_triggered && t_batt >= SHIP_MODE_HOT_C) {
      AG_LOGE(TAG,
              "BATTERY OVER-TEMP %.1f°C >= %.1f°C — triggering ship mode "
              "(SYS/PMID will drop after t_BATFET_DLY)",
              t_batt, SHIP_MODE_HOT_C);
      // Cut charge first so the cell isn't pushed during the shutdown window.
      if (_charge_enabled) {
        if (_bms.set_charge_enable(false)) {
          _charge_enabled = false;
        }
      }
      if (_bms.enter_ship_mode()) {
        _thermal_ship_mode_triggered = true;
        _thermal_charge_disabled = true;
        thermal_charge_disable_now = true;
      } else {
        AG_LOGE(TAG, "enter_ship_mode() failed during thermal trip");
      }
    } else if (!_thermal_charge_disabled && t_batt >= CHARGE_HOT_CUTOFF_C) {
      AG_LOGW(TAG, "BATTERY HOT %.1f°C >= %.1f°C — disabling charge",
              t_batt, CHARGE_HOT_CUTOFF_C);
      _thermal_charge_disabled = true;
      thermal_charge_disable_now = true;
    } else if (_thermal_charge_disabled && !_thermal_ship_mode_triggered &&
               t_batt <= CHARGE_HOT_RESUME_C) {
      AG_LOGI(TAG, "BATTERY cooled %.1f°C <= %.1f°C — charge re-armed",
              t_batt, CHARGE_HOT_RESUME_C);
      _thermal_charge_disabled = false;
      thermal_charge_disable_now = false;
    }
  }

  // --- Over-discharge protection (EDV cutoff -> ship mode) ---
  // The +3.1V buck-boost holds the system rail steady as the cell sags, so on
  // battery there is no brownout to throttle the load — the cell would be
  // dragged to the pack DW01 trip (~2.4V), cutting PACK+ and PORing the
  // BQ27427 (wiping its learning).  Cut the system into ship mode while the
  // cell still has headroom: EDV_SHIP_MV under load relaxes to ~3.0V OCV once
  // the BATFET opens, above DW01.  Same enter_ship_mode() primitive the power
  // button uses (PowerService::shutdown()).  Debounced to ride out transient
  // load dips; skipped on USB (the charger ignores ship mode with VBUS).
  uint16_t cell_mv = 0;
  bool cell_mv_ok = false;
  if (fg.v_ok) {
    cell_mv = fg.v_mv;
    cell_mv_ok = true;
  } else if (status.telemetry.is_battery_voltage_valid()) {
    cell_mv = static_cast<uint16_t>(status.telemetry.battery_voltage * 1000.0f);
    cell_mv_ok = true;
  }
  if (!_edv_ship_mode_triggered && on_battery && cell_mv_ok) {
    if (cell_mv < EDV_SHIP_MV) {
      if (++_edv_low_count >= EDV_SHIP_DEBOUNCE_SAMPLES) {
        AG_LOGE(TAG,
                "VBAT %umV < EDV cutoff %umV for %u polls — discharge complete, "
                "signalling ship mode (relaxed OCV ~3.0V, above DW01)",
                cell_mv, EDV_SHIP_MV, _edv_low_count);
        // Defer the actual ship-mode entry to the orchestrator so the
        // "Discharge complete" e-paper frame lands before the BATFET opens.
        // trigger_edv_ship_mode() sets _edv_ship_mode_triggered on success.
        status.edv_cutoff_reached = true;
      }
    } else {
      _edv_low_count = 0;
    }
  }

  // --- Auto-disable charging when the cell is full ---
  // FG declares Full Charge (FC flag) when voltage reaches Charge Voltage
  // AND |current| drops below Taper Rate.  When the admin opt-in
  // `_charge_cutoff_at_full` is set, we clear the BMS's EN_CHG bit so the
  // cell isn't held at 100 % while USB stays plugged in (better for
  // long-term cell health).  When the cell self-discharges or load
  // drains it enough for FC to clear, we re-enable.  When the opt-in is
  // off (production default), the charger is always left enabled — the
  // BMS itself stops charging once the cell is full but resumes top-ups.
  // Edge-triggered — one I²C write per state change.
  //
  // Thermal cutoff always wins: if the cell is hot, want_charge is forced
  // false regardless of FC state, and FC-based re-enable is suppressed
  // until the cell cools back below CHARGE_HOT_RESUME_C.
  if (fg.flags_ok || thermal_charge_disable_now || _manual_charge_disabled) {
    bool want_charge;
    if (thermal_charge_disable_now) {
      want_charge = false;
    } else if (_manual_charge_disabled) {
      want_charge = false;
    } else {
      want_charge = _charge_cutoff_at_full ? !fg.fc() : true;
    }
    if (want_charge != _charge_enabled) {
      if (_bms.set_charge_enable(want_charge)) {
        _charge_enabled = want_charge;
        AG_LOGI(TAG, "charging %s (cutoff=%d FC=%d thermal_hot=%d manual_dis=%d)",
                want_charge ? "ENABLED" : "DISABLED", _charge_cutoff_at_full,
                fg.flags_ok ? fg.fc() : 0, thermal_charge_disable_now,
                _manual_charge_disabled);
      } else {
        AG_LOGW(TAG, "set_charge_enable(%d) failed", want_charge);
      }
    }
  }

  // --- Compact two-line log ---
  // Line 1: BMS view (USB, charger state, currents on the system rail)
  const auto &t = status.telemetry;
  AG_LOGI(TAG,
          "BMS  chg=%s en=%d chg_dis=%d src=%s vbus=%.2fV ibus=%dmA ibat=%+dmA "
          "vsys=%umV vpmid=%umV tdie=%d°C",
          bms_charging_state_str(status.charger_status.charging_state),
          _charge_enabled, _manual_charge_disabled,
          bms_power_source_str(status.charger_status.power_source),
          status.charging_voltage, t.input_current_ma, t.battery_current_ma,
          t.system_voltage_mv, t.pmid_voltage_mv, t.die_temperature_c);

  // Line 2: FG view (SOC, cell V/I/P, capacity, key flags) when attached.
  if (fg.present) {
    AG_LOGI(TAG,
            "FG   SOC=%s%u%% V=%s%.3fV I=%s%+dmA P=%s%+dmW "
            "rem=%s%u/%s%umAh T=%s%.1f°C [FC=%d CHG=%d DSG=%d OCV=%d "
            "ITPOR=%d CFGUP=%d BATDET=%d] src=%s",
            fg.soc_ok ? "" : "?", fg.soc,
            fg.v_ok ? "" : "?", fg.v_mv / 1000.0f,
            fg.i_ok ? "" : "?", fg.i_ma,
            fg.p_ok ? "" : "?", fg.p_mw,
            fg.rem_ok ? "" : "?", fg.rem_mah,
            fg.fcc_ok ? "" : "?", fg.fcc_mah,
            fg.t_ok ? "" : "?", fg.t_c,
            fg.fc(), fg.chg(), fg.dsg(), fg.ocv_taken(),
            fg.itpor(), fg.cfgupmode(), fg.bat_det(),
            soc_from_fg ? "FG" : "BMS");
  }

  // Line 3: Impedance-Track learning telemetry (bench diagnostic).  QMAX_UP/
  // RES_UP/VOK from CONTROL_STATUS; OCV-window zone from the Qmax-invalid band.
  // Ungated: poll_bms() has no admin/settings access and Line 2 above is
  // likewise always-on.
  // TODO(go): also log Update Status (DM subclass 82, offset 0) once its
  // register location is confirmed for the BQ27427.
  if (fg.present) {
    const char *ocv_zone = !fg.v_ok ? "V?"
        : (fg.v_mv > Q_INVALID_MAX_MV) ? "ABOVE-FLAT(>3811:OCV1-zone)"
        : (fg.v_mv < Q_INVALID_MIN_MV) ? "BELOW-FLAT(<3750:OCV2-zone)"
                                       : "FLAT(Qmax-invalid)";
    AG_LOGI(TAG, "FG-LRN QMAX_UP=%s RES_UP=%s VOK=%s | V=%s%umV %s",
            fg.ctrl_ok ? (fg.qmax_up() ? "1" : "0") : "?",
            fg.ctrl_ok ? (fg.res_up() ? "1" : "0") : "?",
            fg.ctrl_ok ? (fg.vok() ? "1" : "0") : "?",
            fg.v_ok ? "" : "?", fg.v_mv, ocv_zone);
  }

  // One-shot dump of the learned Impedance-Track golden values (Qmax Cell 0 +
  // Ra grid) for sanity-checking before the gauge is SEALed.  Pure Data Memory
  // reads — no CFGUPDATE, so this never perturbs learning.  Fires once per
  // boot to stay off the 10 s poll cadence.
  if (fg.present && !_fg_learned_dumped) {
    log_fg_learned_dump();
    _fg_learned_dumped = true;
  }

  // Regulation flags only when something is actually active — these are
  // important when they happen but pure noise when they aren't.
  const auto &cs = status.charger_status;
  if (cs.thermal_regulation || cs.vsys_regulation || cs.input_current_regulation ||
      cs.input_voltage_regulation || cs.safety_timer_expired || cs.watchdog_expired) {
    AG_LOGW(TAG, "BMS regulation: treg=%d vsys=%d iindpm=%d vindpm=%d safety=%d wd=%d",
            cs.thermal_regulation, cs.vsys_regulation, cs.input_current_regulation,
            cs.input_voltage_regulation, cs.safety_timer_expired, cs.watchdog_expired);
  }

  // Preserve the FG snapshot for the admin-mode power dashboard.  Marked
  // valid only when the gauge was present AND every read in this poll
  // succeeded — partial failures leave fg_valid = false so the dashboard
  // shows a "no data" state instead of a mix of fresh and stale numbers.
  if (fg.present && fg.soc_ok && fg.v_ok && fg.i_ok && fg.rem_ok && fg.fcc_ok &&
      fg.t_ok && fg.flags_ok) {
    status.fg_valid = true;
    status.fg_soc_pct = fg.soc;
    status.fg_voltage_mv = fg.v_mv;
    status.fg_current_ma = fg.i_ma;
    status.fg_remaining_mah = fg.rem_mah;
    status.fg_full_charge_mah = fg.fcc_mah;
    status.fg_temperature_c = fg.t_c;
    status.fg_flag_fc = fg.fc();
    status.fg_flag_chg = fg.chg();
    status.fg_flag_dsg = fg.dsg();
  }

  return status;
}

void PowerService::log_fg_learned_dump() {
  if (_fuel_gauge == nullptr) {
    return;
  }

  // Qmax Cell 0 is a fixed-point value; convert to mAh with the learned
  // Design Capacity (TRM §7.4.2.3.1): Qmax(mAh) = raw * DC / 2^14.
  uint16_t qmax_raw = 0;
  uint16_t dc_mah = 0;
  const bool qmax_ok = _fuel_gauge->read_qmax_cell0(qmax_raw);
  const bool dc_ok = _fuel_gauge->read_design_capacity_mah(dc_mah);
  if (qmax_ok && dc_ok) {
    const uint32_t qmax_mah = (static_cast<uint32_t>(qmax_raw) * dc_mah) / 16384u;
    AG_LOGI(TAG, "FG-DUMP Qmax Cell 0 raw=%u -> %" PRIu32 "mAh (DC=%umAh)", qmax_raw,
            qmax_mah, dc_mah);
  } else {
    AG_LOGW(TAG, "FG-DUMP Qmax read failed (qmax_ok=%d dc_ok=%d)", qmax_ok, dc_ok);
  }

  int16_t ra[BQ27427::RA_TABLE_SIZE] = {};
  if (!_fuel_gauge->read_ra_table(ra)) {
    AG_LOGW(TAG, "FG-DUMP Ra table read failed");
    return;
  }
  AG_LOGI(TAG, "FG-DUMP Ra[0..14] = %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
          ra[0], ra[1], ra[2], ra[3], ra[4], ra[5], ra[6], ra[7], ra[8], ra[9], ra[10],
          ra[11], ra[12], ra[13], ra[14]);

  // Sanity verdict (TRM §7.4.3): a healthy grid has no non-positive entries
  // and smooth grid-to-grid transitions — flag the largest adjacent jump.
  int bad_count = 0;
  int max_jump = 0;
  int max_jump_idx = 0;
  for (int i = 0; i < BQ27427::RA_TABLE_SIZE; ++i) {
    if (ra[i] <= 0) {
      ++bad_count;
    }
    if (i > 0) {
      int jump = ra[i] - ra[i - 1];
      if (jump < 0) {
        jump = -jump;
      }
      if (jump > max_jump) {
        max_jump = jump;
        max_jump_idx = i;
      }
    }
  }
  if (bad_count > 0) {
    AG_LOGW(TAG, "FG-DUMP Ra SANITY: %d non-positive value(s) -- table looks bad", bad_count);
  } else {
    AG_LOGI(TAG, "FG-DUMP Ra SANITY: ok (no non-positive), max grid jump=%d at Ra[%d]->Ra[%d]",
            max_jump, max_jump_idx - 1, max_jump_idx);
  }
}

bool PowerService::poll_charging_status(BmsChargingState &state) {
  if (!_bms.get_charging_state(state)) {
    AG_LOGW(TAG, "poll_charging_status: get_charging_state() failed");
    return false;
  }
  return true;
}

bool PowerService::poll_status(BmsStatus &status) {
  status = BmsStatus{};
  if (!_bms.read_status(status)) {
    AG_LOGW(TAG, "poll_status: read_status() failed");
    return false;
  }

  if (!sync_pmid_mode(status.power_source)) {
    AG_LOGW(TAG, "poll_status: failed to sync PMID mode for source %s",
            bms_power_source_str(status.power_source));
    return false;
  }

  return true;
}

bool PowerService::reset_watchdog() {
  const bool ok = _bms.update_watchdog();
  if (!ok) {
    AG_LOGW(TAG, "reset_watchdog: update_watchdog() failed");
  }
  return ok;
}

bool PowerService::set_charge_current_ma(uint16_t current_ma) {
  if (current_ma != 0 && current_ma == _charge_current_ma) {
    return true;
  }
  if (!_bms.set_charge_current_ma(current_ma)) {
    AG_LOGW(TAG, "set_charge_current_ma(%u) failed", static_cast<unsigned>(current_ma));
    return false;
  }
  AG_LOGI(TAG, "charge current -> %u mA", static_cast<unsigned>(current_ma));
  _charge_current_ma = current_ma;
  return true;
}

void PowerService::shutdown() {
#ifndef TEST_HOST
  AG_LOGI(TAG, "shutdown: entering BMS ship mode (QoN)");
  if (!_bms.enter_ship_mode()) {
    AG_LOGE(TAG, "shutdown: enter_ship_mode failed — falling back to deep sleep");
  }
  // enter_ship_mode() cuts system power and should not return.
  // If it does (failed or unexpected return), fall back to deep sleep with
  // GPIO wake sources so the user can press a button to try powering on again.
  // No timer is configured — this is a shutdown, not a scheduled wake cycle.
  uint64_t wake_mask = 0;
  if (_config.pin_wake_button_power >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_power;
  }
  if (_config.pin_wake_button_boot >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_boot;
  }
  if (wake_mask != 0) {
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_deep_sleep_start();
#endif
}

bool PowerService::trigger_edv_ship_mode() {
#ifndef TEST_HOST
  if (_bms.enter_ship_mode()) {
    _edv_ship_mode_triggered = true;
    return true;
  }
  AG_LOGE(TAG, "enter_ship_mode() failed during EDV trip");
  return false;
#else
  return false;
#endif
}

// ---------------------------------------------------------------------------
// External watchdog
// ---------------------------------------------------------------------------

void PowerService::init_ext_watchdog() {
  if (_config.pin_ext_wdt < 0) {
    return;
  }
  if (!ext_watchdog_init(_gpio, _config.pin_ext_wdt)) {
    AG_LOGE(TAG, "init_ext_watchdog: GPIO %d config failed", _config.pin_ext_wdt);
  }
}

void PowerService::reset_ext_watchdog() {
  if (_config.pin_ext_wdt < 0) {
    return;
  }
  ext_watchdog_reset(_gpio, _config.pin_ext_wdt);
}

// ---------------------------------------------------------------------------
// RTC state persistence
// ---------------------------------------------------------------------------

void PowerService::save_state(const RtcAppState &state) {
  memcpy(&s_rtc_state, &state, sizeof(RtcAppState));
  s_rtc_state_valid = true;
}

RtcAppState PowerService::load_state() const {
  if (!s_rtc_state_valid) {
    // Return defaults: Offline mode, Idle behavior, Locked, GPS on.
    return RtcAppState{};
  }
  RtcAppState out{};
  memcpy(&out, &s_rtc_state, sizeof(RtcAppState));
  return out;
}

// ---------------------------------------------------------------------------
// decide_sleep — pure logic (no platform dependencies)
// ---------------------------------------------------------------------------

PowerService::SleepDecision PowerService::decide_sleep(const GoSettings &settings,
                                                       LockState lock_state, OperatingMode mode,
                                                       uint32_t awake_ms) const {
  // Only Offline mode enters sleep; Portable and Stationary stay awake.
  if (mode != OperatingMode::Offline) {
    return {SleepType::None, 0};
  }

  if (lock_state == LockState::Unlocked) {
    return {SleepType::None, 0};
  }

  uint32_t interval_ms = static_cast<uint32_t>(settings.measure_interval_seconds) * 1000;

  // Subtract time already spent awake so total cycle matches the interval
  uint32_t sleep_ms = (awake_ms < interval_ms) ? (interval_ms - awake_ms) : 0;

  if (sleep_ms >= static_cast<uint32_t>(_config.deep_sleep_threshold_ms)) {
    return {SleepType::Deep, sleep_ms};
  }
  // Interval too short: deep sleep overhead (~3–4 s reboot) exceeds the
  // sleep duration.  Stay awake and let the main loop run normally.
  return {SleepType::None, 0};
}

// ---------------------------------------------------------------------------
// PM sensor hold — pure logic (no platform dependencies)
// ---------------------------------------------------------------------------

bool PowerService::should_hold_pm_sensor(uint32_t sleep_duration_ms) const {
  return _config.pin_pm_power >= 0 && sleep_duration_ms < _config.sensor_hold_max_sleep_ms;
}

bool PowerService::should_sleep_pm_sensor(uint32_t measure_interval_ms) const {
  return _config.pin_pm_power >= 0 && measure_interval_ms >= _config.pm_sleep_threshold_ms;
}

void PowerService::set_pm_power(bool on) {
  if (_config.pin_pm_power < 0) {
    return;
  }
  _gpio.set_level(_config.pin_pm_power, on ? 0 : 1);
  AG_LOGI(TAG, "set_pm_power: %s", on ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// Sleep entry — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

void PowerService::enter_sleep(uint32_t sleep_duration_ms) {
#ifndef TEST_HOST
  // Hold PM sensor power GPIO during short sleeps so the sensor stays warm
  // and the next fast-path boot can skip the 10 s warmup.
  if (should_hold_pm_sensor(sleep_duration_ms)) {
    auto pin = static_cast<gpio_num_t>(_config.pin_pm_power);
    gpio_hold_en(pin);
    AG_LOGI(TAG, "enter_sleep: holding PM power GPIO %d for warm wake", _config.pin_pm_power);
  }

  AG_LOGI(TAG, "enter_sleep: entering deep sleep for %" PRIu32 " ms", sleep_duration_ms);
  configure_wake_sources(sleep_duration_ms);
  esp_deep_sleep_start();
  // Does not return — CPU reboots on wake.
#endif
}

// ---------------------------------------------------------------------------
// Wake source configuration — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

void PowerService::configure_wake_sources(uint32_t timer_ms) {
#ifndef TEST_HOST
  // Timer wake: convert milliseconds to microseconds for the ESP-IDF API.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_ms) * 1000ULL);

  // GPIO wake: both buttons use EXT1 which supports multiple GPIOs in a
  // single bitmask.  The target (ESP32-C5) does not have EXT0; EXT1 is the
  // correct deep-sleep GPIO wake source.  Buttons are active-low, so wake
  // fires when ANY selected GPIO is pulled low.
  uint64_t wake_mask = 0;
  if (_config.pin_wake_button_power >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_power;
  }
  if (_config.pin_wake_button_boot >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_boot;
  }
  if (wake_mask != 0) {
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
#endif
}

bool PowerService::sync_pmid_mode(BmsPowerSource power_source) {
  // Battery Learning override: when forced, never promote PMID to Boost on
  // cell power — keeps the +5 V rail dead so SPS30 doesn't draw during the
  // gauge's Relax window.
  const BmsPmidMode auto_mode = bms_power_source_has_external_input(power_source)
                                    ? BmsPmidMode::PassThrough
                                    : BmsPmidMode::Boost;
  const BmsPmidMode desired_mode =
      _force_pmid_passthrough ? BmsPmidMode::PassThrough : auto_mode;

  if (_pmid_mode == desired_mode) {
    return true;
  }

  if (!_bms.configure_pmid_mode(desired_mode)) {
    AG_LOGW(TAG, "sync_pmid_mode: configure_pmid_mode(%s) failed", bms_pmid_mode_str(desired_mode));
    return false;
  }

  AG_LOGI(TAG, "sync_pmid_mode: %s for power source %s%s", bms_pmid_mode_str(desired_mode),
          bms_power_source_str(power_source), _force_pmid_passthrough ? " (forced)" : "");
  _pmid_mode = desired_mode;
  return true;
}

void PowerService::set_manual_charge_disabled(bool disabled) {
  if (disabled == _manual_charge_disabled) {
    return;
  }
  _manual_charge_disabled = disabled;
  AG_LOGI(TAG, "manual_charge_disabled: %s (BMS reconcile on next poll)",
          disabled ? "ON" : "OFF");
}

void PowerService::set_force_pmid_passthrough(bool force) {
  if (force == _force_pmid_passthrough) {
    return;
  }
  _force_pmid_passthrough = force;
  AG_LOGI(TAG, "force_pmid_passthrough: %s", force ? "ON" : "OFF");

  // Apply immediately when forcing — otherwise the SPS30 rail stays hot
  // until the next sync_pmid_mode() tick.  When releasing the force, leave
  // the rail in its current state; the next sync (within
  // BMS_STATUS_POLL_INTERVAL_MS) will auto-recompute the correct mode.
  if (force && _pmid_mode != BmsPmidMode::PassThrough) {
    if (_bms.configure_pmid_mode(BmsPmidMode::PassThrough)) {
      _pmid_mode = BmsPmidMode::PassThrough;
      AG_LOGI(TAG, "force_pmid_passthrough: PMID -> PassThrough (immediate)");
    } else {
      AG_LOGW(TAG, "force_pmid_passthrough: immediate apply failed");
    }
  }
}

// ---------------------------------------------------------------------------
// Boot path — static helpers
// ---------------------------------------------------------------------------

// static
WakeCause PowerService::get_wake_cause() {
#ifndef TEST_HOST
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  switch (cause) {
  case ESP_SLEEP_WAKEUP_TIMER:
    return WakeCause::Timer;
  case ESP_SLEEP_WAKEUP_EXT0:
  case ESP_SLEEP_WAKEUP_EXT1:
  case ESP_SLEEP_WAKEUP_GPIO:
    return WakeCause::Button;
  default:
    // ESP_SLEEP_WAKEUP_UNDEFINED = first power-on (not a wake from sleep).
    return WakeCause::PowerOn;
  }
#else
  return WakeCause::PowerOn;
#endif
}

// static
bool PowerService::is_fast_path_wake(WakeCause cause, const RtcAppState &state) {
  return cause == WakeCause::Timer && state.lock_state == LockState::Locked;
}

// static
void PowerService::release_sleep_gpio_holds(int pin_pm_power) {
#ifndef TEST_HOST
  if (pin_pm_power >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(pin_pm_power));
  }
#else
  (void)pin_pm_power;
#endif
}

// ---------------------------------------------------------------------------
// Free function — early boot path
// ---------------------------------------------------------------------------

RtcAppState load_rtc_app_state() {
  if (!s_rtc_state_valid) {
    return RtcAppState{};
  }
  RtcAppState out{};
  memcpy(&out, &s_rtc_state, sizeof(RtcAppState));
  return out;
}
