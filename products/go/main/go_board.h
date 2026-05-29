#pragma once

#include "airgradient_gpio.h"
#include "go_types.h"

#include <cstdint>
#include <string>

// Forward declarations — avoid pulling full headers into the interface.
class BmsDevice;
class CapTouchSensor;
class LP5036;
class ConfigStore;
class DisplayService;
class GpsDriver;
class PowerService;
class SensorManager;
class StorageService;
struct GoSettings;

/// Abstract factory / BSP interface for the AGo board.
///
/// Owns the lifecycle of every hardware-touching object.  GoApp calls
/// init methods in boot-path-specific order, then accesses services via
/// lazy accessors.
///
/// On target, GoHardwareBoard implements this with real ESP-IDF drivers.
/// In tests, MockBoard implements this with stubs and mocks.
struct GoBoard {
  virtual ~GoBoard() = default;

  // -----------------------------------------------------------------
  // Init methods (fine-grained, idempotent)
  //
  // Each initialises one subsystem.  Safe to call multiple times —
  // subsequent calls are no-ops.  Boot paths call these in the order
  // their hardware sequencing requires.
  // -----------------------------------------------------------------

  virtual void init_nvs() = 0;   ///< NVS flash
  virtual void init_buses() = 0; ///< GPIO power enables + I2C bus + settling
  virtual void init_spi() = 0;   ///< SPI bus
  virtual void init_bms() = 0;   ///< BMS driver (requires buses)

  // -----------------------------------------------------------------
  // Convenience gate — calls all init methods above (skips what's done)
  // -----------------------------------------------------------------

  virtual void init_core() = 0;

  // -----------------------------------------------------------------
  // Lazy service accessors
  //
  // Create-on-first-call.  Objects are owned by the board and live for
  // the duration of the process (never freed — the app never returns).
  // Callers must ensure prerequisite init methods have been called.
  // -----------------------------------------------------------------

  virtual ConfigStore &config_store() = 0;
  virtual GoSettings load_settings() = 0;
  virtual BmsDevice &bms() = 0;
  virtual SensorManager &sensors(bool warm = false) = 0;
  virtual StorageService &storage() = 0;
  virtual DisplayService &display() = 0;
  virtual PowerService &power() = 0;

  // -----------------------------------------------------------------
  // Per-call factories (caller owns the returned object)
  // -----------------------------------------------------------------

  virtual GpsDriver *new_gps_driver() = 0;
  virtual CapTouchSensor *new_touch_sensor() = 0;
  virtual LP5036 *new_led_driver() = 0;

  // -----------------------------------------------------------------
  // Platform info
  // -----------------------------------------------------------------

  virtual std::string serial_number() = 0;
  virtual const char *firmware_version() = 0;
  virtual const gpio::Hal &gpio_hal() = 0;

  // -----------------------------------------------------------------
  // Hardware operations
  // -----------------------------------------------------------------

  virtual void release_gpio_holds() = 0;
  virtual void ulp_stop() = 0;
  virtual void ulp_start() = 0;

  // -----------------------------------------------------------------
  // Button ISR management
  //
  // The fast path uses a GPIO ISR to detect button presses during
  // blocking operations.  The board owns ISR setup/teardown;
  // GoApp reads the volatile flag.
  // -----------------------------------------------------------------

  /// Install a falling-edge ISR on the given pin.  When triggered,
  /// the ISR sets *flag to true.
  virtual void install_button_isr(int pin, volatile bool *flag) = 0;

  /// Remove the ISR and disable the interrupt on the given pin.
  virtual void remove_button_isr(int pin) = 0;
};
