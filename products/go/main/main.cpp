/**
 * AirGradient Go — Application Entry Point
 *
 * Boot path selection, hardware initialization, and fast-path boot for
 * the AirGradient Go portable air quality monitor.
 *
 * Three boot paths:
 *   1. Timer wake (fast path) -> measure, display, sleep or promote to interactive
 *   2. Button wake (Offline)  -> four-phase boot with early paint
 *   3. Fresh power-on / other -> run_interactive() with empty BootContext
 *
 * BootContext tracks hardware resources initialized during boot. Passed to
 * run_interactive() on fast-path promotion to avoid double-initialization.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <string>

#include "ag_log.h"
#include "airgradient_uart.h"
#include "backends/rtc_payload_cache_storage.h"
#include "cap1203.h"
#include "common.h"
#include "drivers/bq25629/bq25629_bms.h"
#include "drivers/dps368/dps368.h"
#include "drivers/s12/s12.h"
#include "drivers/scd4x/scd4x.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
#include "gps/gps_driver.h"
#include "native_gpio.h"
#include "nvs_config_store.h"
#include "rtos.h"
#include "services/payload_cache.h"
#include "services/sensor_manager.h"
#include "spi_nand_storage.h"

#include "board_config.h"
#include "go_ble.h"
#include "go_display.h"
#include "go_events.h"
#include "go_input.h"
#include "go_orchestrator.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"
#include "gps/gps_service.h"

static constexpr const char *TAG = "main";

static constexpr const char *FIRMWARE_VERSION = "0.1.0";

// ===========================================================================
// BootContext — tracks hardware resources initialized during boot
// ===========================================================================

/// Tracks hardware resources initialized during boot. Populated
/// incrementally by init helpers. Passed to run_interactive() on
/// fast-path promotion to avoid double-initialization.
///
/// All pointers are heap-allocated and never freed (the Orchestrator's
/// run() never returns). On fast-path sleep, enter_sleep() reboots the
/// CPU and the heap is reclaimed.
struct BootContext {
  // Buses
  i2c_master_bus_handle_t i2c_bus = nullptr;
  bool spi_ready = false;

  // Settings
  NvsConfigStore *config_store = nullptr;
  GoSettings settings{};

  // Drivers
  BQ25629Bms *bms = nullptr;
  SensorManager *sensor_manager = nullptr;

  // Storage
  PayloadCache *cache = nullptr;
  StorageService *storage = nullptr;

  // Display
  DisplayService *display = nullptr;

  // Power
  PowerService *power_service = nullptr;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void run_fast_path(const RtcAppState &state);
static void run_button_wake_path(const RtcAppState &state);
static void run_interactive(WakeCause cause, BootContext &ctx, BootHandoff handoff);

static void init_nvs();
static i2c_master_bus_handle_t init_i2c_bus();
static void init_gpio();
static void init_spi_buses();
static BQ25629Bms *init_bms(i2c_master_bus_handle_t i2c_bus);
static CO2Sensor *init_co2_sensor(i2c_master_bus_handle_t i2c_bus);
static MeasuresAGo measures_to_ago(const Measures &m);
static DisplayValues build_fast_path_display(const MeasuresAGo &measures, const GpsData &gps,
                                             const PowerSnapshot &bms, const GoSettings &settings,
                                             bool tracking_active);
static DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot, bool snapshot_valid);

// ===========================================================================
// Shared init helpers
//
// Each helper writes its results into BootContext &ctx. Called exactly once
// per boot — no idempotency logic needed.
// ===========================================================================

/// Initialize NVS flash, construct config store, load settings.
static void init_settings(BootContext &ctx) {
  init_nvs();
  ctx.config_store = new NvsConfigStore("go");
  ctx.settings = load_go_settings(*ctx.config_store);
  print_settings(ctx.settings);
}

/// Initialize GPIO power enables + I2C bus + settling delays.
static void init_buses(BootContext &ctx) {
  init_gpio();
  RTOS::delay_ms(100);
  ctx.i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);
}

/// Initialize SPI bus(es).
static void init_spi(BootContext &ctx) {
  init_spi_buses();
  ctx.spi_ready = true;
}

/// Initialize BMS (requires I2C).
static void init_bms_driver(BootContext &ctx) {
  ctx.bms = init_bms(ctx.i2c_bus);
  if (!ctx.bms->init()) {
    AG_LOGE(TAG, "BMS init failed");
  }
}

/// Construct and init all sensor drivers, build SensorManager.
///
/// @param sensors_warm When true the PM sensor was kept powered across deep
///        sleep.  SPS30::init() skips CMD_RESET so the fan keeps spinning
///        and existing measurement data is preserved.
static void init_sensors(BootContext &ctx, bool sensors_warm = false) {
  auto *sgp41 = new SGP41(ctx.i2c_bus, I2C_ADDR_SGP41);
  auto *sps30 = new SPS30(ctx.i2c_bus);
  auto *dps368 = new DPS368(ctx.i2c_bus, I2C_ADDR_DPS368);

  // Heap-allocated: SensorManager stores Sensors by reference, so the
  // struct must outlive this function.
  auto *sensors = new Sensors{};
  sensors->co2 = init_co2_sensor(ctx.i2c_bus);

  if (sgp41->init()) {
    sensors->tvoc_nox = sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }
  if (sps30->init(sensors_warm)) {
    sensors->pms_a = sps30;
  } else {
    AG_LOGE(TAG, "SPS30 init failed");
  }
  if (dps368->init()) {
    sensors->pressure = dps368;
  } else {
    AG_LOGE(TAG, "DPS368 init failed");
  }

  sensors->temp_hum_a_fallback.priority[0] = TempHumSource::CO2;
  sensors->temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
  sensors->temp_hum_a_fallback.count = 2;

  ctx.sensor_manager = new SensorManager(*sensors);
}

/// Initialize RTC payload cache + NAND flash + StorageService.
static void init_storage(BootContext &ctx) {
  auto *rtc_storage = new RtcPayloadCacheStorage();
  ctx.cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST;
  nand_config.cs_pin = PIN_NAND_CS;
  auto *nand = new SpiNandStorage(nand_config);

  ctx.storage = new StorageService(*ctx.cache, *nand);
  ctx.storage->restore_cache();
  if (!ctx.storage->init()) {
    AG_LOGE(TAG, "NAND storage init failed");
  }
}

/// Initialize PowerService + external watchdog.
static void init_power(BootContext &ctx) {
  ctx.power_service = new PowerService(*ctx.bms, gpio::native::hal,
                                       {
                                           .pin_wake_button_power = PIN_BUTTON_POWER,
                                           .pin_wake_button_boot = -1,
                                           .pin_ext_wdt = PIN_EXT_WDT,
                                           .deep_sleep_threshold_ms = 5000,
                                           .pin_pm_power = PIN_PM_POWER,
                                           .sensor_hold_max_sleep_ms = 20000,
                                       });
  ctx.power_service->init_ext_watchdog();
  ctx.power_service->reset_ext_watchdog();
  ctx.power_service->init_light_sleep_lock();
}

/// Initialize display service (construction only, no init/paint).
static void init_display(BootContext &ctx) {
  ctx.display = new DisplayService({
      .spi_host = SPI_HOST,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });
}

/// Common early init: settings + buses + SPI + BMS.
static void init_core(BootContext &ctx) {
  init_settings(ctx);
  init_buses(ctx);
  init_spi(ctx);
  init_bms_driver(ctx);
}

/// Same as init_core but skips SPI (already initialized for early paint).
static void init_core_no_spi(BootContext &ctx) {
  init_settings(ctx);
  init_buses(ctx);
  // SPI already initialized — ctx.spi_ready must be true.
  init_bms_driver(ctx);
}

// ===========================================================================
// app_main — boot path selection
// ===========================================================================

extern "C" void app_main() {
  RTOS::delay_ms(100);
  WakeCause cause = PowerService::get_wake_cause();

  if (cause == WakeCause::Timer) {
    RtcAppState state = load_rtc_app_state();
    if (PowerService::is_fast_path_wake(cause, state)) {
      run_fast_path(state);
      // Never returns — sleeps or promotes to interactive.
    }
  }

  if (cause == WakeCause::Button) {
    RtcAppState state = load_rtc_app_state();
    if (state.mode == OperatingMode::Offline) {
      run_button_wake_path(state);
      // Never returns.
    }
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial_number.c_str());

  BootContext ctx;
  run_interactive(cause, ctx, {});
  // Never returns.
}

// ===========================================================================
// Fast-path boot (timer wake, locked)
//
// Minimal initialization — no event loop, no producer tasks, no input
// handling.  Goal: measure, display, sleep — as fast as possible to
// conserve battery.
//
// On sleep-too-short, promotes to run_interactive() with the partially
// filled BootContext. Never returns to app_main().
// ===========================================================================

static void run_fast_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_fast_path: entering fast-path boot (sensors_warm=%d)", state.sensors_warm);
  const uint32_t boot_time_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // --- ISR setup for button detection ---
  static volatile bool s_button_pressed = false;
  s_button_pressed = false;
  gpio_install_isr_service(0); // idempotent
  gpio_set_intr_type(PIN_BUTTON_POWER, GPIO_INTR_NEGEDGE);
  gpio_isr_handler_add(
      PIN_BUTTON_POWER, [](void *arg) { *static_cast<volatile bool *>(arg) = true; },
      const_cast<bool *>(&s_button_pressed));

  // --- Core init ---
  BootContext ctx;
  init_core(ctx);

  // Release GPIO hold AFTER init_gpio() (inside init_core → init_buses) has
  // reconfigured PIN_PM_POWER as output HIGH.  While the hold is active the
  // pad stays latched HIGH; the GPIO driver writes the new output config to
  // registers underneath.  Releasing the hold then lets the fresh output
  // driver take over — no power glitch on the SPS30 supply.
  PowerService::release_sleep_gpio_holds(PIN_PM_POWER);

  init_sensors(ctx, state.sensors_warm);

  bool promote = false;

  // --- Interruptible warmup (skip when sensors were kept powered) ---
  if (state.sensors_warm) {
    AG_LOGI(TAG, "fast-path: sensors warm — skipping warmup");
  } else {
    const int warmup_iters = CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
    AG_LOGI(TAG, "fast-path: warmup %d iterations (%d ms interval)", warmup_iters,
            CONFIG_SENSOR_WARMUP_INTERVAL_MS);
    for (int i = 0; i < warmup_iters && !promote; i++) {
      AG_LOGI(TAG, "fast-path: warmup iteration %d/%d", i + 1, warmup_iters);
      uint64_t start = RTOS::get_time_ms();
      ctx.sensor_manager->warmup_step();

      if (s_button_pressed) {
        promote = true;
        break;
      }

      uint64_t elapsed = RTOS::get_time_ms() - start;
      if (elapsed < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
        RTOS::delay_ms(static_cast<uint32_t>(CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed));
      }
      if (s_button_pressed) {
        promote = true;
        break;
      }
    }
  }

  // --- One-shot measurement (skip if promoting) ---
  MeasuresAGo ago{};
  bool has_measures = false;
  if (!promote) {
    Measures measures = ctx.sensor_manager->start_measures(1, SensorGroup::All);
    // TODO: raw-to-index placeholder (remove when algorithm is applied)
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
    if (s_button_pressed) {
      promote = true;
    }
  }

  // --- One-shot GPS (skip if promoting) ---
  GpsData gps{};
  const bool gps_active =
      (ctx.settings.gps_mode == GpsMode::AlwaysOn) ||
      (ctx.settings.gps_mode == GpsMode::OnWhenTracking && state.tracking_active);

  if (!promote && state.tracking_active && gps_active) {
    AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
    GpsDriver gps_driver(gps_serial);
    gps = gps_read_once(gps_driver, GPS_BAUD, 2000, s_button_pressed);
    if (s_button_pressed) {
      promote = true;
    }
  }

  // --- Storage + cache (skip if promoting) ---
  if (!promote) {
    init_storage(ctx);
    ctx.storage->cache_measurement(ago);

    if (state.tracking_active) {
      float battery_pct = -1.0f;
      ctx.bms->get_battery_percentage(&battery_pct);
      ctx.storage->start_route(state.tracking_session_id);
      RoutePoint point{};
      point.timestamp = time(nullptr);
      point.gps = gps;
      point.sensors = ago;
      point.battery_percentage = battery_pct;
      ctx.storage->append_route_point(point);
      ctx.storage->end_route();
    }
    ctx.storage->backup_cache();
  }

  // --- Power service + display + sleep (skip if promoting) ---
  if (!promote) {
    init_power(ctx);
    PowerSnapshot bms_snap = ctx.power_service->poll_bms();

    // --- Display ---
    init_display(ctx);
    DisplayValues values =
        build_fast_path_display(ago, gps, bms_snap, ctx.settings, state.tracking_active);
    ctx.display->init(values);

    // --- Sleep decision ---
    uint32_t awake_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - boot_time_ms;
    auto decision = ctx.power_service->decide_sleep(ctx.settings, LockState::Locked,
                                                    OperatingMode::Offline, awake_ms);

    if (decision.type == PowerService::SleepType::Deep) {
      // Persist RTC state with the warm-sensor flag for the next wake cycle.
      RtcAppState save = state;
      save.sensors_warm = ctx.power_service->should_hold_pm_sensor(decision.duration_ms);
      ctx.power_service->save_state(save);

      ctx.display->stop();
      ctx.display->deep_sleep();
      gpio_isr_handler_remove(PIN_BUTTON_POWER);
      ctx.power_service->enter_sleep(decision.duration_ms);
      // Never returns — CPU reboots on wake.
    }

    // Sleep too short — promote to interactive, stay locked.
    promote = true;
  }

  // --- Remove ISR before InputService takes over ---
  gpio_isr_handler_remove(PIN_BUTTON_POWER);
  gpio_set_intr_type(PIN_BUTTON_POWER, GPIO_INTR_DISABLE);

  // --- Build handoff ---
  const bool button_caused_promote = s_button_pressed;

  // Load RTC snapshot for button promotion — used to seed the initial
  // display with stale values (same as button wake path) so the user
  // sees sensor data instead of dashes while waiting for fresh readings.
  RtcDisplaySnapshot snapshot{};
  bool snapshot_valid = false;
  if (button_caused_promote) {
    snapshot_valid = load_rtc_display_snapshot(&snapshot);
  }

  BootHandoff handoff{};
  handoff.measurement_completed = has_measures;
  handoff.fast_path_measures = has_measures ? &ago : nullptr;

  if (button_caused_promote) {
    // User pressed button during fast path — unlock, suppress wake press.
    handoff.initial_lock_state = LockState::Unlocked;
    handoff.suppress_wake_press = true;
    handoff.display_snapshot = snapshot_valid ? &snapshot : nullptr;
    // Display may or may not be initialized depending on where we
    // interrupted. If ctx.display is non-null, the display is showing
    // the locked fast-path frame — NOT valid for unlocked state.
    handoff.display_painted = false;
  } else {
    // Sleep too short — stay locked. Display shows correct locked frame.
    handoff.initial_lock_state = LockState::Locked;
    handoff.display_painted = (ctx.display != nullptr);
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "fast-path promoting to interactive (button=%d)",
          static_cast<int>(button_caused_promote));
  run_interactive(WakeCause::Timer, ctx, handoff);
  // Never returns.
}

// ===========================================================================
// Button-wake boot (button wake, Offline mode)
//
// Shows the Home + Unlocked + "Unlocked" snackbar frame on the first and
// only display refresh while initializing all hardware in parallel.
//
// Four phases:
//   Phase 1 (~10 ms)   Early paint — render frame, hand off SPI refresh
//                       to the display worker.
//   Phase 2 (~300 ms)  Parallel init — NVS, I2C, BMS, sensors, GPS, touch,
//                       BLE, event queue.  Nothing here uses SPI.
//   Phase 3 (~3 s)     NAND init — blocks on SPI until display refresh done.
//   Phase 4 (~10 ms)   Orchestrator init + run — never returns.
// ===========================================================================

static void run_button_wake_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_button_wake_path: entering button-wake boot");

  BootContext ctx;

  // -------------------------------------------------------------------------
  // Phase 1: Early paint (~10 ms)
  // -------------------------------------------------------------------------

  init_spi(ctx);
  init_display(ctx);

  RtcDisplaySnapshot snapshot{};
  const bool snapshot_valid = load_rtc_display_snapshot(&snapshot);
  DisplayValues wake_values = build_wake_values(snapshot, snapshot_valid);

  // Returns in ~10 ms.  Worker task handles the SPI full refresh (~3 s).
  ctx.display->init(wake_values, /* defer_refresh= */ true);

  // -------------------------------------------------------------------------
  // Phase 2: Parallel init (~300 ms)
  // Non-SPI peripherals — runs while the display refreshes in the background.
  // -------------------------------------------------------------------------

  init_core_no_spi(ctx);
  init_sensors(ctx);

  // GPS serial + driver (UART — no SPI contention)
  auto *gps_serial = new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  auto *gps_driver = new GpsDriver(*gps_serial);

  // Touch sensor (I2C)
  CAP1203::Config touch_cfg;
  touch_cfg.delta_sense = TOUCH_DELTA_SENSE;
  auto *touch = new CAP1203(ctx.i2c_bus, I2C_ADDR_CAP1203, touch_cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  // Event queue
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // Construct producer services; reuse display from Phase 1
  auto *sensor_producer = new SensorProducer(*ctx.sensor_manager, event_queue,
                                             {
                                                 .task_stack_size = 4096,
                                                 .task_priority = 5,
                                             });

  auto *gps_service =
      new GpsService(*gps_driver, event_queue,
                     {
                         .baud_rate = GPS_BAUD,
                         .posting_interval_ms = ctx.settings.gps_interval_seconds * 1000,
                         .task_stack_size = 4096,
                         .task_priority = 3,
                     });

  // InputService: suppress the first ButtonPower event (the wake press)
  auto *input_service = new InputService(*touch, gpio::native::hal, event_queue,
                                         {
                                             .pin_cap_int = PIN_CAP_INT,
                                             .pin_button_power = PIN_BUTTON_POWER,
                                             .pin_button_boot = PIN_BUTTON_BOOT,
                                             .suppress_button_wake = true,
                                         });

  init_power(ctx);

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial_number.c_str());

  auto *ui_manager = new UIManager({
      .firmware_version = FIRMWARE_VERSION,
      .serial_number = serial_number.c_str(),
  });

  // Start producer tasks — touch and sensors operational from here (~310 ms)
  sensor_producer->start();
  gps_service->start();
  input_service->start();

  // -------------------------------------------------------------------------
  // Phase 3: Storage init (blocks on SPI until display refresh finishes ~3 s)
  // -------------------------------------------------------------------------

  init_storage(ctx);

  // BleService construction requires StorageService (must come after NAND init)
  auto *ble_service = new BleService(event_queue, *ctx.storage);

  // -------------------------------------------------------------------------
  // Phase 4: Orchestrator — display + all services ready
  // -------------------------------------------------------------------------

  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = *ctx.display,
      .storage_service = *ctx.storage,
      .power_service = *ctx.power_service,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
  };

  auto *orchestrator = new Orchestrator(event_queue, services, ctx.settings, *ctx.config_store,
                                        serial_number.c_str());

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
//
// Handles both fresh boot (empty BootContext) and fast-path promotion
// (partially filled BootContext).
// ===========================================================================

