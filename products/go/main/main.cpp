/**
 * AirGradient Go — Application Entry Point
 *
 * Boot path selection, hardware initialization, and fast-path boot for
 * the AirGradient Go portable air quality monitor.
 *
 * Three boot paths:
 *   1. Fresh power-on   -> full initialization with default state
 *   2. Timer wake        -> fast-path: measure, display, sleep (no event loop)
 *   3. Button wake       -> full initialization, restore state, unlock
 *
 * All objects are stack-allocated in the boot function they belong to.
 * Since the orchestrator's run() never returns, they live for the duration
 * of the program.
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
#include "drivers/nmea_gps/nmea_gps.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
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
#include "go_gps.h"
#include "go_input.h"
#include "go_orchestrator.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"

static constexpr const char *TAG = "main";

static constexpr const char *FIRMWARE_VERSION = "0.1.0";
static constexpr bool PM_STRESS_TEST_ENABLED = true;
static constexpr int PM_STRESS_FALLBACK_INTERVAL_SECONDS = 10;
static constexpr uint32_t PM_STRESS_SENSOR_BOOT_DELAY_MS = 1500;
static constexpr uint32_t PM_STRESS_OFF_SAMPLE_DELAY_MS = 100;
static constexpr uint32_t PM_STRESS_WATCHDOG_SERVICE_INTERVAL_MS = 1000;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void run_pm_stress_test();
static void run_fast_path(const RtcAppState &state);
static void run_button_wake_path(const RtcAppState &state);
static void run_full_boot(WakeCause cause, const char *serial_number);

static void init_nvs();
static i2c_master_bus_handle_t init_i2c_bus();
static void init_gpio();
static void init_spi_buses();
static BQ25629Bms *init_bms(i2c_master_bus_handle_t i2c_bus);
static MeasuresAGo measures_to_ago(const Measures &m);
static void delay_with_watchdogs(PowerService &power_service, uint32_t delay_ms);
static DisplayValues build_pm_stress_display(const PMData *pm, const Measures *other,
                                             const PowerSnapshot &bms, const GoSettings &settings);
static DisplayValues build_fast_path_display(const Measures &measures, const GpsData &gps,
                                             const PowerSnapshot &bms, const GoSettings &settings);
static DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot, bool snapshot_valid);

// ===========================================================================
// app_main — boot path selection
// ===========================================================================

extern "C" void app_main() {
  if (PM_STRESS_TEST_ENABLED) {
    run_pm_stress_test();
    return;
  }

  RTOS::delay_ms(100);
  WakeCause cause = PowerService::get_wake_cause();

  if (cause == WakeCause::Timer) {
    RtcAppState state = load_rtc_app_state();
    if (PowerService::is_fast_path_wake(cause, state)) {
      run_fast_path(state);
      // Never returns.
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
  run_full_boot(cause, serial_number.c_str());
  // Never returns.
}

// ===========================================================================
// PM stress test boot
//
// Test-branch runtime: power the PMID rail only for each SPS30 reading,
// then turn it back off. Logs PM + other-sensor + PMID telemetry and updates
// the Home screen with the latest valid values.
// ===========================================================================

static void run_pm_stress_test() {
  AG_LOGI(TAG, "run_pm_stress_test: entering PM-only stress loop");

  init_nvs();
  auto *config_store = new NvsConfigStore("go");
  GoSettings settings = load_go_settings(*config_store);
  print_settings(settings);

  uint32_t pm_interval_ms = static_cast<uint32_t>((settings.pm_interval_seconds > 0)
                                                      ? settings.pm_interval_seconds
                                                      : PM_STRESS_FALLBACK_INTERVAL_SECONDS) *
                            1000;
  if (settings.pm_interval_seconds <= 0) {
    AG_LOGW(TAG, "pm stress: pm_interval_seconds disabled, using fallback %d s",
            PM_STRESS_FALLBACK_INTERVAL_SECONDS);
  }

  init_gpio();
  RTOS::delay_ms(100);

  i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);

  init_spi_buses();

  auto *bms = init_bms(i2c_bus);
  if (!bms->init()) {
    AG_LOGE(TAG, "pm stress: BMS init failed");
  }

  auto *display = new DisplayService({
      .spi_host = SPI_HOST,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });

  auto *power_service = new PowerService(*bms, gpio::native::hal,
                                         {
                                             .pin_wake_button_power = PIN_BUTTON_POWER,
                                             .pin_wake_button_boot = -1,
                                             .pin_ext_wdt = PIN_EXT_WDT,
                                         });

  power_service->init_ext_watchdog();
  power_service->reset_ext_watchdog();
  power_service->reset_watchdog();

  if (!bms->disable_boost()) {
    AG_LOGW(TAG, "pm stress: failed to force PMID off after BMS init");
  }
  delay_with_watchdogs(*power_service, PM_STRESS_OFF_SAMPLE_DELAY_MS);

  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  auto *sgp41 = new SGP41(i2c_bus, I2C_ADDR_SGP41);
  auto *dps368 = new DPS368(i2c_bus, I2C_ADDR_DPS368);

  Sensors sensors{};

  if (stcc4->init()) {
    sensors.co2 = stcc4;
  } else {
    AG_LOGE(TAG, "pm stress: STCC4 init failed");
  }

  if (sgp41->init()) {
    sensors.tvoc_nox = sgp41;
  } else {
    AG_LOGE(TAG, "pm stress: SGP41 init failed");
  }

  if (dps368->init()) {
    sensors.pressure = dps368;
  } else {
    AG_LOGE(TAG, "pm stress: DPS368 init failed");
  }

  sensors.temp_hum_a_fallback.priority[0] = TempHumSource::CO2;
  sensors.temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
  sensors.temp_hum_a_fallback.count = 2;

  auto *sensor_manager = new SensorManager(sensors);

  PowerSnapshot initial_bms = power_service->poll_bms();
  display->init(build_pm_stress_display(nullptr, nullptr, initial_bms, settings));

  auto *sps30 = new SPS30(i2c_bus);
  bool sps30_initialized = false;

  while (true) {
    const uint32_t cycle_start_ms = static_cast<uint32_t>(RTOS::get_time_ms());

    const Measures other_measures = sensor_manager->start_measures(1, SensorGroup::Other);

    AG_LOGI(TAG, "pm stress: PMID ON");
    bool boost_enabled = bms->enable_boost();
    power_service->reset_watchdog();
    power_service->reset_ext_watchdog();

    if (!boost_enabled) {
      AG_LOGE(TAG, "pm stress: enable boost failed");
      PowerSnapshot bms_snap = power_service->poll_bms();
      (void)display->update(build_pm_stress_display(nullptr, &other_measures, bms_snap, settings),
                            true);
      delay_with_watchdogs(*power_service, pm_interval_ms);
      continue;
    }

    delay_with_watchdogs(*power_service, PM_STRESS_SENSOR_BOOT_DELAY_MS);

    if (!sps30_initialized) {
      sps30_initialized = sps30->init();
      if (!sps30_initialized) {
        AG_LOGE(TAG, "pm stress: SPS30 init failed");
      }
    }

    PMData pm = {
        .pm_01 = MeasuresInvalid::PM,
        .pm_25 = MeasuresInvalid::PM,
        .pm_10 = MeasuresInvalid::PM,
        .pm_01_sp = MeasuresInvalid::PM,
        .pm_25_sp = MeasuresInvalid::PM,
        .pm_10_sp = MeasuresInvalid::PM,
        .pm_03_pc = MeasuresInvalid::PM,
        .pm_05_pc = MeasuresInvalid::PM,
        .pm_01_pc = MeasuresInvalid::PM,
        .pm_25_pc = MeasuresInvalid::PM,
        .pm_5_pc = MeasuresInvalid::PM,
        .pm_10_pc = MeasuresInvalid::PM,
    };
    const bool read_ok = sps30_initialized && sps30->read(pm);

    PowerSnapshot on_bms = power_service->poll_bms();
    power_service->reset_watchdog();

    const int log_co2 =
        other_measures.co2.is_valid() ? other_measures.co2.co2 : MeasuresInvalid::CO2;
    const float log_temp = other_measures.temp_hum_a.is_temp_valid()
                               ? other_measures.temp_hum_a.temperature
                               : MeasuresInvalid::TEMPERATURE;
    const float log_hum = other_measures.temp_hum_a.is_hum_valid()
                              ? other_measures.temp_hum_a.humidity
                              : MeasuresInvalid::HUMIDITY;
    const int log_tvoc = other_measures.tvoc_nox.is_tvoc_index_valid()
                             ? other_measures.tvoc_nox.tvoc_index
                             : MeasuresInvalid::TVOC;
    const int log_nox = other_measures.tvoc_nox.is_nox_index_valid()
                            ? other_measures.tvoc_nox.nox_index
                            : MeasuresInvalid::NOX;
    const float log_pressure = other_measures.pressure.is_pressure_valid()
                                   ? other_measures.pressure.pressure
                                   : MeasuresInvalid::PM;

    if (read_ok) {
      AG_LOGI(TAG,
              "pm stress: PM1.0=%.1f PM2.5=%.1f PM10=%.1f ug/m3 | CO2=%d TVOC=%d NOX=%d T=%.1fC "
              "H=%.1f%% P=%.1fhPa | PMID=%umV battery=%.1f%%",
              pm.pm_01, pm.pm_25, pm.pm_10, log_co2, log_tvoc, log_nox, log_temp, log_hum,
              log_pressure, on_bms.telemetry.pmid_voltage_mv, on_bms.battery_percentage);
    } else {
      AG_LOGW(TAG,
              "pm stress: SPS30 read failed | CO2=%d TVOC=%d NOX=%d T=%.1fC H=%.1f%% P=%.1fhPa | "
              "PMID=%umV battery=%.1f%%",
              log_co2, log_tvoc, log_nox, log_temp, log_hum, log_pressure,
              on_bms.telemetry.pmid_voltage_mv, on_bms.battery_percentage);
    }

    AG_LOGI(TAG, "pm stress: PMID OFF");
    if (!bms->disable_boost()) {
      AG_LOGE(TAG, "pm stress: disable boost failed");
    }

    delay_with_watchdogs(*power_service, PM_STRESS_OFF_SAMPLE_DELAY_MS);

    PowerSnapshot off_bms = power_service->poll_bms();
    power_service->reset_watchdog();
    AG_LOGI(TAG, "pm stress: PMID after OFF = %umV", off_bms.telemetry.pmid_voltage_mv);

    if (!display->update(
            build_pm_stress_display(read_ok ? &pm : nullptr, &other_measures, off_bms, settings),
            true)) {
      AG_LOGW(TAG, "pm stress: display update skipped");
    }

    const uint32_t elapsed_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - cycle_start_ms;
    if (elapsed_ms < pm_interval_ms) {
      delay_with_watchdogs(*power_service, pm_interval_ms - elapsed_ms);
    } else {
      AG_LOGW(TAG, "pm stress: cycle overran interval (%lu ms >= %lu ms)",
              static_cast<unsigned long>(elapsed_ms), static_cast<unsigned long>(pm_interval_ms));
    }
  }
}

// ===========================================================================
// Fast-path boot (timer wake, locked)
//
// Minimal initialization — no event loop, no producer tasks, no input
// handling.  Goal: measure, display, sleep — as fast as possible to
// conserve battery.
// ===========================================================================

static void run_fast_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_fast_path: entering fast-path boot");
  const uint32_t boot_time_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // All driver and service objects are heap-allocated because this function
  // never returns (enter_sleep reboots the CPU).  Heap allocation keeps
  // the main task stack small; the objects live for the program's lifetime.

  // --- 0. NVS + settings (needed for sleep duration, GPS mode) ---
  init_nvs();
  auto *config_store = new NvsConfigStore("go");
  GoSettings settings = load_go_settings(*config_store);
  print_settings(settings);

  // --- 1. GPIO (sensor power enables) ---
  init_gpio();
  RTOS::delay_ms(100);

  // --- 2. I2C bus ---
  i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);

  // --- 3. SPI buses (needed for NAND and display) ---
  init_spi_buses();

  // --- 4. BMS (must init before sensors — SPS30 is on the PMID 5V rail) ---
  auto *bms = init_bms(i2c_bus);
  if (!bms->init()) {
    AG_LOGE(TAG, "BMS init failed (fast path)");
  }

  // --- 5. Sensor drivers (same construction as full boot) ---
  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  auto *sgp41 = new SGP41(i2c_bus, I2C_ADDR_SGP41);
  auto *sps30 = new SPS30(i2c_bus);
  auto *dps368 = new DPS368(i2c_bus, I2C_ADDR_DPS368);

  Sensors sensors{};

  if (stcc4->init()) {
    sensors.co2 = stcc4;
  } else {
    AG_LOGE(TAG, "STCC4 init failed");
  }

  if (sgp41->init()) {
    sensors.tvoc_nox = sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }

  if (sps30->init()) {
    sensors.pms_a = sps30;
  } else {
    AG_LOGE(TAG, "SPS30 init failed");
  }

  if (dps368->init()) {
    sensors.pressure = dps368;
  } else {
    AG_LOGE(TAG, "DPS368 init failed");
  }

  // No dedicated temp/hum sensor; fallback to STCC4 (CO2) then DPS368 (pressure)
  sensors.temp_hum_a_fallback.priority[0] = TempHumSource::CO2;
  sensors.temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
  sensors.temp_hum_a_fallback.count = 2;

  auto *sensor_manager = new SensorManager(sensors);

  // --- 5. One-shot measurement (blocking, single iteration) ---
  Measures measures = sensor_manager->start_measures(1, SensorGroup::All);
  AG_LOGI(TAG,
          "fast-path: temp=%.1f hum=%.1f pm25=%.1f co2=%d tvoc=%d nox=%d tvoc_raw=%d nox_raw=%d "
          "pres=%.1f",
          measures.temp_hum_a.temperature, measures.temp_hum_a.humidity, measures.pm_a.pm_25,
          measures.co2.co2, measures.tvoc_nox.tvoc_index, measures.tvoc_nox.nox_index,
          measures.tvoc_nox.tvoc_raw, measures.tvoc_nox.nox_raw, measures.pressure.pressure);

  // TODO: Temporarily use raw value for index since algorithm not applied yet
  measures.tvoc_nox.tvoc_index = measures.tvoc_nox.tvoc_raw;
  measures.tvoc_nox.nox_index = measures.tvoc_nox.nox_raw;

  // --- 6. One-shot GPS (if tracking + GPS active) ---
  GpsData gps{};
  const bool gps_active = (settings.gps_mode == GpsMode::AlwaysOn) ||
                          (settings.gps_mode == GpsMode::OnWhenTracking && state.tracking_active);

  if (state.tracking_active && gps_active) {
    AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
    NmeaGps nmea_gps(gps_serial);
    gps = gps_read_once(nmea_gps, GPS_BAUD, 2000);
  }

  // --- 7. Storage ---
  auto *rtc_storage = new RtcPayloadCacheStorage();
  auto *cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST;
  nand_config.cs_pin = PIN_NAND_CS;
  auto *nand = new SpiNandStorage(nand_config);

  auto *storage = new StorageService(*cache, *nand);
  storage->restore_cache();
  if (!storage->init()) {
    AG_LOGE(TAG, "NAND storage init failed (fast path)");
  }

  MeasuresAGo ago = measures_to_ago(measures);
  storage->cache_measurement(ago);

  if (state.tracking_active) {
    storage->start_route(state.tracking_session_id);
    RoutePoint point{};
    point.timestamp = time(nullptr);
    point.gps = gps;
    point.sensors = ago;
    storage->append_route_point(point);
    storage->end_route();
  }

  storage->backup_cache();

  // --- 8. Power service
  auto *power_service =
      new PowerService(*bms, gpio::native::hal,
                       {
                           .pin_wake_button_power = PIN_BUTTON_POWER,
                           .pin_wake_button_boot = -1, // GPIO28 is not RTC-capable
                           .pin_ext_wdt = PIN_EXT_WDT,
                       });

  power_service->init_ext_watchdog();
  power_service->reset_ext_watchdog();

  PowerSnapshot bms_snap = power_service->poll_bms();
  power_service->reset_watchdog();

  // --- 9. Display (synchronous, no worker task) ---
  auto *display = new DisplayService({
      .spi_host = SPI_HOST,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });

  DisplayValues values = build_fast_path_display(measures, gps, bms_snap, settings);
  display->init(values);

  // --- 10. Save state and re-enter deep sleep ---
  power_service->save_state(state);
  uint32_t awake_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - boot_time_ms;
  auto decision =
      power_service->decide_sleep(settings, LockState::Locked, OperatingMode::Offline, awake_ms);
  AG_LOGI(TAG, "fast-path awake %lu ms, sleeping %lu ms", static_cast<unsigned long>(awake_ms),
          static_cast<unsigned long>(decision.duration_ms));
  if (decision.type == PowerService::SleepType::Deep) {
    power_service->enter_sleep(decision.duration_ms);
    // Never returns — CPU reboots on wake.
  }
  // decision.type == None: interval too short for deep sleep (sleep_ms <
  // deep_sleep_threshold_ms).  Fast path already measured and updated the
  // display.  Return to app_main which falls through to run_full_boot() for
  // a full event-loop session.
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

  // -------------------------------------------------------------------------
  // Phase 1: Early paint (~10 ms)
  // -------------------------------------------------------------------------

  // SPI bus must be up before the display driver can attach its device.
  init_spi_buses();

  auto *display_service = new DisplayService({
      .spi_host = SPI_HOST,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });

  RtcDisplaySnapshot snapshot{};
  const bool snapshot_valid = load_rtc_display_snapshot(&snapshot);
  DisplayValues wake_values = build_wake_values(snapshot, snapshot_valid);

  // Returns in ~10 ms.  Worker task handles the SPI full refresh (~3 s).
  display_service->init(wake_values, /* defer_refresh= */ true);

  // -------------------------------------------------------------------------
  // Phase 2: Parallel init (~300 ms)
  // Non-SPI peripherals — runs while the display refreshes in the background.
  // -------------------------------------------------------------------------

  init_nvs();
  auto *config_store = new NvsConfigStore("go");
  GoSettings settings = load_go_settings(*config_store);

  init_gpio();
  RTOS::delay_ms(100);

  i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);
  // SPI bus already initialized in Phase 1 — do not call init_spi_buses() again.

  print_settings(settings);

  auto *bms = init_bms(i2c_bus);
  if (!bms->init()) {
    AG_LOGE(TAG, "BMS init failed (button wake)");
  }

  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  auto *sgp41 = new SGP41(i2c_bus, I2C_ADDR_SGP41);
  auto *sps30 = new SPS30(i2c_bus);
  auto *dps368 = new DPS368(i2c_bus, I2C_ADDR_DPS368);

  Sensors sensors{};

  if (stcc4->init()) {
    sensors.co2 = stcc4;
  } else {
    AG_LOGE(TAG, "STCC4 init failed");
  }

  if (sgp41->init()) {
    sensors.tvoc_nox = sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }

  if (sps30->init()) {
    sensors.pms_a = sps30;
  } else {
    AG_LOGE(TAG, "SPS30 init failed");
  }

  if (dps368->init()) {
    sensors.pressure = dps368;
  } else {
    AG_LOGE(TAG, "DPS368 init failed");
  }

  sensors.temp_hum_a_fallback.priority[0] = TempHumSource::CO2;
  sensors.temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
  sensors.temp_hum_a_fallback.count = 2;

  auto *sensor_manager = new SensorManager(sensors);

  // GPS serial + driver (UART — no SPI contention)
  auto *gps_serial = new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  auto *nmea_gps = new NmeaGps(*gps_serial);

  // Touch sensor (I2C)
  CAP1203::Config touch_cfg;
  touch_cfg.delta_sense = TOUCH_DELTA_SENSE;
  auto *touch = new CAP1203(i2c_bus, I2C_ADDR_CAP1203, touch_cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  // Payload cache: RTC memory — no SPI
  auto *rtc_storage = new RtcPayloadCacheStorage();
  auto *cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  // Event queue
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // Construct producer services; reuse display_service from Phase 1
  auto *sensor_producer = new SensorProducer(*sensor_manager, event_queue,
                                             {
                                                 .task_stack_size = 4096,
                                                 .task_priority = 5,
                                             });

  auto *gps_service =
      new GpsService(*nmea_gps, event_queue,
                     {
                         .baud_rate = GPS_BAUD,
                         .posting_interval_ms = settings.gps_interval_seconds * 1000,
                         .task_stack_size = 4096,
                         .task_priority = 5,
                     });

  // InputService: suppress the first ButtonPower event (the wake press)
  auto *input_service = new InputService(*touch, gpio::native::hal, event_queue,
                                         {
                                             .pin_cap_int = PIN_CAP_INT,
                                             .pin_button_power = PIN_BUTTON_POWER,
                                             .pin_button_boot = PIN_BUTTON_BOOT,
                                             .suppress_button_wake = true,
                                         });

  auto *power_service =
      new PowerService(*bms, gpio::native::hal,
                       {
                           .pin_wake_button_power = PIN_BUTTON_POWER,
                           .pin_wake_button_boot = -1, // GPIO28 is not RTC-capable
                           .pin_ext_wdt = PIN_EXT_WDT,
                       });

  power_service->init_ext_watchdog();
  power_service->reset_ext_watchdog();

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
  //
  // SpiNandStorage::init() calls spi_device_transmit() internally.  While the
  // display worker holds the SPI bus via spi_device_acquire_bus(), this call
  // blocks.  No explicit synchronization needed — bus serialization handles it.

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST;
  nand_config.cs_pin = PIN_NAND_CS;
  auto *nand = new SpiNandStorage(nand_config);

  auto *storage = new StorageService(*cache, *nand);
  storage->restore_cache();
  if (!storage->init()) {
    AG_LOGE(TAG, "NAND storage init failed — route persistence unavailable");
  }

  // BleService construction requires StorageService (must come after NAND init)
  auto *ble_service = new BleService(event_queue, *storage);

  // -------------------------------------------------------------------------
  // Phase 4: Orchestrator — display + all services ready
  // -------------------------------------------------------------------------

  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = *display_service,
      .storage_service = *storage,
      .power_service = *power_service,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
  };

  auto *orchestrator =
      new Orchestrator(event_queue, services, settings, *config_store, serial_number.c_str());
  // already_painted=true: init() sets lock state, arms snackbar timer, and
  // resumes any active route; skips update_display() (first live update
  // comes from the event loop once sensors deliver data).
  orchestrator->init(WakeCause::Button, /* already_painted= */ true);
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// Full boot (fresh power-on or button wake)
//
// Initializes all hardware and services in dependency order, then hands
// control to the Orchestrator.
// ===========================================================================

