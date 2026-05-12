/**
 * AirGradient Go — Power Management Service
 *
 * Handles BMS status polling, battery monitoring, sleep cycle management,
 * RTC state persistence, and shutdown.  Called synchronously by the
 * orchestrator — no independent task.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "airgradient_gpio.h"
#include "go_settings.h"
#include "go_types.h"
#include "hal/bms_device.h"

#include <cstdint>

// ---------------------------------------------------------------------------
// PowerSnapshot
// ---------------------------------------------------------------------------

/// Aggregated BMS snapshot for the orchestrator and display.
/// All fields are initialized to invalid sentinels; call poll_bms() to fill.
struct PowerSnapshot {
  float battery_voltage = BmsInvalid::VOLT;
  float charging_voltage = BmsInvalid::VOLT;
  float battery_percentage = -1.0f;
  BmsChargingState charging_status = BmsChargingState::Unknown;
  bool critical = false; ///< true when battery_percentage < BATTERY_CRITICAL_PERCENT

  /// Full charger status (power source, regulation flags, fault flags).
  BmsStatus charger_status{};

  /// Full ADC telemetry (currents, voltages, temperatures).
  BmsTelemetry telemetry{};
};

// ---------------------------------------------------------------------------
// Charging state helper
// ---------------------------------------------------------------------------

/// Return true when the BMS charging state indicates active charging.
/// Excludes NotCharging, Unknown (read error / uninitialised), and
/// ChargeTerminationDone (battery full, no longer drawing current).
inline bool is_bms_charging(BmsChargingState state) {
  return state != BmsChargingState::NotCharging && state != BmsChargingState::Unknown &&
         state != BmsChargingState::ChargeTerminationDone;
}

// ---------------------------------------------------------------------------
// PowerService
// ---------------------------------------------------------------------------

class BQ27427;

class PowerService {
public:
  // -------------------------------------------------------------------------
  // Configuration
  // -------------------------------------------------------------------------

  struct Config {
    int pin_wake_button_power;                 ///< GPIO for deep sleep wake (Button Power)
    int pin_wake_button_boot;                  ///< GPIO for deep sleep wake (Button Boot)
    int pin_ext_wdt = -1;                      ///< External watchdog GPIO (-1 = disabled)
    int deep_sleep_threshold_ms = 5000;        ///< Minimum interval (ms) to prefer deep sleep
    int pin_pm_power = -1;                     ///< PM sensor power GPIO (-1 = no hold)
    uint32_t sensor_hold_max_sleep_ms = 20000; ///< Max sleep (ms) to hold PM sensor powered
    uint32_t pm_sleep_threshold_ms = 20000;    ///< Min measure interval (ms) to power-cycle PM
  };

  // -------------------------------------------------------------------------
  // Sleep type
  // -------------------------------------------------------------------------

  /// Sleep type selected by decide_sleep().
  enum class SleepType {
    None, ///< Do not sleep (device is unlocked, not Offline, or interval too short)
    Deep, ///< Deep sleep — CPU reboots on wake; does not return from enter_sleep()
  };

  /// Combined result of sleep decision: what type and for how long.
  struct SleepDecision {
    SleepType type;       ///< None or Deep
    uint32_t duration_ms; ///< How long to sleep (0 when type == None)
  };

  // -------------------------------------------------------------------------
  // Construction
  // -------------------------------------------------------------------------

  /// @param bms     BMS device (via BmsDevice HAL).  Must outlive this service.
  /// @param gpio    GPIO HAL function-pointer table.
  /// @param config  Runtime configuration (wake pins, sleep threshold).
  PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config);

  /// Attach an optional BQ27427 fuel gauge.  When set, poll_bms() will
  /// prefer the gauge's Impedance Track SOC over the BMS voltage-based
  /// estimate.  Pass nullptr to detach.  Not owned.
  void set_fuel_gauge(BQ27427 *fg) { _fuel_gauge = fg; }

  // -------------------------------------------------------------------------
  // BMS operations (called by orchestrator on timer)
  // -------------------------------------------------------------------------

  /// Poll BMS for current status.  Fast I2C read, non-blocking.
  /// Returns a PowerSnapshot with all fields populated (invalid sentinels on error).
  PowerSnapshot poll_bms();

  /// Lightweight charging-status-only poll
  /// Use on a fast timer to detect plug/unplug quickly without the cost
  /// of a full ADC + battery-percentage poll.
  /// @param[out] state  Populated on success.
  /// @return true if the read succeeded.
  bool poll_charging_status(BmsChargingState &state);

  /// Lightweight charger status poll.
  ///
  /// Used by the fast runtime timer to detect plug/unplug changes and keep the
  /// PMID rail configured correctly without waiting for the full telemetry poll.
  /// @param[out] status Populated on success.
  /// @return true if the read succeeded.
  bool poll_status(BmsStatus &status);

  /// Reset BMS watchdog.  Must be called periodically (< 10 s interval).
  /// @return true if the watchdog reset succeeded.
  bool reset_watchdog();

  /// Trigger BMS QoN (ship mode).  Device powers off.  Does not return.
  void shutdown();

  // -------------------------------------------------------------------------
  // External watchdog
  // -------------------------------------------------------------------------

  /// Configure the external watchdog GPIO as output (LOW).
  /// No-op if pin_ext_wdt < 0 in Config.
  void init_ext_watchdog();

  /// Pulse the external watchdog GPIO (HIGH 20 ms, then LOW).
  /// No-op if pin_ext_wdt < 0 in Config.
  void reset_ext_watchdog();

  // -------------------------------------------------------------------------
  // RTC state persistence
  // -------------------------------------------------------------------------

  /// Save application state to RTC memory before sleep.
  /// Under TEST_HOST the state is stored in a regular static variable.
  void save_state(const RtcAppState &state);

  /// Load application state from RTC memory after wake.
  /// Returns default-constructed RtcAppState when no valid state has been saved.
  RtcAppState load_state() const;

  // -------------------------------------------------------------------------
  // Sleep cycle
  // -------------------------------------------------------------------------

  /// Determine whether to sleep, which type, and for how long.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  /// Uses Config::deep_sleep_threshold_ms from construction.
  ///
  /// Rules:
  ///   - Not Offline mode  -> {None, 0}  (only Offline sleeps)
  ///   - Unlocked          -> {None, 0}  (never sleep while user is active)
  ///   - sleep_ms >= deep_sleep_threshold_ms -> {Deep, sleep_ms}
  ///   - sleep_ms <  deep_sleep_threshold_ms -> {None, 0}  (stay awake; avoid
  ///     deep sleep overhead exceeding the benefit for short intervals)
  ///
  /// sleep_ms = min(enabled intervals) - awake_ms, clamped to 0.
  ///
  /// @param settings   Current settings (sensor and display intervals).
  /// @param lock_state Current lock state.
  /// @param mode       Current operating mode.
  /// @param awake_ms   Milliseconds the device has been awake this cycle.
  SleepDecision decide_sleep(const GoSettings &settings, LockState lock_state, OperatingMode mode,
                             uint32_t awake_ms) const;

  /// Return true when the given sleep duration is short enough that keeping
  /// the PM sensor powered (GPIO held) across deep sleep is beneficial.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  bool should_hold_pm_sensor(uint32_t sleep_duration_ms) const;

  /// Return true when the measurement interval is long enough to justify
  /// power-cycling the PM sensor between measurements (Portable mode).
  ///
  /// The threshold is `Config::pm_sleep_threshold_ms` (default 20 s), which
  /// accounts for ~10 s warmup plus a minimum off-time to make the power
  /// cycle worthwhile.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  bool should_sleep_pm_sensor(uint32_t measure_interval_ms) const;

  /// Control PM sensor power GPIO.  EN_PM is active-low on v0.3 hardware:
  /// drives the pin LOW (on=true) or HIGH (on=false).  No-op when
  /// `Config::pin_pm_power < 0`.
  void set_pm_power(bool on);

  /// Enter deep sleep.  Does not return — CPU reboots on wake.
  ///
  /// Configures timer and GPIO wake sources, then calls
  /// esp_deep_sleep_start().  Only call when decide_sleep() returns Deep.
  ///
  /// When `should_hold_pm_sensor(sleep_duration_ms)` is true, the PM power
  /// GPIO is latched at its current level during deep sleep via
  /// `gpio_hold_en()` (active-low LOW = on for v0.3).  On ESP32-C5
  /// per-pin hold automatically persists through deep sleep.  The caller
  /// must set `RtcAppState::sensors_warm`
  /// accordingly before calling `save_state()`.
  ///
  /// @param sleep_duration_ms How long to sleep before timer wake.
  void enter_sleep(uint32_t sleep_duration_ms);

  // -------------------------------------------------------------------------
  // Boot path (static — call before any service is constructed)
  // -------------------------------------------------------------------------

  /// Determine wake cause early in app_main.
  /// Translates esp_sleep_get_wakeup_cause() to WakeCause.
  static WakeCause get_wake_cause();

  /// Returns true when this boot should follow the abbreviated fast path:
  ///   cause == WakeCause::Timer && state.lock_state == LockState::Locked
  ///
  /// Pure logic — no platform dependencies; testable on host.
  static bool is_fast_path_wake(WakeCause cause, const RtcAppState &state);

  /// Release GPIO holds that were enabled before the previous deep sleep.
  ///
  /// Must be called **after** `init_gpio()` has reconfigured the held pin
  /// as output HIGH.  While hold is active the pad stays latched; the GPIO
  /// driver writes the new output config to registers underneath.
  /// Releasing hold then lets the fresh output driver take over with no
  /// power glitch.  No-op when pin_pm_power < 0.
  /// Guarded by `#ifndef TEST_HOST`.
  static void release_sleep_gpio_holds(int pin_pm_power);

  // -------------------------------------------------------------------------
  // Constants
  // -------------------------------------------------------------------------

  /// Battery percentage below which the critical flag is set in PowerSnapshot.
  /// Fixed threshold — not a user-configurable setting.
  static constexpr float BATTERY_CRITICAL_PERCENT = 5.0f;

private:
  BmsDevice &_bms;
  const gpio::Hal &_gpio;
  Config _config;
  BQ27427 *_fuel_gauge = nullptr; ///< Optional fuel gauge — not owned.
  BmsPmidMode _pmid_mode = BmsPmidMode::Unknown;

  /// Cached state of the BMS charge-enable bit, so we only issue an I²C
  /// write when the desired state actually changes (edge-trigger).
  /// Default to "true" (enabled) since BQ25629::init() leaves charging on.
  bool _charge_enabled = true;

  /// Configure timer and GPIO wake sources before entering sleep.
  /// Wrapped in #ifndef TEST_HOST — not callable from host test builds.
  void configure_wake_sources(uint32_t timer_ms);

  /// Reconcile the PMID mode with the current charger power source.
  bool sync_pmid_mode(BmsPowerSource power_source);
};

// ---------------------------------------------------------------------------
// Free function — early boot path
// ---------------------------------------------------------------------------

/// Read RtcAppState from RTC memory.  Returns default state if no valid
/// state has been saved.  No dependencies — safe to call early in app_main
/// before PowerService is constructed.
RtcAppState load_rtc_app_state();