static void run_interactive(WakeCause cause, BootContext &ctx, BootHandoff handoff) {
  // --- Complete any missing core init ---
  if (!ctx.config_store) {
    init_core(ctx);
  }
  if (!ctx.sensor_manager) {
    init_sensors(ctx);
  }

  // --- GPS driver (never done in fast path) ---
  auto *gps_serial = new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  auto *gps_driver = new GpsDriver(*gps_serial);

  // --- Touch sensor (never done in fast path) ---
  CAP1203::Config touch_cfg;
  touch_cfg.delta_sense = TOUCH_DELTA_SENSE;
  auto *touch = new CAP1203(ctx.i2c_bus, I2C_ADDR_CAP1203, touch_cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  // --- Storage (may already be done from fast path) ---
  if (!ctx.storage) {
    init_storage(ctx);
  }

  // --- Event queue ---
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // --- BLE ---
  auto *ble_service = new BleService(event_queue, *ctx.storage);

  // --- Service construction ---
  auto *sensor_producer = new SensorProducer(*ctx.sensor_manager, event_queue,
                                             {.task_stack_size = 4096, .task_priority = 5});

  auto *gps_service =
      new GpsService(*gps_driver, event_queue,
                     {.baud_rate = GPS_BAUD,
                      .posting_interval_ms = ctx.settings.gps_interval_seconds * 1000,
                      .task_stack_size = 4096,
                      .task_priority = 3});

  auto *input_service = new InputService(*touch, gpio::native::hal, event_queue,
                                         {.pin_cap_int = PIN_CAP_INT,
                                          .pin_button_power = PIN_BUTTON_POWER,
                                          .pin_button_boot = PIN_BUTTON_BOOT,
                                          .suppress_button_wake = handoff.suppress_wake_press});

  if (!ctx.display) {
    init_display(ctx);
  }

  if (!ctx.power_service) {
    init_power(ctx);
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial_number.c_str());

  auto *ui_manager = new UIManager({
      .firmware_version = FIRMWARE_VERSION,
      .serial_number = serial_number.c_str(),
  });

  // --- Display init (if boot hasn't painted) ---
  if (!handoff.display_painted) {
    if (handoff.display_snapshot != nullptr) {
      // Promotion with RTC snapshot: show stale values + unlocked state
      // so the user sees data instead of dashes while waiting for fresh
      // readings. Same pattern as the button wake early paint.
      DisplayValues wake = build_wake_values(*handoff.display_snapshot, true);
      ctx.display->init(wake);
    } else {
      DisplayValues initial{};
      ctx.display->init(initial);
    }
    // Display is now painted — tell orchestrator so it doesn't repaint.
    handoff.display_painted = true;
  }

  // --- Start producer tasks ---
  sensor_producer->start();
  gps_service->start();
  input_service->start();

  // --- Orchestrator ---
  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = *ctx.display,
      .storage_service = *ctx.storage,
      .power_service = *ctx.power_service,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
  };

  auto *orchestrator = new Orchestrator(event_queue, services, ctx.settings, *ctx.config_store,
                                        serial_number.c_str());
  orchestrator->init(cause, handoff);
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// Low-level init functions (unchanged)
// ===========================================================================

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

static i2c_master_bus_handle_t init_i2c_bus() {
  i2c_master_bus_config_t config = {
      .i2c_port = I2C_MASTER_PORT,
      .sda_io_num = PIN_I2C_SDA,
      .scl_io_num = PIN_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags =
          {
              .enable_internal_pullup = I2C_INTERNAL_PULLUPS,
              .allow_pd = false,
          },
  };

  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
  AG_LOGI(TAG, "I2C bus ready");
  return bus;
}

static void init_gpio() {
  auto &hal = gpio::native::hal;

  // PM sensor (SPS30) power enable — active-high
  hal.configure(PIN_PM_POWER, gpio::Mode::Output, gpio::PullMode::Floating,
                gpio::InterruptType::Disabled);
  gpio_set_drive_capability(PIN_PM_POWER, GPIO_DRIVE_CAP_3);
  hal.set_level(PIN_PM_POWER, 1);
}

static void init_spi_buses() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = PIN_SPI_MISO;
  bus.sclk_io_num = PIN_SPI_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO));

  AG_LOGI(TAG, "SPI bus ready");
}

