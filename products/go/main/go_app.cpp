/**
 * AirGradient Go — GoApp Implementation
 *
 * Contains all boot-path logic moved from main.cpp.  Hardware access is
 * routed through the GoBoard interface for testability.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_app.h"

#include "ag_log.h"
#ifndef TEST_HOST
#include "board_config.h"
#include <esp_system.h>
#else
// Minimal pin/bus constants for host test compilation.
inline constexpr int PIN_BUTTON_POWER = 5;
inline constexpr int PIN_BUTTON_BOOT = 28;
inline constexpr int PIN_CAP_INT = 1;
inline constexpr int PIN_BUZZER = 8;
inline constexpr uint32_t BUZZER_FREQ_HZ = 2700;
inline constexpr int GPS_BAUD = 115200;
// Stub esp_reset_reason for host builds.
enum esp_reset_reason_t { ESP_RST_UNKNOWN = 0 };
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_UNKNOWN; }
#endif
#include "go_ble.h"
#include "go_buzzer.h"
#include "go_events.h"
#include "go_input.h"
#include "go_led.h"
#include "go_orchestrator.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_ui.h"
#include "go_ulp.h"
#include "gps/gps_service.h"
#include "rtos.h"
#include "services/sensor_manager.h"

#include <ctime>

static constexpr const char *TAG = "app";

// ===========================================================================
// Construction
// ===========================================================================

GoApp::GoApp(GoBoard &board) : _board(board) {}

// ===========================================================================
// GoApp::run() — boot path selection
// ===========================================================================

void GoApp::run() {
  RTOS::delay_ms(100);
  WakeCause cause = PowerService::get_wake_cause();
  RtcAppState state = load_rtc_app_state();

  AG_LOGI(TAG,
          "reset_reason=%d wake_cause=%d rtc_state: mode=%d behavior=%d lock=%d gps=%d "
          "tracking=%d session=%u warm=%d",
          static_cast<int>(esp_reset_reason()), static_cast<int>(cause),
          static_cast<int>(state.mode), static_cast<int>(state.behavior),
          static_cast<int>(state.lock_state), state.gps_enabled, state.tracking_active,
          state.tracking_session_id, state.sensors_warm);

  BootPath path = select_boot_path(cause, state);

  switch (path) {
  case BootPath::FastPath:
    run_fast_path(state);
    break; // never reached
  case BootPath::ButtonWake:
    run_button_wake_path(state);
    break; // never reached
  case BootPath::Interactive:
    AG_LOGI(TAG, "Serial number: %s", _board.serial_number().c_str());
    run_interactive(cause, {});
    break; // never reached
  }
}

// ===========================================================================
// Fast-path boot (timer wake, locked)
// ===========================================================================

void GoApp::run_fast_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_fast_path: entering fast-path boot (sensors_warm=%d)", state.sensors_warm);

  // ISR for button detection during blocking operations.
  volatile bool button_pressed = false;
  _board.install_button_isr(PIN_BUTTON_POWER, &button_pressed);

  _board.ulp_stop();

  // Load snapshot here so it stays on this (non-returning) stack.
  // execute_fast_path may reference it via pointer in the handoff.
  RtcDisplaySnapshot snapshot{};
  bool snapshot_valid = load_rtc_display_snapshot(&snapshot);

  auto result = execute_fast_path(state, button_pressed, &snapshot, snapshot_valid);

  _board.remove_button_isr(PIN_BUTTON_POWER);

  if (result.outcome == FastPathResult::Outcome::Sleep) {
    RtcAppState save = state;
    save.sensors_warm = result.sensors_warm;
    _board.power().save_state(save);

    _board.display().stop();
    _board.display().deep_sleep();
    _board.ulp_start();
    _board.power().enter_sleep(result.sleep_duration_ms);
    // Never returns — CPU reboots on wake.
  }

  // Promotion to interactive — wire fast_path_measures pointer into
  // result struct (lives on this non-returning stack).
  if (result.has_measures) {
    result.handoff.fast_path_measures = &result.measures;
  }

  AG_LOGI(TAG, "fast-path promoting to interactive (button=%d)", static_cast<int>(button_pressed));
  run_interactive(WakeCause::Timer, result.handoff);
}

// ===========================================================================
// GoApp::execute_fast_path() — testable core
// ===========================================================================

GoApp::FastPathResult GoApp::execute_fast_path(const RtcAppState &state,
                                               const volatile bool &button_pressed,
                                               const RtcDisplaySnapshot *snapshot,
                                               bool snapshot_valid) {
  const uint32_t boot_time_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // --- Core init (NVS must be ready before load_settings) ---
  _board.init_core();
  _board.release_gpio_holds();

  GoSettings settings = _board.load_settings();

  SensorManager &sm = _board.sensors(state.sensors_warm);

  bool promote = false;

  // --- Warmup (interruptible) ---
  if (state.sensors_warm) {
    AG_LOGI(TAG, "fast-path: sensors warm — skipping warmup");
    RTOS::delay_ms(200);
  } else {
    const int warmup_iters = CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
    AG_LOGI(TAG, "fast-path: warmup %d iterations (%d ms interval)", warmup_iters,
            CONFIG_SENSOR_WARMUP_INTERVAL_MS);
    for (int i = 0; i < warmup_iters && !promote; i++) {
      AG_LOGI(TAG, "fast-path: warmup iteration %d/%d", i + 1, warmup_iters);
      uint64_t start = RTOS::get_time_ms();
      sm.warmup_step();

      if (button_pressed) {
        promote = true;
        break;
      }

      uint64_t elapsed = RTOS::get_time_ms() - start;
      if (elapsed < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
        RTOS::delay_ms(static_cast<uint32_t>(CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed));
      }
      if (button_pressed) {
        promote = true;
        break;
      }
    }
  }

  // --- One-shot measurement ---
  MeasuresAGo ago{};
  bool has_measures = false;
  if (!promote) {
    Measures measures = sm.start_measures(1, SensorGroup::All);
    // UX placeholder: fast path never configures the gas-index algorithm
    // (no SensorProducer), so index fields stay at invalid sentinels.
    // Overwrite with raw ticks so the display and stored route points
    // still show a value during Offline fast-path cycles.
    measures.tvoc_nox.tvoc_index = measures.tvoc_nox.tvoc_raw;
    measures.tvoc_nox.nox_index = measures.tvoc_nox.nox_raw;
    ago = measures_to_ago(measures);
    has_measures = true;
    AG_LOGI(TAG,
            "fast-path: temp=%.1f hum=%.1f pm25=%.1f co2=%d tvoc=%d nox=%d "
            "tvoc_raw=%d nox_raw=%d pres=%.1f",
            ago.temp_hum_a.temperature, ago.temp_hum_a.humidity, ago.pm_a.pm_25, ago.co2.co2,
            ago.tvoc_nox.tvoc_index, ago.tvoc_nox.nox_index, ago.tvoc_nox.tvoc_raw,
            ago.tvoc_nox.nox_raw, ago.pressure.pressure);
    if (button_pressed) {
      promote = true;
    }
  }

  // --- One-shot GPS ---
  GpsData gps{};
  const bool gps_active = is_gps_active_at_boot(settings, state);

  if (!promote && state.tracking_active && gps_active) {
    auto *gps_driver = _board.new_gps_driver();
    gps = gps_read_once(*gps_driver, GPS_BAUD, 2000, button_pressed);
    AG_LOGI(TAG, "fast-path: gps fix_type=%d sat=%d lat=%.6f lon=%.6f alt=%.1f hdop=%.1f",
            static_cast<int>(gps.fix.fix_type), gps.fix.satellite_count, gps.position.latitude,
            gps.position.longitude, gps.altitude_m, gps.fix.hdop);
    if (button_pressed) {
      promote = true;
    }
  }

  // --- Storage + cache ---
  if (!promote) {
    StorageService &stor = _board.storage();
    stor.cache_measurement(ago);

    if (state.tracking_active) {
      float battery_pct = -1.0f;
      _board.bms().get_battery_percentage(&battery_pct);
      stor.start_route(state.tracking_session_id);
      RoutePoint point{};
      point.timestamp = time(nullptr);
      point.gps = gps;
      point.sensors = ago;
      point.battery_percentage = battery_pct;
      stor.append_route_point(point);
      stor.end_route();
    }
    _board.storage().backup_cache();
  }

  // --- Display + sleep decision ---
  if (!promote) {
    PowerService &pwr = _board.power();
    PowerSnapshot bms_snap = pwr.poll_bms();

    DisplayService &disp = _board.display();
    DisplayValues values =
        build_fast_path_display(ago, gps, bms_snap, settings, state.tracking_active);
    disp.init(values);

    uint32_t awake_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - boot_time_ms;
    auto decision = pwr.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, awake_ms);

    if (decision.type == PowerService::SleepType::Deep) {
      return {
          .outcome = FastPathResult::Outcome::Sleep,
          .handoff = {},
          .measures = ago,
          .has_measures = has_measures,
          .sleep_duration_ms = decision.duration_ms,
          .sensors_warm = pwr.should_hold_pm_sensor(decision.duration_ms),
      };
    }
    // Sleep too short — fall through to promotion.
    promote = true;
  }

  // --- Build promotion handoff ---
  //
  // NOTE: handoff.fast_path_measures is NOT set here.  The MeasuresAGo
  // value is returned in FastPathResult::measures.  The caller
  // (run_fast_path) sets fast_path_measures to point into the result
  // struct, which lives on the caller's non-returning stack.
  const bool button_caused = button_pressed;

  BootHandoff handoff{};
  handoff.measurement_completed = has_measures;
  // fast_path_measures left null — caller wires the pointer (see note above)

  if (button_caused) {
    handoff.initial_lock_state = LockState::Unlocked;
    handoff.suppress_wake_press = true;
    handoff.display_snapshot = snapshot_valid ? snapshot : nullptr;
    handoff.display_painted = false;
  } else {
    // Sleep too short — stay locked.  display.init() was called in
    // the sleep phase, so the display shows a correct locked frame.
    handoff.initial_lock_state = LockState::Locked;
    handoff.display_painted = true;
  }

  return {
      .outcome = FastPathResult::Outcome::Promote,
      .handoff = handoff,
      .measures = ago,
      .has_measures = has_measures,
      .sleep_duration_ms = 0,
      .sensors_warm = false,
  };
}

// ===========================================================================
// Button-wake boot path
// ===========================================================================

void GoApp::run_button_wake_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_button_wake_path: entering button-wake boot");

  // -----------------------------------------------------------------------
  // Phase 1: Early paint (~10 ms)
  // -----------------------------------------------------------------------

  _board.init_spi();
  DisplayService &disp = _board.display();

  RtcDisplaySnapshot snapshot{};
  const bool snapshot_valid = load_rtc_display_snapshot(&snapshot);

  AG_LOGI(TAG,
          "button_wake: snapshot_valid=%d co2=%d pm25=%.1f temp=%.1f hum=%.1f "
          "tvoc=%d nox=%d pres=%.1f alt=%.1f batt=%d charging=%d "
          "gps_en=%d gps_fix=%d tracking=%d ble=%d fahrenheit=%d usaqi=%d",
          snapshot_valid, snapshot.co2_ppm, snapshot.pm25_ugm3, snapshot.temperature_c,
          snapshot.humidity_pct, snapshot.tvoc_index, snapshot.nox_index, snapshot.pressure_hpa,
          snapshot.altitude_m, snapshot.battery_pct, snapshot.is_battery_charging,
          snapshot.gps_enabled, snapshot.gps_fix, snapshot.tracking_active, snapshot.ble_enabled,
          snapshot.use_fahrenheit, snapshot.pm_use_usaqi);

  DisplayValues wake_values = build_wake_values(snapshot, snapshot_valid);

  // Returns in ~10 ms.  Worker task handles the SPI full refresh (~3 s).
  disp.init(wake_values, /* defer_refresh= */ true);

  // Stop LP Core after SPI/display init, before I2C init.
  _board.ulp_stop();

  // -----------------------------------------------------------------------
  // Phase 2: Parallel init (~300 ms)
  // Non-SPI peripherals — runs while the display refreshes in background.
  // -----------------------------------------------------------------------

  _board.init_nvs();
  _board.init_buses();
  _board.init_bms();

  GoSettings settings = _board.load_settings();

  SensorManager &sm = _board.sensors();

  // GPS driver (UART — no SPI contention)
  auto *gps_driver = _board.new_gps_driver();

  // Touch sensor (I2C)
  auto *touch = _board.new_touch_sensor();

  // Event queue
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // Construct producer services
  auto *sensor_producer = new SensorProducer(sm, event_queue,
                                             {
                                                 .task_stack_size = 4096,
                                                 .task_priority = 5,
                                             });

  auto *gps_service =
      new GpsService(*gps_driver, event_queue,
                     {
                         .baud_rate = GPS_BAUD,
                         .posting_interval_ms = settings.gps_interval_seconds * 1000,
                         .task_stack_size = 4096,
                         .task_priority = 3,
                     });

  // InputService: suppress the first ButtonPower event (the wake press)
  auto *input_service = new InputService(*touch, _board.gpio_hal(), event_queue,
                                         {
                                             .pin_cap_int = PIN_CAP_INT,
                                             .pin_button_power = PIN_BUTTON_POWER,
                                             .pin_button_boot = PIN_BUTTON_BOOT,
                                             .suppress_button_wake = true,
                                         });

  PowerService &pwr = _board.power();

  std::string serial = _board.serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial.c_str());

  auto *ui_manager = new UIManager({
      .firmware_version = _board.firmware_version(),
      .serial_number = serial.c_str(),
  });

  // Determine whether GPS should be active based on settings and RTC state.
  const bool gps_active = is_gps_active_at_boot(settings, state);

  // Start producer tasks — touch and sensors operational from here (~310 ms)
  sensor_producer->start();
  if (gps_active) {
    gps_service->start();
  } else {
    gps_service->idle_gnss();
  }
  input_service->start();

  // -----------------------------------------------------------------------
  // Phase 3: Storage init (blocks on SPI until display refresh finishes)
  // -----------------------------------------------------------------------

  StorageService &stor = _board.storage();

  // BleService construction requires StorageService
  auto *ble_service = new BleService(event_queue, stor);

  auto *buzzer = new BuzzerService({
      .pin = PIN_BUZZER,
      .default_freq_hz = BUZZER_FREQ_HZ,
      .duty_percent = 50,
  });
  buzzer->init();
  buzzer->start();

  auto *led_driver = _board.new_led_driver();
  auto *led = new LedService({.driver = led_driver});
  led->init();
  led->start();

  // -----------------------------------------------------------------------
  // Phase 4: Orchestrator — display + all services ready
  // -----------------------------------------------------------------------

  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = disp,
      .storage_service = stor,
      .power_service = pwr,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
      .buzzer = *buzzer,
      .led = *led,
  };

  auto *orchestrator =
      new Orchestrator(event_queue, services, settings, _board.config_store(), serial.c_str());

  BootHandoff handoff{};
  handoff.display_painted = true;
  handoff.suppress_wake_press = true;
  handoff.initial_lock_state = LockState::Unlocked;
  handoff.display_snapshot = snapshot_valid ? &snapshot : nullptr;
  orchestrator->init(WakeCause::Button, handoff);
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// run_interactive — unified interactive entry
// ===========================================================================

