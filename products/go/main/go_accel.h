/**
 * AirGradient Go — LIS2DH12 3-axis accelerometer driver
 *
 * Minimal driver for the ST LIS2DH12TR (U11 on v0.3) over I²C.  Provides
 * `WHO_AM_I` check, init (100 Hz, normal mode, ±2 g, high-resolution),
 * and raw 3-axis read in milli-g.
 *
 * INT1 (LIS2DH12 pin 12) is wired to ESP32-C5 on the v0.3 board (net
 * ACC_INT) but interrupt-driven motion detection is not part of this
 * initial driver — the chip is configured in continuous read mode for
 * basic bring-up testing.
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
using AccelI2cBusHandle = i2c_master_bus_handle_t;
using AccelI2cDevHandle = i2c_master_dev_handle_t;
#else
using AccelI2cBusHandle = void *;
using AccelI2cDevHandle = void *;
#endif

#include <cstdint>

class LIS2DH12 {
public:
  struct Config {
    uint8_t address = 0x18;          ///< 7-bit address (SA0 = GND on v0.3)
    uint32_t scl_speed_hz = 400000;  ///< 400 kHz fast-mode
    int timeout_ms = 50;
  };

  struct Reading {
    int16_t x_mg = 0;
    int16_t y_mg = 0;
    int16_t z_mg = 0;
  };

  /// Expected response from the WHO_AM_I register (datasheet §8.7).
  static constexpr uint8_t WHO_AM_I_EXPECTED = 0x33;

  LIS2DH12(AccelI2cBusHandle bus, const Config &config);
  ~LIS2DH12();

  LIS2DH12(const LIS2DH12 &) = delete;
  LIS2DH12 &operator=(const LIS2DH12 &) = delete;

  /// Probe + configure for 100 Hz continuous reads at ±2 g, high-resolution
  /// mode (12-bit, 1 mg/LSB). Verifies WHO_AM_I matches 0x33. Returns false
  /// on any I²C error or wrong WHO_AM_I.
  bool init();

  /// Read the WHO_AM_I register (0x0F). Returns the raw byte or 0 on error.
  uint8_t who_am_i();

  /// Read X/Y/Z and convert to milli-g. Returns false on I²C error.
  /// Values are signed; gravity registers as ~+1000 mg on the axis pointing up.
  bool read(Reading &out);

private:
  // Register addresses (LIS2DH12 datasheet §6)
  static constexpr uint8_t REG_WHO_AM_I = 0x0F;
  static constexpr uint8_t REG_CTRL_REG1 = 0x20;
  static constexpr uint8_t REG_CTRL_REG4 = 0x23;
  static constexpr uint8_t REG_OUT_X_L = 0x28;
  // Auto-increment flag (bit 7) set when reading a register block
  static constexpr uint8_t AUTO_INCR = 0x80;

  // CTRL_REG1 = 0x57 → ODR=0101 (100 Hz), LPen=0 (normal), Z/Y/X enabled
  static constexpr uint8_t CTRL_REG1_CONFIG = 0x57;
  // CTRL_REG4 = 0x88 → BDU=1 (block update), FS=00 (±2 g), HR=1 (high-res)
  static constexpr uint8_t CTRL_REG4_CONFIG = 0x88;

  Config _config;
  AccelI2cBusHandle _bus = nullptr;
  AccelI2cDevHandle _dev = nullptr;

  bool _write_reg(uint8_t reg, uint8_t value);
  bool _read_reg(uint8_t reg, uint8_t &out);
  bool _read_block(uint8_t reg, uint8_t *buf, size_t len);
};