static BQ25629Bms *init_bms(i2c_master_bus_handle_t i2c_bus) {
  constexpr drivers::BQ25629_Config config = {
      .charge_voltage_mv = 4200,
      .charge_current_ma = 500,
      .input_current_limit_ma = 1500,
      .input_voltage_limit_mv = 4600,
      .min_system_voltage_mv = 3520,
      .precharge_current_ma = 30,
      .term_current_ma = 20,
      .enable_charging = true,
      .enable_adc = true,
  };
  return new BQ25629Bms(i2c_bus, config, I2C_ADDR_BMS);
}

static CO2Sensor *init_co2_sensor(i2c_master_bus_handle_t i2c_bus) {
  // 1. SenseAir S12 (no integrated T/RH)
  auto *s12 = new S12(i2c_bus, I2C_ADDR_S12);
  if (s12->init()) {
    AG_LOGI(TAG, "CO2 sensor: S12 selected");
    return s12;
  }
  AG_LOGW(TAG, "CO2 sensor: S12 not detected");
  delete s12;

  // 2. Sensirion SCD4x (with integrated T/RH)
  auto *scd4x = new SCD4x(i2c_bus, I2C_ADDR_SCD4X);
  if (scd4x->init()) {
    AG_LOGI(TAG, "CO2 sensor: SCD4x selected");
    return scd4x;
  }
  AG_LOGW(TAG, "CO2 sensor: SCD4x not detected");
  delete scd4x;

  // 3. Sensirion STCC4 (with integrated T/RH)
  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  if (stcc4->init()) {
    AG_LOGI(TAG, "CO2 sensor: STCC4 selected");
    return stcc4;
  }
  AG_LOGW(TAG, "CO2 sensor: STCC4 not detected");
  delete stcc4;

  AG_LOGE(TAG, "CO2 sensor init failed (all candidates)");
  return nullptr;
}

static MeasuresAGo measures_to_ago(const Measures &m) {
  MeasuresAGo ago{};
  ago.temp_hum_a = m.temp_hum_a;
  ago.pm_a = m.pm_a;
  ago.co2 = m.co2;
  ago.tvoc_nox = m.tvoc_nox;
  ago.power = m.power;
  ago.pressure = m.pressure;

  // TODO: Temporarily use raw value for index since algorithm not applied yet
  ago.tvoc_nox.tvoc_index = ago.tvoc_nox.tvoc_raw;
  ago.tvoc_nox.nox_index = ago.tvoc_nox.nox_raw;

  return ago;
}

static DisplayValues build_fast_path_display(const MeasuresAGo &measures, const GpsData &gps,
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

static DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot, bool snapshot_valid) {
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