void GoApp::run_interactive(WakeCause cause, BootHandoff handoff) {
  // --- Complete any missing core init (idempotent) ---
  _board.init_core();

  GoSettings settings = _board.load_settings();

  SensorManager &sm = _board.sensors();

  // --- GPS driver (never done in fast path) ---
  auto *gps_driver = _board.new_gps_driver();

  // --- Touch sensor (never done in fast path) ---
  auto *touch = _board.new_touch_sensor();

  // --- Storage ---
  StorageService &stor = _board.storage();

  // --- Event queue ---
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // --- BLE ---
  auto *ble_service = new BleService(event_queue, stor);

  // --- Service construction ---
  auto *sensor_producer =
      new SensorProducer(sm, event_queue, {.task_stack_size = 4096, .task_priority = 5});

  auto *gps_service = new GpsService(*gps_driver, event_queue,
                                     {.baud_rate = GPS_BAUD,
                                      .posting_interval_ms = settings.gps_interval_seconds * 1000,
                                      .task_stack_size = 4096,
                                      .task_priority = 3});

  auto *input_service = new InputService(*touch, _board.gpio_hal(), event_queue,
                                         {.pin_cap_int = PIN_CAP_INT,
                                          .pin_button_power = PIN_BUTTON_POWER,
                                          .pin_button_boot = PIN_BUTTON_BOOT,
                                          .suppress_button_wake = handoff.suppress_wake_press});

  DisplayService &disp = _board.display();
  PowerService &pwr = _board.power();

  std::string serial = _board.serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial.c_str());

  auto *ui_manager = new UIManager({
      .firmware_version = _board.firmware_version(),
      .serial_number = serial.c_str(),
  });

  // --- Display init (if boot hasn't painted) ---
  if (!handoff.display_painted) {
    if (handoff.display_snapshot != nullptr) {
      DisplayValues wake = build_wake_values(*handoff.display_snapshot, true);
      disp.init(wake);
    } else {
      DisplayValues initial{};
      disp.init(initial);
    }
    handoff.display_painted = true;
  }

  // --- Determine whether GPS should be active ---
  // On fresh power-on, tracking is always inactive (no RTC state).
  // On wake from sleep, load the persisted tracking state.
  RtcAppState boot_state{};
  if (cause != WakeCause::PowerOn) {
    boot_state = load_rtc_app_state();
  }
  const bool gps_active = is_gps_active_at_boot(settings, boot_state);

  // --- Start producer tasks ---
  sensor_producer->start();
  if (gps_active) {
    gps_service->start();
  } else {
    gps_service->idle_gnss();
  }
  input_service->start();

  // --- Buzzer ---
  auto *buzzer = new BuzzerService({
      .pin = PIN_BUZZER,
      .default_freq_hz = BUZZER_FREQ_HZ,
      .duty_percent = 50,
  });
  buzzer->init();
  buzzer->start();

  // Boot melody (fresh power-on only — wake-from-sleep stays silent)
  if (cause == WakeCause::PowerOn) {
    static constexpr BuzzerService::Note kBootMelody[] = {
        {2400, 80}, {0, 30}, {2700, 80}, {0, 30}, {3200, 120},
    };
    buzzer->play(kBootMelody, sizeof(kBootMelody) / sizeof(kBootMelody[0]));
  }

  auto *led_driver = _board.new_led_driver();
  auto *led = new LedService({.driver = led_driver});
  led->init();
  led->start();

  // Boot self-test: brief white flash on all three touch LEDs.
  // Space the calls so each one finishes before the next is enqueued — the
  // LedService worker preempts in-flight holds when a new command arrives, so
  // back-to-back enqueues would only show the last LED.
  if (cause == WakeCause::PowerOn) {
    led->flash_white(LedService::Led::Select, 76, 200);
    RTOS::delay_ms(220);
    led->flash_white(LedService::Led::Left, 76, 200);
    RTOS::delay_ms(220);
    led->flash_white(LedService::Led::Right, 76, 200);
  }

  // --- Orchestrator ---
  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = disp,
      .storage_service = stor,
      .power_service = pwr,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
      .buzzer = *buzzer,
      .led = *led,
  };

  auto *orchestrator =
      new Orchestrator(event_queue, services, settings, _board.config_store(), serial.c_str());
  orchestrator->init(cause, handoff);
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// Pure utility functions
// ===========================================================================

