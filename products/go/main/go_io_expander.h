/**
 * AirGradient Go — TCA9536 I²C I/O expander driver
 *
 * Minimal driver for the TI TCA9536DTMR 4-channel open-drain I²C GPIO
 * expander (U18 on v0.3).  Only the channels actually wired on the board
 * are exercised; today that's P0 → TAU1113 PRTRG (GPS wake trigger).
 *
 * Address is fixed at 0x41 in silicon (no addr-strap pins).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "rtos.h"

#ifndef TEST_HOST
#include <driver/i2c_master.h>
using IoExpI2cBusHandle = i2c_master_bus_handle_t;
using IoExpI2cDevHandle = i2c_master_dev_handle_t;
#else
using IoExpI2cBusHandle = void *;
using IoExpI2cDevHandle = void *;
#endif

#include <cstdint>

class TCA9536 {
public:
  struct Config {
    uint8_t address = 0x41;         ///< Fixed I²C address per datasheet
    uint32_t scl_speed_hz = 400000; ///< 400 kHz fast-mode
    int timeout_ms = 50;
  };

  /// Channel identifiers — match the chip's bit layout (P0 = bit 0 …).
  enum class Pin : uint8_t { P0 = 0, P1 = 1, P2 = 2, P3 = 3 };

  TCA9536(IoExpI2cBusHandle bus, const Config &config);
  ~TCA9536();

  TCA9536(const TCA9536 &) = delete;
  TCA9536 &operator=(const TCA9536 &) = delete;

  /// Probe device and apply sane defaults: all pins as input (high-Z),
  /// output register left at 0xFF (all-high) so that switching any pin
  /// to output drives it high — safe for PRTRG which must NOT be low at
  /// boot or the TAU1113 enters BootROM mode.
  bool init();

  /// Configure a pin as push-pull output.  Returns false on I²C error.
  bool set_output(Pin pin);

  /// Configure a pin as input (high-Z).
  bool set_input(Pin pin);

  /// Set the output level of a pin (must be configured as output first).
  bool write(Pin pin, bool high);

  /// Drive a pin low for hold_ms, then return it high. Pin must already
  /// be configured as output. Used as the TAU1113 PRTRG wake trigger
  /// (CFG-SLEEP can be cut short by pulling PRTRG low for ≥10 ms).
  /// Returns false on any I²C error; partial completion possible.
  bool pulse_low(Pin pin, uint32_t hold_ms);

private:
  // Register map (TCA9536 datasheet, Table 4-1)
  static constexpr uint8_t REG_INPUT = 0x00;
  static constexpr uint8_t REG_OUTPUT = 0x01;
  static constexpr uint8_t REG_POLARITY = 0x02;
  static constexpr uint8_t REG_CONFIG = 0x03;

  Config _config;
  IoExpI2cBusHandle _bus = nullptr;
  IoExpI2cDevHandle _dev = nullptr;

  // Local caches of the OUTPUT (0x01) and CONFIG (0x03) registers.
  // Initialised to the chip's documented power-on defaults.
  uint8_t _out_cache = 0xFF; // power-on: all outputs latched high
  uint8_t _cfg_cache = 0xFF; // power-on: all pins as input

  static uint8_t bit_of(Pin pin) { return 1u << static_cast<uint8_t>(pin); }

  bool _write_reg(uint8_t reg, uint8_t value);
};