static void run_full_boot(WakeCause cause, const char *serial_number) {
  AG_LOGI(TAG, "run_full_boot: cause=%d", static_cast<int>(cause));

  // All driver and service objects are heap-allocated because this function
  // never returns (orchestrator.run() loops forever).  Heap allocation keeps
  // the main task stack small; the objects live for the program's lifetime.

  // --- 1. NVS ---
  init_nvs();

  // --- 2. Settings ---
  auto *config_store = new NvsConfigStore("go");
  GoSettings settings = load_go_settings(*config_store);

  // --- 3. GPIO (power enables, initial levels) ---
  init_gpio();
  RTOS::delay_ms(100);

  // --- 4. I2C bus ---
  i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);

  // --- 5. SPI bus(es) ---
  init_spi_buses();

  print_settings(settings);

  // --- 6. BMS ---
  auto *bms = init_bms(i2c_bus);
  if (!bms->init()) {
    AG_LOGE(TAG, "BMS init failed");
  }

  // ---7. Sensor drivers ---
  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  auto *sgp41 = new SGP41(i2c_bus, I2C_ADDR_SGP41);
  auto *sps30 = new SPS30(i2c_bus);
  auto *dps368 = new DPS368(i2c_bus, I2C_ADDR_DPS368);

  Sensors sensors{};

  if (stcc4->init()) {
    sensors.co2 = stcc4;
  } else {
    AG_LOGE(TAG, "STCC4 init failed");
  }

  if (sgp41->init()) {
    sensors.tvoc_nox = sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }

  if (sps30->init()) {
    sensors.pms_a = sps30;
  } else {
    AG_LOGE(TAG, "SPS30 init failed");
  }

  if (dps368->init()) {
    sensors.pressure = dps368;
  } else {
    AG_LOGE(TAG, "DPS368 init failed");
  }

  // No dedicated temp/hum sensor; fallback to STCC4 (CO2) then DPS368 (pressure)
  sensors.temp_hum_a_fallback.priority[0] = TempHumSource::CO2;
  sensors.temp_hum_a_fallback.priority[1] = TempHumSource::PRESSURE;
  sensors.temp_hum_a_fallback.count = 2;

  // --- 8. Sensors struct + SensorManager ---
  auto *sensor_manager = new SensorManager(sensors);

  // --- 9. GPS ---
  // UART begin() is called by GpsService::run() when the task starts;
  // calling it here would cause a "UART already initialized" warning.
  auto *gps_serial = new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  auto *nmea_gps = new NmeaGps(*gps_serial);

  // --- 10. Touch ---
  CAP1203::Config touch_cfg;
  touch_cfg.delta_sense = TOUCH_DELTA_SENSE;
  auto *touch = new CAP1203(i2c_bus, I2C_ADDR_CAP1203, touch_cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  // --- 11. Storage ---
  auto *rtc_storage = new RtcPayloadCacheStorage();
  auto *cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST;
  nand_config.cs_pin = PIN_NAND_CS;
  auto *nand = new SpiNandStorage(nand_config);

  auto *storage = new StorageService(*cache, *nand);
  storage->restore_cache();
  if (!storage->init()) {
    AG_LOGE(TAG, "NAND storage init failed — route persistence unavailable");
  }

  // --- 12. Event queue ---
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // --- 12b. BLE service ---
  auto *ble_service = new BleService(event_queue, *storage);

  // --- 13. Construct services ---
  auto *sensor_producer = new SensorProducer(*sensor_manager, event_queue,
                                             {
                                                 .task_stack_size = 4096,
                                                 .task_priority = 5,
                                             });

  auto *gps_service =
      new GpsService(*nmea_gps, event_queue,
                     {
                         .baud_rate = GPS_BAUD,
                         .posting_interval_ms = settings.gps_interval_seconds * 1000,
                         .task_stack_size = 4096,
                         .task_priority = 5,
                     });

  auto *input_service = new InputService(*touch, gpio::native::hal, event_queue,
                                         {
                                             .pin_cap_int = PIN_CAP_INT,
                                             .pin_button_power = PIN_BUTTON_POWER,
                                             .pin_button_boot = PIN_BUTTON_BOOT,
                                         });

  auto *display_service = new DisplayService({
      .spi_host = SPI_HOST,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });

  auto *power_service =
      new PowerService(*bms, gpio::native::hal,
                       {
                           .pin_wake_button_power = PIN_BUTTON_POWER,
                           .pin_wake_button_boot = -1, // GPIO28 is not RTC-capable
                           .pin_ext_wdt = PIN_EXT_WDT,
                       });

  power_service->init_ext_watchdog();
  power_service->reset_ext_watchdog();

  auto *ui_manager = new UIManager({
      .firmware_version = FIRMWARE_VERSION,
      .serial_number = serial_number,
  });

  // --- 14. Init display (shows initial empty dashboard) ---
  DisplayValues initial{};
  display_service->init(initial);

  // --- 15. Start producer tasks ---
  sensor_producer->start();
  gps_service->start();
  input_service->start();

  // --- 16. Construct and run orchestrator ---
  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = *display_service,
      .storage_service = *storage,
      .power_service = *power_service,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
  };

  auto *orchestrator =
      new Orchestrator(event_queue, services, settings, *config_store, serial_number);
  orchestrator->init(cause);
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// Helper functions
// ===========================================================================

