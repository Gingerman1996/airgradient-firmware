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
    AG_LOGW(TAG, "poll_bms: read_telemetry() failed");
  }

  float pct = -1.0f;
  if (_bms.get_battery_percentage(&pct)) {
    status.battery_percentage = pct;
    status.critical = (pct >= 0.0f && pct < BATTERY_CRITICAL_PERCENT);
  } else {
    AG_LOGW(TAG, "poll_bms: get_battery_percentage() failed");
  }

  BmsStatus bms_status{};
  if (_bms.read_status(bms_status)) {
    status.charging_status = bms_status.charging_state;
    status.charger_status = bms_status;
    if (!sync_pmid_mode(bms_status.power_source)) {
      AG_LOGW(TAG, "poll_bms: failed to sync PMID mode for source %s",
              bms_power_source_str(bms_status.power_source));
    }
  }

  AG_LOGI(TAG,
          "poll_bms: perc=%.1f%% vbat=%.1fV vbus=%.1fV critical=%d | "
          "charge=%s src=%s | "
          "treg=%d vsys=%d iindpm=%d vindpm=%d safety_tmr=%d wd=%d",
          status.battery_percentage, status.battery_voltage, status.charging_voltage,
          status.critical, bms_charging_state_str(status.charger_status.charging_state),
          bms_power_source_str(status.charger_status.power_source),
          status.charger_status.thermal_regulation, status.charger_status.vsys_regulation,
          status.charger_status.input_current_regulation,
          status.charger_status.input_voltage_regulation,
          status.charger_status.safety_timer_expired, status.charger_status.watchdog_expired);

  const auto &t = status.telemetry;
  AG_LOGI(TAG, "poll_bms: ibus=%dmA ibat=%dmA vsys=%umV vpmid=%umV ts=%.1f%% tdie=%d°C",
          t.input_current_ma, t.battery_current_ma, t.system_voltage_mv, t.pmid_voltage_mv,
          t.ts_percent, t.die_temperature_c);

  return status;
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
  const BmsPmidMode desired_mode = bms_power_source_has_external_input(power_source)
                                       ? BmsPmidMode::PassThrough
                                       : BmsPmidMode::Boost;

  if (_pmid_mode == desired_mode) {
    return true;
  }

  if (!_bms.configure_pmid_mode(desired_mode)) {
    AG_LOGW(TAG, "sync_pmid_mode: configure_pmid_mode(%s) failed", bms_pmid_mode_str(desired_mode));
    return false;
  }

  AG_LOGI(TAG, "sync_pmid_mode: %s for power source %s", bms_pmid_mode_str(desired_mode),
          bms_power_source_str(power_source));
  _pmid_mode = desired_mode;
  return true;
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
