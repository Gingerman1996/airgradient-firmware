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
static constexpr int HEADLESS_POWER_TEST_SLEEP_INTERVAL_SECONDS = 300;

static bool has_primary_air_sensors(const Sensors &sensors) {
  return sensors.temp_hum != nullptr || sensors.co2 != nullptr || sensors.pms_a != nullptr ||
         sensors.pms_b != nullptr || sensors.tvoc_nox != nullptr || sensors.o3_no2 != nullptr;
}

static bool has_any_measurement_sensors(const Sensors &sensors) {
  return has_primary_air_sensors(sensors) || sensors.pressure != nullptr;
}

static void apply_headless_power_test_profile(GoSettings &settings) {
  settings.pm_interval_seconds = HEADLESS_POWER_TEST_SLEEP_INTERVAL_SECONDS;
  settings.other_sensor_interval_seconds = HEADLESS_POWER_TEST_SLEEP_INTERVAL_SECONDS;
  settings.display_refresh_interval_seconds = HEADLESS_POWER_TEST_SLEEP_INTERVAL_SECONDS;
}

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void run_fast_path(const RtcAppState &state);
static void run_full_boot(WakeCause cause, const char *serial_number);

static void init_nvs();
static i2c_master_bus_handle_t init_i2c_bus();
static void init_gpio();
static void init_spi_buses();
static BQ25629Bms *init_bms(i2c_master_bus_handle_t i2c_bus);
static MeasuresAGo measures_to_ago(const Measures &m);
static DisplayValues build_fast_path_display(const Measures &measures, const GpsData &gps,
                                             const PowerSnapshot &bms, const GoSettings &settings);

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
      // Never returns.
    }
  }

  std::string serial_number = build_serial_number();
  AG_LOGI(TAG, "Serial number: %s", serial_number.c_str());
  run_full_boot(cause, serial_number.c_str());
  // Never returns.
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

  const bool headless_power_test_profile = !has_primary_air_sensors(sensors);
  if (headless_power_test_profile) {
    apply_headless_power_test_profile(settings);
    AG_LOGW(TAG, "fast-path headless power-test profile active: sleep interval forced to %d s",
            HEADLESS_POWER_TEST_SLEEP_INTERVAL_SECONDS);
  }

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
  const int wake_button_power_pin = headless_power_test_profile ? -1 : PIN_BUTTON_POWER;
  auto *power_service =
      new PowerService(*bms, gpio::native::hal,
                       {
                           .pin_wake_button_power = wake_button_power_pin,
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
  power_service->enter_sleep(decision.type, decision.duration_ms);
  // Never returns — CPU reboots on wake.
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
  const bool touch_available = touch->init();
  if (!touch_available) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  const bool has_measurement_sensors = has_any_measurement_sensors(sensors);
  const bool headless_power_test_profile =
      !has_primary_air_sensors(sensors) && !touch_available;
  if (headless_power_test_profile) {
    apply_headless_power_test_profile(settings);
    AG_LOGW(TAG,
            "Headless power-test profile active: forcing Offline mode, skipping initial measurement, disabling GPIO wake, and forcing %d s sleep interval",
            HEADLESS_POWER_TEST_SLEEP_INTERVAL_SECONDS);
  }
  print_settings(settings);

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

  const int wake_button_power_pin = headless_power_test_profile ? -1 : PIN_BUTTON_POWER;
  auto *power_service =
      new PowerService(*bms, gpio::native::hal,
                       {
                           .pin_wake_button_power = wake_button_power_pin,
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
      new Orchestrator(event_queue, services, settings, *config_store, serial_number,
                       {
                           .require_initial_measurement =
                               has_measurement_sensors && !headless_power_test_profile,
                           .force_offline_mode = headless_power_test_profile,
                       });
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
      .enable_otg = false,
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
  return ago;
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
  v.is_battery_charging = (bms.charging_status != BmsChargingState::Unknown &&
                           bms.charging_status != BmsChargingState::NotCharging);

  // State
  v.locked = true;
  v.screen = Screen::Home;

  // Settings-derived
  v.use_fahrenheit = settings.use_fahrenheit;
  v.pm_use_usaqi = settings.pm_use_usaqi;
  v.display_off = (settings.display_refresh_interval_seconds == 0);

  return v;
}