// ---------------------------------------------------------------------------
// init_nvs
// ---------------------------------------------------------------------------

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// ---------------------------------------------------------------------------
// init_i2c_bus
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// init_gpio
//
// Configures power-enable pins and sets initial levels.  Exact pins depend
// on the AGo schematic.  Follows the reference product pattern:
// configure as output, set drive capability, set initial levels.
// ---------------------------------------------------------------------------

static void init_gpio() {
  auto &hal = gpio::native::hal;

  // PM sensor (SPS30) power enable — active-high
  hal.configure(PIN_PM_POWER, gpio::Mode::Output, gpio::PullMode::Floating,
                gpio::InterruptType::Disabled);
  gpio_set_drive_capability(PIN_PM_POWER, GPIO_DRIVE_CAP_3);
  hal.set_level(PIN_PM_POWER, 1);
}

// ---------------------------------------------------------------------------
// init_spi_buses
// ---------------------------------------------------------------------------

static void init_spi_buses() {
  // Single SPI bus shared by display and NAND flash.
  // Each driver adds its own device with a separate CS pin.
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

// ---------------------------------------------------------------------------
// init_bms
//
// Constructs a BQ25629Bms instance with the AGo charger configuration.
// The caller is responsible for calling init() on the returned object.
// ---------------------------------------------------------------------------

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
      .enable_otg = true,
      .enable_bypass_otg = true,
      .enable_adc = true,
  };
  return new BQ25629Bms(i2c_bus, config, I2C_ADDR_BMS);
}