BootPath select_boot_path(WakeCause cause, const RtcAppState &state) {
  if (cause == WakeCause::Timer) {
    if (PowerService::is_fast_path_wake(cause, state)) {
      return BootPath::FastPath;
    }
  }
  if (cause == WakeCause::Button && state.mode == OperatingMode::Offline) {
    return BootPath::ButtonWake;
  }
  return BootPath::Interactive;
}

bool is_gps_active_at_boot(const GoSettings &settings, const RtcAppState &state) {
  return (settings.gps_mode == GpsMode::AlwaysOn) ||
         (settings.gps_mode == GpsMode::OnWhenTracking && state.tracking_active);
}

MeasuresAGo measures_to_ago(const Measures &m) {
  MeasuresAGo ago{};
  ago.temp_hum_a = m.temp_hum_a;
  ago.pm_a = m.pm_a;
  ago.co2 = m.co2;
  ago.tvoc_nox = m.tvoc_nox;
  ago.power = m.power;
  ago.pressure = m.pressure;
  return ago;
}

DisplayValues build_fast_path_display(const MeasuresAGo &measures, const GpsData &gps,
                                      const PowerSnapshot &bms, const GoSettings &settings,
                                      bool tracking_active) {
  DisplayValues v{};

  if (measures.co2.is_valid()) {
    v.co2_ppm = measures.co2.co2;
  }
  if (measures.pm_a.is_pm_25_valid()) {
    v.pm25_ugm3 = measures.pm_a.pm_25;
  }
  if (measures.temp_hum_a.is_temp_valid()) {
    v.temperature_c = measures.temp_hum_a.temperature;
  }
  if (measures.temp_hum_a.is_hum_valid()) {
    v.humidity_pct = measures.temp_hum_a.humidity;
  }
  if (measures.tvoc_nox.is_tvoc_index_valid()) {
    v.tvoc_index = measures.tvoc_nox.tvoc_index;
  }
  if (measures.tvoc_nox.is_nox_index_valid()) {
    v.nox_index = measures.tvoc_nox.nox_index;
  }
  if (measures.pressure.is_pressure_valid()) {
    v.pressure_hpa = measures.pressure.pressure;
  }
  if (measures.pressure.is_altitude_valid()) {
    v.altitude_m = measures.pressure.altitude;
  }

  v.gps_fix = is_fix_valid(gps.fix);

  if (bms.battery_percentage >= 0.0f) {
    v.battery_pct = static_cast<uint8_t>(bms.battery_percentage);
  }
  v.is_battery_charging = is_bms_charging(bms.charging_status);

  v.locked = true;
  v.screen = Screen::Home;
  v.tracking_active = tracking_active;

  v.use_fahrenheit = settings.use_fahrenheit;
  v.pm_use_usaqi = settings.pm_use_usaqi;
  v.display_off = false;

  return v;
}

DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot, bool snapshot_valid) {
  DisplayValues v{};

  if (snapshot_valid) {
    v.co2_ppm = snapshot.co2_ppm;
    v.pm25_ugm3 = snapshot.pm25_ugm3;
    v.temperature_c = snapshot.temperature_c;
    v.humidity_pct = snapshot.humidity_pct;
    v.tvoc_index = snapshot.tvoc_index;
    v.nox_index = snapshot.nox_index;
    v.pressure_hpa = snapshot.pressure_hpa;
    v.altitude_m = snapshot.altitude_m;
    v.battery_pct = snapshot.battery_pct;
    v.is_battery_charging = snapshot.is_battery_charging;
    v.gps_enabled = snapshot.gps_enabled;
    v.gps_fix = snapshot.gps_fix;
    v.tracking_active = snapshot.tracking_active;
    v.ble_enabled = snapshot.ble_enabled;
    v.use_fahrenheit = snapshot.use_fahrenheit;
    v.pm_use_usaqi = snapshot.pm_use_usaqi;
  }

  v.screen = Screen::Home;
  v.locked = false;
  v.display_off = false;
  v.snackbar_text = "Unlocked";

  return v;
}
