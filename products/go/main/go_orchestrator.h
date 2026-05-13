/**
 * AirGradient Go — Orchestrator
 *
 * Central event loop that consumes typed events from producers, updates
 * state machines (app state, UI state, power policy), and calls consumers
 * directly.  See ARCHITECTURE.md §3 for the high-level design.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "config_store.h"
#include "go_ble.h"
#include "go_buzzer.h"
#include "go_led.h"
#include "go_ulp.h"
#include "go_display.h"
#include "go_events.h"
#include "gps/gps_service.h"
#include "go_input.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"
#include "rtos.h"

#include <cstdint>

class Orchestrator {
public:
  /// References to all product services.  All referenced objects must outlive
  /// the Orchestrator instance.
  struct Services {
    SensorProducer &sensor_producer;
    GpsService &gps_service;
    InputService &input_service;
    DisplayService &display_service;
    StorageService &storage_service;
    PowerService &power_service;
    UIManager &ui_manager;
    BleService &ble_service;
    BuzzerService &buzzer;
    LedService &led;
  };

  /// Construct the orchestrator.
  ///
  /// @param event_queue  RTOS queue handle for the central event queue.
  /// @param services     References to all product services.
  /// @param settings     Product settings (owned copy — orchestrator may
  ///                     update on SettingsChanged events).
  /// @param config_store Config store for persisting setting changes.
  /// @param serial       Device serial string (e.g., "AABBCCDDEEFF"),
  ///                     used for BLE advertising name.  Must remain
  ///                     valid for the lifetime of the Orchestrator.
  Orchestrator(RtosQueueHandle event_queue, const Services &services, GoSettings settings,
               ConfigStore &config_store, const char *serial);

  /// Set initial state from boot context and perform first-boot actions.
  /// Call once before run().
  ///
  /// @param cause   Wake cause from PowerService::get_wake_cause().
  /// @param handoff Boot-to-runtime handoff describing what boot has
  ///                already done. Default is a fresh power-on.
  void init(WakeCause cause, const BootHandoff &handoff = {});

  /// Main event loop.  Does not return.
  void run();

private:
#ifdef TEST_HOST
  friend class OrchestratorTestAccess;
#endif

  RtosQueueHandle _event_queue;
  Services _svc;
  GoSettings _settings;
  ConfigStore &_config_store;
  const char *_serial; ///< Device serial string for BLE advertising

  // --- Application state ---
  OperatingMode _mode = OperatingMode::Portable;
  Behavior _behavior = Behavior::Idle;
  LockState _lock_state = LockState::Locked;
  bool _gps_enabled = true;
  bool _tracking_active = false;
  uint32_t _tracking_session_id = 0;

  // --- Cached data ---
  MeasuresAGo _cached_measures{}; ///< Merged sensor results (invalid sentinels by default)
  GpsData _latest_gps{};
  PowerSnapshot _latest_power{};

  // --- Timer tracking (millisecond timestamps) ---
  uint32_t _last_measurement_ms = 0;
  uint32_t _last_bms_poll_ms = 0;
  uint32_t _last_bms_status_poll_ms = 0;
  uint32_t _last_ext_wdt_ms = 0;
  uint32_t _last_input_ms = 0;                ///< Reset on every input; drives inactivity
  uint32_t _snackbar_refresh_deadline_ms = 0; ///< 0 = inactive; non-zero = absolute deadline
  /// 0 = inactive; non-zero = absolute deadline after which the post-melody
  /// LED restore (apply_pm25_indicator) should run.  Set by the PlaySound
  /// UI handler, checked in check_timers().
  uint32_t _melody_pm25_restore_deadline_ms = 0;
  bool _first_measurement_done = false;

  // --- PM sensor sleep (Portable mode power-cycling) ---
  bool _pm_prepare_sent = false; ///< PREPARE already sent for the current measurement cycle

  // --- Charge-done alert tracking ---
  /// Uptime when the BMS transitioned to NotCharging while still plugged in.
  /// 0 = not currently in the charge-done waiting state.
  uint32_t _charge_done_start_ms = 0;
  /// True after the rest-timeout alert (LED8 blink + beep) has fired.
  /// Prevents repeating the alert every poll cycle.
  bool _charge_done_alerted = false;

  // --- Admin-mode entry gesture ---
  // The tap-count constant is declared here (not in the "Constants" block
  // below) so it precedes the array dimension that uses it.
  static constexpr uint8_t ADMIN_TAP_COUNT = 5;
  enum class AdminEntryState : uint8_t {
    None,                  ///< No gesture in progress.
    ArmedExpectingLeft,    ///< 5 taps detected; LED8 blinks; waiting for Left.
    ArmedExpectingRight,   ///< Left received; waiting for Right.
  };
  AdminEntryState _admin_entry_state = AdminEntryState::None;
  uint32_t _admin_entry_deadline_ms = 0; ///< 0 = no active deadline.
  /// Ring buffer of recent Select-tap timestamps (millisecond uptime).
  uint32_t _admin_tap_times[ADMIN_TAP_COUNT] = {0};
  uint8_t _admin_tap_idx = 0;

  // --- Display buffers (mutable for const build_context) ---
  mutable Measures _display_measures{};
  mutable MeasuresAGo _cache_buf[UI_CHART_BUF_SIZE]{};

  // --- Constants ---
  static constexpr uint32_t BMS_POLL_INTERVAL_MS = 10000;
  // 1 Hz so PMID mode (Boost/PassThrough) re-syncs within ~1 s of a USB plug
  // event. EN_OTG is dropped on PassThrough entry, so a slower cadence leaves
  // the PM sensor's +5V rail dead for the polling window after USB removal.
  static constexpr uint32_t BMS_STATUS_POLL_INTERVAL_MS = 1000;
  static constexpr uint32_t EXT_WDT_INTERVAL_MS = 60000;
  static constexpr uint32_t MAX_REASONABLE_TIMEOUT_MS = 3600000;
  static constexpr uint32_t BLE_COMMAND_RESULT_DELAY_MS = 200;
  static constexpr uint32_t SHUTDOWN_DISPLAY_DELAY_MS = 500;

  // After the BMS reports charge complete and the user-facing "charge done"
  // melody fires, we wait this long before triggering the unplug-me alert
  // (LED8 blink + beep).  Matches the BQ27427 ResRelax Time default so the
  // fuel gauge has its OCV-at-rest reading captured before the user
  // disconnects.
  static constexpr uint32_t CHARGE_REST_TIMEOUT_MS = 500000;

  // Admin-mode entry gesture: ADMIN_TAP_COUNT rapid Select taps within
  // ADMIN_TAP_WINDOW_MS arms the sequence; then each subsequent step
  // (Left, Right) must complete within ADMIN_STEP_TIMEOUT_MS.  On success
  // LED8 flashes green for ADMIN_SUCCESS_FLASH_MS.  (ADMIN_TAP_COUNT lives
  // up top with the private state because it's used as an array dimension.)
  static constexpr uint32_t ADMIN_TAP_WINDOW_MS = 3000;
  static constexpr uint32_t ADMIN_STEP_TIMEOUT_MS = 5000;
  static constexpr uint32_t ADMIN_SUCCESS_FLASH_MS = 1000;

  // --- Event dispatch ---
  void dispatch(const Event &event);

  // --- Event handlers ---
  void on_sensor_data(const MeasuresAGo &data);
  void on_gps_fix(const GpsData &data);
  void on_input(const InputEventData &input);
  void on_co2_calibration_done(Co2CalibrationResult result);

  // --- BLE event handlers ---
  void on_ble_connected();
  void on_ble_disconnected();
  void on_ble_config_write();
  void on_ble_history_write();
  void on_ble_pairing_request(uint32_t passkey);
  void on_ble_auth_complete();

  // --- State transitions ---
  void lock();
  void unlock();
  void start_tracking();
  void stop_tracking();
  void change_mode(OperatingMode new_mode);
  void apply_settings_change();
  void apply_led_brightness();
  void apply_pm25_indicator();

  // --- Admin-mode entry gesture helpers ---
  /// Record one Select tap and detect the rapid-tap "arm" pattern.
  /// Returns true if this tap completed the pattern (admin entry should
  /// arm); false otherwise.
  bool record_select_tap_check_pattern(uint32_t now_ms);

  /// Try to consume `input` as part of the admin-entry sequence.
  /// Returns true if the input was consumed (caller must not forward it
  /// to the UI manager).  When the sequence is not active or this input
  /// is irrelevant, returns false.
  bool consume_admin_entry_input(const InputEventData &input);

  /// Enter the armed state: LED8 blinks, deadline timer starts, log line.
  void arm_admin_entry();
  /// Successful sequence — persist admin_mode=true, flash LED8 green.
  void complete_admin_entry();
  /// Abandon any pending sequence (timeout, wrong key, or external).
  void abort_admin_entry(const char *reason);
  bool clear_data();
  bool factory_reset();
  void save_tag(uint8_t tag_index, const char *tag_label);
  void shutdown();

  // --- Timer management ---
  uint32_t compute_queue_timeout_ms() const;
  void check_timers();
  void on_bms_timer();
  void on_bms_status_timer();
  void on_inactivity_timeout();
  void reschedule_sensor_timer(const GoSettings &previous_settings);

  // --- Display ---
  void update_display();
  void request_background_display_update();
  BuildContext build_context() const;

  // --- Sleep ---
  void try_enter_sleep();
  void prepare_for_sleep(uint32_t sleep_duration_ms);

  // --- BLE ---
  void init_ble_if_portable();
  static constexpr size_t BLE_WRITE_BUF_SIZE = 256;

  // --- Helpers ---
  bool is_gps_active() const;
  void deactivate_gps();
  uint32_t generate_session_id();
  RtcAppState snapshot_state() const;
};