// ---------------------------------------------------------------------------
// measures_to_ago
//
// Extract AGo-relevant fields from the full Measures struct returned by
// SensorManager::start_measures().
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// delay_with_watchdogs
//
// Keep the BMS and external watchdogs alive during long PM-stress waits.
// ---------------------------------------------------------------------------

static void delay_with_watchdogs(PowerService &power_service, uint32_t delay_ms) {
  uint32_t remaining_ms = delay_ms;
  while (remaining_ms > 0) {
    const uint32_t chunk_ms = std::min(remaining_ms, PM_STRESS_WATCHDOG_SERVICE_INTERVAL_MS);
    RTOS::delay_ms(chunk_ms);
    power_service.reset_watchdog();
    power_service.reset_ext_watchdog();
    remaining_ms -= chunk_ms;
  }
}

// ---------------------------------------------------------------------------
// build_pm_stress_display
//
// Build the Home screen values for the PM stress loop.
// PM is read via explicit PMID gating; the other fields come from the
// non-PM SensorManager cycle and are rendered when valid.
// ---------------------------------------------------------------------------

static DisplayValues build_pm_stress_display(const PMData *pm, const Measures *other,
                                             const PowerSnapshot &bms, const GoSettings &settings) {
  DisplayValues v{};

  v.screen = Screen::Home;
  v.locked = true;
  v.pm_use_usaqi = settings.pm_use_usaqi;
  v.use_fahrenheit = settings.use_fahrenheit;
  v.display_off = false;

  if (pm != nullptr && pm->is_pm_25_valid()) {
    v.pm25_ugm3 = pm->pm_25;
  }

  if (other != nullptr) {
    if (other->co2.is_valid()) {
      v.co2_ppm = other->co2.co2;
    }
    if (other->temp_hum_a.is_temp_valid()) {
      v.temperature_c = other->temp_hum_a.temperature;
    }
    if (other->temp_hum_a.is_hum_valid()) {
      v.humidity_pct = other->temp_hum_a.humidity;
    }
    if (other->tvoc_nox.is_tvoc_index_valid()) {
      v.tvoc_index = other->tvoc_nox.tvoc_index;
    }
    if (other->tvoc_nox.is_nox_index_valid()) {
      v.nox_index = other->tvoc_nox.nox_index;
    }
    if (other->pressure.is_pressure_valid()) {
      v.pressure_hpa = other->pressure.pressure;
    }
  }

  if (bms.battery_percentage >= 0.0f) {
    v.battery_pct = static_cast<uint8_t>(bms.battery_percentage);
  }
  v.is_battery_charging = is_bms_charging(bms.charging_status);

  return v;
}

