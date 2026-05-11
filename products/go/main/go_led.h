/**
 * AirGradient Go — LED service
 *
 * Drives the LP5036 36-channel I²C constant-current LED controller on the
 * mobile_display sub-PCB.  Used today for white touch-feedback flashes on
 * LED1 (select), LED2 (left) and LED10 (right).
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
using LedI2cBusHandle = i2c_master_bus_handle_t;
using LedI2cDevHandle = i2c_master_dev_handle_t;
#else
using LedI2cBusHandle = void *;
using LedI2cDevHandle = void *;
#endif

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// LP5036 driver — minimal: per-channel 8-bit PWM via I²C.
// ---------------------------------------------------------------------------

class LP5036 {
public:
  struct Config {
    uint8_t address = 0x33;          ///< 7-bit I²C address (ADDR0=ADDR1=1 → 0x33)
    uint32_t scl_speed_hz = 400000;  ///< 400 kHz fast-mode is the LP5036 default
    int timeout_ms = 100;
  };

  /// Total number of OUT channels (OUT0–OUT35).
  static constexpr uint8_t NUM_CHANNELS = 36;

  LP5036(LedI2cBusHandle bus, const Config &config);
  ~LP5036();

  LP5036(const LP5036 &) = delete;
  LP5036 &operator=(const LP5036 &) = delete;

  /// Probe + enable the chip and zero all 36 output channels.
  /// Returns false on I²C error.  Idempotent.
  bool init();

  /// Set the 8-bit PWM value for one OUT channel (0–35).
  /// Returns false on I²C error or invalid channel.
  bool set_channel(uint8_t channel, uint8_t value);

  /// Set R/G/B for a logical RGB LED in one transaction (auto-increment write).
  /// `b_channel` is the OUT channel for blue; the next two registers are
  /// written with `g` and `r` (matches v0.3 ordering: OUT[3n+0]=B, +1=G, +2=R).
  bool set_rgb(uint8_t b_channel, uint8_t r, uint8_t g, uint8_t b);

private:
  Config _config;
  LedI2cBusHandle _bus = nullptr;
  LedI2cDevHandle _dev = nullptr;

  bool _write_reg(uint8_t reg, uint8_t value);
  bool _write_block(uint8_t reg, const uint8_t *data, size_t len);
};

// ---------------------------------------------------------------------------
// LedService — async fire-and-forget RGB LED feedback.
// ---------------------------------------------------------------------------

class LedService {
public:
  /// Logical LED index used by the service.
  enum class Led : uint8_t {
    Select = 0, // LED1 — touch CH3
    Left = 1,   // LED2 — touch CH2
    Right = 2,  // LED10 — touch CH1
  };

  struct Config {
    LP5036 *driver = nullptr; ///< Required.  Owned externally.
    uint16_t task_stack_size = 2048;
    uint8_t task_priority = 3;
    uint8_t queue_depth = 8;
  };

  /// One step in a flash sequence (RGB at full set, then off after duration).
  struct Step {
    Led led;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint32_t duration_ms;
  };

  explicit LedService(const Config &config);
  ~LedService();

  LedService(const LedService &) = delete;
  LedService &operator=(const LedService &) = delete;

  /// Initialise underlying driver, create queue.
  bool init();

  /// Start the worker task.  Call after init().
  bool start();

  /// Flash the given LED at white, `value` PWM (0–255), for `duration_ms`,
  /// then turn it off.  Non-blocking — drops the request if queue is full.
  void flash_white(Led led, uint8_t value, uint32_t duration_ms);

  /// Turn an LED off immediately.
  void off(Led led);

  /// Turn all 36 channels off.
  void all_off();

  /// Set brightness of indicator LEDs LED25 (OUT30) and LED26 (OUT31).
  /// `pwm` is 0–255 (0 = off, 255 = full).  Bypasses the worker queue —
  /// safe because the worker only writes to channels 0–2, 3–5, 27–29.
  void set_indicator_brightness(uint8_t pwm);

  bool ready() const { return _config.driver != nullptr && _queue != nullptr; }

private:
  Config _config;
  RtosQueueHandle _queue = nullptr;
  RtosTaskHandle _task = nullptr;

  static void _task_entry(void *arg);
  void _run();

  /// Map Led → (b_channel, g_channel, r_channel) per v0.3 wiring.
  static void _map(Led led, uint8_t &b_ch, uint8_t &g_ch, uint8_t &r_ch);
  void _set_led_rgb(Led led, uint8_t r, uint8_t g, uint8_t b);
};
