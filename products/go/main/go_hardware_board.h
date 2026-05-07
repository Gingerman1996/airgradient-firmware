#pragma once

#include "go_board.h"
#include "go_settings.h"

#include <driver/i2c_master.h>

class BQ25629Bms;
class NvsConfigStore;

/// Real hardware implementation of GoBoard for the AGo board.
///
/// Wraps all ESP-IDF init calls, driver creation, and bus management.
/// Objects are heap-allocated and never freed (process lifetime).
class GoHardwareBoard : public GoBoard {
public:
  // --- Init methods ---
  void init_nvs() override;
  void init_buses() override;
  void init_spi() override;
  void init_bms() override;
  void init_core() override;

  // --- Lazy service accessors ---
  ConfigStore &config_store() override;
  GoSettings load_settings() override;
  BmsDevice &bms() override;
  SensorManager &sensors(bool warm) override;
  StorageService &storage() override;
  DisplayService &display() override;
  PowerService &power() override;

  // --- Per-call factories ---
  GpsDriver *new_gps_driver() override;
  CapTouchSensor *new_touch_sensor() override;
  LP5036 *new_led_driver() override;

  // --- Platform ---
  std::string serial_number() override;
  const char *firmware_version() override;
  const gpio::Hal &gpio_hal() override;
  void release_gpio_holds() override;
  void ulp_stop() override;
  void ulp_start() override;
  void install_button_isr(int pin, volatile bool *flag) override;
  void remove_button_isr(int pin) override;

private:
  // Init tracking (idempotency)
  bool _nvs_ready = false;
  bool _buses_ready = false;
  bool _spi_ready = false;
  bool _bms_ready = false;

  // Bus handles
  i2c_master_bus_handle_t _i2c_bus = nullptr;

  // Owned objects (heap-allocated, never freed)
  NvsConfigStore *_config_store = nullptr;
  GoSettings _settings{};
  bool _settings_loaded = false;
  BQ25629Bms *_bms_driver = nullptr;
  SensorManager *_sensor_manager = nullptr;
  StorageService *_storage = nullptr;
  DisplayService *_display = nullptr;
  PowerService *_power = nullptr;
};
