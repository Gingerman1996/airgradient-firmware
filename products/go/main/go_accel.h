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

  /// Configure motion detection on Interrupt 1 (INT1 pin) — high-pass-
  /// filtered "any axis high event" with the given threshold. Latches the
  /// source register so the host can identify which axis tripped; the latch
  /// clears when read_int1_src() is called. INT1 line idles low and asserts
  /// high on a qualifying event. Returns false on I²C error.
  /// 1 LSB of threshold_mg = 16 mg @ ±2 g FS; max ~2000 mg.
  bool enable_motion_int1(uint16_t threshold_mg);

  /// Read and clear the INT1_SRC latch. Bit 6 = IA (interrupt active),
  /// bits 5..0 = ZH ZL YH YL XH XL (axis-specific high/low triggers).
  /// Returns 0 on I²C error.
  uint8_t read_int1_src();

  /// Spawn an interrupt-driven motion-log task. Configures the chip's
  /// INT1 motion detector at the given threshold, sets up an ISR on
  /// @p int_pin (rising edge), and runs a task that wakes on motion and
  /// emits `MOTION src=0xNN x=±N y=±N z=±N mg`. Self-contained — process
  /// lifetime, matching the driver instance. Default threshold 250 mg
  /// (easy to trigger by hand, ignores desk vibration).
  void start_motion_log_task(int int_pin, uint16_t threshold_mg = 250);

  /// ISR-set flag — public so the volatile-bool GPIO ISR can write to it
  /// without a friend declaration. Polled and cleared by the motion task.
  volatile bool motion_flag = false;

private:
  // Register addresses (LIS2DH12 datasheet §6)
  static constexpr uint8_t REG_WHO_AM_I = 0x0F;
  static constexpr uint8_t REG_CTRL_REG1 = 0x20;
  static constexpr uint8_t REG_CTRL_REG2 = 0x21;
  static constexpr uint8_t REG_CTRL_REG3 = 0x22;
  static constexpr uint8_t REG_CTRL_REG4 = 0x23;
  static constexpr uint8_t REG_CTRL_REG5 = 0x24;
  static constexpr uint8_t REG_OUT_X_L = 0x28;
  static constexpr uint8_t REG_INT1_CFG = 0x30;
  static constexpr uint8_t REG_INT1_SRC = 0x31;
  static constexpr uint8_t REG_INT1_THS = 0x32;
  static constexpr uint8_t REG_INT1_DURATION = 0x33;
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