// ---------------------------------------------------------------------------
// build_fast_path_display
//
// Build a minimal DisplayValues for the locked dashboard without a
// UIManager.  No chart data, no menu state, no snackbar.
// ---------------------------------------------------------------------------

static DisplayValues build_fast_path_display(const Measures &measures, const GpsData &gps,
                                             const PowerSnapshot &bms, const GoSettings &settings) {
  DisplayValues v{};

  // Sensor readings
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

  // GPS clock
  if (is_gps_timestamp_valid(gps.timestamp)) {
    v.hour = static_cast<uint8_t>(gps.timestamp.hour);
    v.minute = static_cast<uint8_t>(gps.timestamp.minute);
  }

  // GPS status
  v.gps_fix = is_fix_valid(gps.fix);

  // Battery
  if (bms.battery_percentage >= 0.0f) {
    v.battery_pct = static_cast<uint8_t>(bms.battery_percentage);
  }
  v.is_battery_charging = is_bms_charging(bms.charging_status);

  // State
  v.locked = true;
  v.screen = Screen::Home;

  // Settings-derived
  v.use_fahrenheit = settings.use_fahrenheit;
  v.pm_use_usaqi = settings.pm_use_usaqi;
  v.display_off = (settings.display_refresh_interval_seconds == 0);

  return v;
}

// ---------------------------------------------------------------------------
// build_wake_values
//
// Build the DisplayValues for the button-wake early paint from the RTC
// snapshot saved before the last deep sleep.
//
// Always shows Home + Unlocked + "Unlocked" snackbar regardless of the
// snapshot content.  If the snapshot is invalid (first power-on), sensor
// fields remain at their default invalid sentinels and the renderer shows
// dashes.
// ---------------------------------------------------------------------------

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
    v.hour = snapshot.hour;
    v.minute = snapshot.minute;
    v.battery_pct = snapshot.battery_pct;
    v.is_battery_charging = snapshot.is_battery_charging;
    v.gps_enabled = snapshot.gps_enabled;
    v.gps_fix = snapshot.gps_fix;
    v.tracking_active = snapshot.tracking_active;
    v.ble_enabled = snapshot.ble_enabled;
    v.use_fahrenheit = snapshot.use_fahrenheit;
    v.pm_use_usaqi = snapshot.pm_use_usaqi;
  }
  // else: all sensor/status fields stay at DisplayValues default invalid sentinels

  // Wake always shows Home, unlocked, not display-off
  v.screen = Screen::Home;
  v.locked = false;
  v.display_off = false;
  v.snackbar_text = "Unlocked";

  return v;
}
