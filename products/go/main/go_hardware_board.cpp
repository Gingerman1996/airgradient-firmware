/**
 * AirGradient Go — GoHardwareBoard Implementation
 *
 * Real ESP-IDF hardware implementation of the GoBoard interface.
 * Contains all hardware init calls, driver creation, and bus management
 * moved from main.cpp.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_hardware_board.h"

#include <cassert>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_app_desc.h>
#include <nvs_flash.h>

#include "ag_log.h"
#include "airgradient_uart.h"
#include "backends/rtc_payload_cache_storage.h"
#include "cap1203.h"
#include "common.h"
#include "drivers/bq25629/bq25629_bms.h"
#include "drivers/bq27427/bq27427.h"
#include "drivers/dps368/dps368.h"
#include "drivers/s12/s12.h"
#include "drivers/scd4x/scd4x.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sht40/sht40.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
#include "go_io_expander.h"
#include "go_led.h"
#include "gps/gps_driver.h"
#include "native_gpio.h"
#include "nvs_config_store.h"
#include "rtos.h"
#include "services/payload_cache.h"
#include "services/sensor_manager.h"
#include "spi_nand_storage.h"

#include "board_config.h"
#include "go_display.h"
#include "go_power.h"
#include "go_storage.h"
#include "go_ulp.h"

static constexpr const char *TAG = "board";

// ===========================================================================
// Private helper: CO2 sensor detection
// ===========================================================================

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

// ===========================================================================
// Init methods (idempotent)
// ===========================================================================

void GoHardwareBoard::init_nvs() {
  if (_nvs_ready)
    return;
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  _nvs_ready = true;
}

void GoHardwareBoard::init_buses() {
  if (_buses_ready)
    return;

  // GPIO power enables
  auto &hal = gpio::native::hal;
  hal.configure(PIN_PM_POWER, gpio::Mode::Output, gpio::PullMode::Floating,
                gpio::InterruptType::Disabled);
  gpio_set_drive_capability(PIN_PM_POWER, GPIO_DRIVE_CAP_3);
  hal.set_level(PIN_PM_POWER, 0);

  RTOS::delay_ms(100);

  // I2C bus
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
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &_i2c_bus));
  AG_LOGI(TAG, "I2C bus ready");

  RTOS::delay_ms(100);
  _buses_ready = true;
}

void GoHardwareBoard::init_spi() {
  if (_spi_ready)
    return;

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = PIN_SPI_MISO;
  bus.sclk_io_num = PIN_SPI_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO));

  AG_LOGI(TAG, "SPI bus ready");
  _spi_ready = true;
}

void GoHardwareBoard::init_bms() {
  if (_bms_ready)
    return;

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
  _bms_driver = new BQ25629Bms(_i2c_bus, config, I2C_ADDR_BMS);
  if (!_bms_driver->init()) {
    AG_LOGE(TAG, "BMS init failed");
  }

  _fuel_gauge = new BQ27427(_i2c_bus, {.address = I2C_ADDR_FUEL_GAUGE});
  if (_fuel_gauge->init()) {
    // Recovery path: detect a corrupted fuel-gauge state from a prior aborted
    // CFGUPDATE write and reset RAM back to ROM defaults.  Two indicators:
    //   1. Design Capacity out of the sane 500..8000 mAh range (e.g. the
    //      partially-written 208 mAh = 0x00D0 leftover from a previous run).
    //   2. FullChargeCapacity out of range (the chip's Qmax is independent
    //      of Design Capacity and can be corrupted on its own — e.g. 32507).
    uint16_t dc = 0;
    uint16_t fcc = 0;
    const bool dc_ok = _fuel_gauge->read_design_capacity_mah(dc);
    const bool fcc_ok = _fuel_gauge->read_full_charge_capacity_mah(fcc);
    if (dc_ok) {
      AG_LOGI(TAG, "BQ27427: current Design Capacity = %umAh", dc);
    }
    if (fcc_ok) {
      AG_LOGI(TAG, "BQ27427: current FullChargeCapacity = %umAh", fcc);
    }
    const bool dc_bad = dc_ok && (dc < 500 || dc > 8000);
    const bool fcc_bad = fcc_ok && fcc > 8500;
    if (dc_bad || fcc_bad) {
      AG_LOGW(TAG, "BQ27427: corrupted state (dc_bad=%d fcc_bad=%d) — resetting",
              dc_bad, fcc_bad);
      if (!_fuel_gauge->reset_to_factory_defaults()) {
        AG_LOGW(TAG, "BQ27427: factory reset failed");
      }
    }

    // Configure the BQ27427 for the AGo cell.  All four fields written
    // atomically in a single CFGUPDATE session.  Idempotent — if every
    // field already matches, no CFGUPDATE is entered (preserves any
    // learned Qmax / impedance state across reboots).
    //
    //   DC=2000 mAh     — AGo's 2000 mAh single-cell Li-ion
    //   DE=7400 mWh     — 2000 mAh × 3.7 V nominal
    //   TermV=3000 mV   — conservative cell-empty cutoff; verify against
    //                     cell datasheet (some Li-ion cells spec 2.75 V)
    //   SleepI=50 mA    — up from default 10 mA so AGo's normal idle
    //                     current qualifies for Qmax-learning rest periods
    const BQ27427::CellConfig cell = {
        .design_capacity_mah = 2000,
        .design_energy_mwh = 7400,
        .terminate_voltage_mv = 3000,
        .sleep_current_ma = 50,
    };
    if (!_fuel_gauge->configure_cell(cell)) {
      AG_LOGW(TAG, "BQ27427: failed to apply cell configuration");
    }
    uint8_t soc = 0;
    uint16_t mv = 0;
    int16_t ma = 0;
    float tc = 0.0f;
    const bool ok_soc = _fuel_gauge->read_soc_percent(soc);
    const bool ok_v = _fuel_gauge->read_voltage_mv(mv);
    const bool ok_i = _fuel_gauge->read_average_current_ma(ma);
    const bool ok_t = _fuel_gauge->read_internal_temperature_c(tc);
    AG_LOGI(TAG, "BQ27427 boot: soc=%s%u%% v=%s%umV i=%s%dmA t=%s%.1fC",
            ok_soc ? "" : "?", soc, ok_v ? "" : "?", mv,
            ok_i ? "" : "?", ma, ok_t ? "" : "?", tc);
  } else {
    AG_LOGW(TAG, "BQ27427 fuel gauge init failed");
  }

  _bms_ready = true;
}

void GoHardwareBoard::init_core() {
  init_nvs();
  init_buses();
  init_spi();
  init_bms();
}

// ===========================================================================
// Lazy service accessors
// ===========================================================================

ConfigStore &GoHardwareBoard::config_store() {
  assert(_nvs_ready && "config_store() requires init_nvs()");
  if (!_config_store) {
    _config_store = new NvsConfigStore("go");
  }
  return *_config_store;
}

GoSettings GoHardwareBoard::load_settings() {
  if (!_settings_loaded) {
    _settings = load_go_settings(config_store());
    print_settings(_settings);
    _settings_loaded = true;
  }
  return _settings;
}

BmsDevice &GoHardwareBoard::bms() {
  assert(_bms_ready && "bms() requires init_bms()");
  return *_bms_driver;
}

SensorManager &GoHardwareBoard::sensors(bool warm) {
  assert(_buses_ready && "sensors() requires init_buses()");
  assert(_bms_ready && "sensors() requires init_bms()");
  if (!_sensor_manager) {
    auto *sgp41 = new SGP41(_i2c_bus, I2C_ADDR_SGP41);
    auto *sht40 = new SHT40(_i2c_bus, I2C_ADDR_SHT40);
    auto *sps30 = new SPS30(_i2c_bus);
    auto *dps368 = new DPS368(_i2c_bus, I2C_ADDR_DPS368);

    auto *s = new Sensors{};

    // Init DPS368 first: continuous mode starts immediately, giving the
    // pressure sensor time to produce its first measurement (~120 ms)
    if (dps368->init()) {
      s->pressure = dps368;
    } else {
      AG_LOGE(TAG, "DPS368 init failed");
    }

    s->co2 = init_co2_sensor(_i2c_bus);

    if (sht40->init()) {
      s->temp_hum = sht40;
    } else {
      AG_LOGE(TAG, "SHT40 init failed");
    }
    if (sgp41->init()) {
      s->tvoc_nox = sgp41;
    } else {
      AG_LOGE(TAG, "SGP41 init failed");
    }
    if (sps30->init(warm)) {
      s->pms_a = sps30;
    } else {
      AG_LOGE(TAG, "SPS30 init failed");
    }

    s->temp_hum_a_fallback.priority[0] = TempHumSource::DEDICATED;
    s->temp_hum_a_fallback.priority[1] = TempHumSource::CO2;
    s->temp_hum_a_fallback.priority[2] = TempHumSource::PRESSURE;
    s->temp_hum_a_fallback.count = 3;

    _sensor_manager = new SensorManager(*s);
  }
  return *_sensor_manager;
}

StorageService &GoHardwareBoard::storage() {
  assert(_spi_ready && "storage() requires init_spi()");
  if (!_storage) {
    auto *rtc_storage = new RtcPayloadCacheStorage();
    auto *cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

    SpiNandStorage::Config nand_config{};
    nand_config.spi_host = SPI_HOST;
    nand_config.cs_pin = PIN_NAND_CS;
    auto *nand = new SpiNandStorage(nand_config);

    _storage = new StorageService(*cache, *nand);
    _storage->restore_cache();
    if (!_storage->init()) {
      AG_LOGE(TAG, "NAND storage init failed");
    }
  }
  return *_storage;
}

DisplayService &GoHardwareBoard::display() {
  assert(_spi_ready && "display() requires init_spi()");
  if (!_display) {
    _display = new DisplayService({
        .spi_host = SPI_HOST,
        .pin_cs = PIN_DISPLAY_CS,
        .pin_dc = PIN_DISPLAY_DC,
        .pin_rst = PIN_DISPLAY_RST,
        .pin_busy = PIN_DISPLAY_BUSY,
    });
  }
  return *_display;
}

PowerService &GoHardwareBoard::power() {
  assert(_bms_ready && "power() requires init_bms()");
  if (!_power) {
    _power = new PowerService(*_bms_driver, gpio::native::hal,
                              {
                                  .pin_wake_button_power = PIN_BUTTON_POWER,
                                  .pin_wake_button_boot = -1,
                                  .pin_ext_wdt = PIN_EXT_WDT,
                                  .deep_sleep_threshold_ms = 5000,
                                  .pin_pm_power = PIN_PM_POWER,
                                  .sensor_hold_max_sleep_ms = 20000,
                              });
    if (_fuel_gauge != nullptr && _fuel_gauge->ready()) {
      _power->set_fuel_gauge(_fuel_gauge);
    }
    _power->init_ext_watchdog();
    _power->reset_ext_watchdog();
  }
  return *_power;
}

// ===========================================================================
// Per-call factories
// ===========================================================================

GpsDriver *GoHardwareBoard::new_gps_driver() {
  assert(_buses_ready && "new_gps_driver() requires init_buses()");
  auto *serial = new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  auto *driver = new GpsDriver(*serial);

  // Wire the TAU1113 PRTRG wake line: U18 channel P0 must be driven LOW for
  // ≥10 ms to wake the module out of CFG-SLEEP. Configure P0 as output and
  // park it HIGH so the GPS sees User Normal Mode at boot (low at power-up
  // would enter BootROM Command Mode — per TAU1113 datasheet §4.3).
  TCA9536::Config tca_cfg;
  tca_cfg.address = I2C_ADDR_TCA9536;
  auto *expander = new TCA9536(_i2c_bus, tca_cfg);
  if (expander->init() && expander->write(TCA9536::Pin::P0, true) &&
      expander->set_output(TCA9536::Pin::P0)) {
    driver->set_wake_handler(
        [](void *ctx) {
          static_cast<TCA9536 *>(ctx)->pulse_low(TCA9536::Pin::P0, 20);
        },
        expander);
    AG_LOGI(TAG, "GPS wake line ready (TCA9536 P0 -> PRTRG)");
  } else {
    AG_LOGW(TAG, "TCA9536 init failed — GPS host-wake unavailable, CFG-SLEEP "
                 "can only end on its own timer");
  }
  return driver;
}

CapTouchSensor *GoHardwareBoard::new_touch_sensor() {
  assert(_buses_ready && "new_touch_sensor() requires init_buses()");
  CAP1203::Config cfg;
  cfg.delta_sense = TOUCH_DELTA_SENSE;
  auto *touch = new CAP1203(_i2c_bus, I2C_ADDR_CAP1203, cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }
  return touch;
}

LP5036 *GoHardwareBoard::new_led_driver() {
  assert(_buses_ready && "new_led_driver() requires init_buses()");
  LP5036::Config cfg;
  cfg.address = I2C_ADDR_LP5036;
  auto *led = new LP5036(_i2c_bus, cfg);
  if (!led->init()) {
    AG_LOGE(TAG, "LP5036 LED driver init failed");
  }
  // LED25 (OUT30) and LED26 (OUT31) brightness applied by orchestrator after
  // settings load — see Orchestrator::apply_led_brightness().
  return led;
}

// ===========================================================================
// Platform info
// ===========================================================================

std::string GoHardwareBoard::serial_number() { return build_serial_number(); }

const char *GoHardwareBoard::firmware_version() { return esp_app_get_description()->version; }

const gpio::Hal &GoHardwareBoard::gpio_hal() { return gpio::native::hal; }

// ===========================================================================
// Hardware operations
// ===========================================================================

void GoHardwareBoard::release_gpio_holds() { PowerService::release_sleep_gpio_holds(PIN_PM_POWER); }

void GoHardwareBoard::ulp_stop() { ulp_wdt_stop(); }

void GoHardwareBoard::ulp_start() { ulp_wdt_start(); }

void GoHardwareBoard::install_button_isr(int pin, volatile bool *flag) {
  gpio_install_isr_service(0); // idempotent
  gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_NEGEDGE);
  gpio_isr_handler_add(
      static_cast<gpio_num_t>(pin), [](void *arg) { *static_cast<volatile bool *>(arg) = true; },
      const_cast<bool *>(flag));
}

void GoHardwareBoard::remove_button_isr(int pin) {
  gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
  gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_DISABLE);
}
