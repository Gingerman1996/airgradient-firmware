/**
 * AirGradient Go — LED service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_led.h"

#include "ag_log.h"

#include <cstring>

namespace {
constexpr const char *TAG = "Led";

// LP5036 register map (only what we use)
constexpr uint8_t REG_DEVICE_CONFIG0 = 0x00;
constexpr uint8_t REG_DEVICE_CONFIG1 = 0x01;
constexpr uint8_t REG_OUT0_COLOR = 0x14; // OUTn = REG_OUT0_COLOR + n

constexpr uint8_t DEV_CONFIG0_CHIP_EN = 0x40;
// Bit 3 = AUTO_INCR_EN (data sheet calls it "Auto-Increment")
// Bit 5 = PWM_DITHERING_EN
constexpr uint8_t DEV_CONFIG1_DEFAULT = 0b00111000;
} // namespace

// ===========================================================================
// LP5036 driver
// ===========================================================================

LP5036::LP5036(LedI2cBusHandle bus, const Config &config) : _config(config), _bus(bus) {}

LP5036::~LP5036() {
#ifndef TEST_HOST
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
#endif
}

bool LP5036::init() {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = _config.address,
        .scl_speed_hz = _config.scl_speed_hz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK) {
      AG_LOGE(TAG, "LP5036: bus_add_device failed at addr 0x%02X", _config.address);
      return false;
    }
  }
#endif

  // Enable chip
  if (!_write_reg(REG_DEVICE_CONFIG0, DEV_CONFIG0_CHIP_EN)) {
    AG_LOGE(TAG, "LP5036: enable failed (no ACK at 0x%02X?)", _config.address);
    return false;
  }
  if (!_write_reg(REG_DEVICE_CONFIG1, DEV_CONFIG1_DEFAULT)) {
    return false;
  }
  // Zero all 36 output channels — leaves LEDs dark on boot
  uint8_t zeros[NUM_CHANNELS] = {};
  if (!_write_block(REG_OUT0_COLOR, zeros, NUM_CHANNELS)) {
    return false;
  }
  AG_LOGI(TAG, "LP5036: initialised at I²C 0x%02X", _config.address);
  return true;
}

bool LP5036::set_channel(uint8_t channel, uint8_t value) {
  if (channel >= NUM_CHANNELS) {
    return false;
  }
  return _write_reg(REG_OUT0_COLOR + channel, value);
}

bool LP5036::set_rgb(uint8_t b_channel, uint8_t r, uint8_t g, uint8_t b) {
  if (b_channel + 2 >= NUM_CHANNELS) {
    return false;
  }
  // Three discrete writes — avoids dependency on AUTO_INCR_EN bit being set
  // exactly the way we assumed in DEVICE_CONFIG1.  At 400 kHz each transaction
  // is ~250 µs so the whole LED update is well under 1 ms.
  // v0.3 mapping per OUT register order: B, G, R contiguous.
  return _write_reg(REG_OUT0_COLOR + b_channel, b) &&
         _write_reg(REG_OUT0_COLOR + b_channel + 1, g) &&
         _write_reg(REG_OUT0_COLOR + b_channel + 2, r);
}

bool LP5036::_write_reg(uint8_t reg, uint8_t value) {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  return i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)value;
  return true;
#endif
}

bool LP5036::_write_block(uint8_t reg, const uint8_t *data, size_t len) {
#ifndef TEST_HOST
  if (_dev == nullptr || len == 0 || len > 64) {
    return false;
  }
  uint8_t buf[1 + 64];
  buf[0] = reg;
  std::memcpy(buf + 1, data, len);
  return i2c_master_transmit(_dev, buf, len + 1, _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)data;
  (void)len;
  return true;
#endif
}

// ===========================================================================
// LedService
// ===========================================================================

namespace {
struct Cmd {
  enum class Kind : uint8_t { FlashOn, FlashOff, AllOff };
  Kind kind;
  LedService::Led led;
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint32_t duration_ms;
};
constexpr uint32_t QUEUE_TIMEOUT_MS = 0; // never block the producer
} // namespace

LedService::LedService(const Config &config) : _config(config) {}

LedService::~LedService() {
  if (_task != nullptr) {
    RTOS::task_delete(_task);
  }
  if (_queue != nullptr) {
    RTOS::queue_delete(_queue);
  }
}

bool LedService::init() {
  if (_config.driver == nullptr) {
    AG_LOGE(TAG, "init: no driver");
    return false;
  }
  if (_queue == nullptr) {
    _queue = RTOS::queue_create(_config.queue_depth, sizeof(Cmd));
    if (_queue == nullptr) {
      AG_LOGE(TAG, "init: queue_create failed");
      return false;
    }
  }
  return true;
}

bool LedService::start() {
  if (!ready()) {
    return false;
  }
  if (_task != nullptr) {
    return true;
  }
  return RTOS::task_create(&LedService::_task_entry, "led", _config.task_stack_size, this,
                           _config.task_priority, &_task);
}

void LedService::flash_white(Led led, uint8_t value, uint32_t duration_ms) {
  if (!ready()) {
    return;
  }
  Cmd c{Cmd::Kind::FlashOn, led, value, value, value, duration_ms};
  RTOS::queue_send(_queue, &c, QUEUE_TIMEOUT_MS);
}

void LedService::off(Led led) {
  if (!ready()) {
    return;
  }
  Cmd c{Cmd::Kind::FlashOff, led, 0, 0, 0, 0};
  RTOS::queue_send(_queue, &c, QUEUE_TIMEOUT_MS);
}

void LedService::set_indicator_brightness(uint8_t pwm) {
  if (_config.driver == nullptr) {
    return;
  }
  _config.driver->set_channel(30, pwm);
  _config.driver->set_channel(31, pwm);
}

void LedService::set_back_leds_rgb(uint8_t r, uint8_t g, uint8_t b) {
  if (_config.driver == nullptr) {
    return;
  }
  // Back-side RGB groups: LED3=OUT6/7/8, LED5=OUT12/13/14, LED6=OUT15/16/17,
  // LED7=OUT18/19/20, LED9=OUT24/25/26.  Channel order within each group is
  // B (lowest), G, R per v0.3 wiring.
  static constexpr uint8_t BACK_B_CHANNELS[] = {6, 12, 15, 18, 24};
  for (uint8_t b_ch : BACK_B_CHANNELS) {
    _config.driver->set_rgb(b_ch, r, g, b);
  }
}

void LedService::all_off() {
  if (!ready()) {
    return;
  }
  Cmd c{Cmd::Kind::AllOff, Led::Select, 0, 0, 0, 0};
  RTOS::queue_send(_queue, &c, QUEUE_TIMEOUT_MS);
}

void LedService::_task_entry(void *arg) { static_cast<LedService *>(arg)->_run(); }

void LedService::_run() {
  Cmd c;
  if (!RTOS::queue_receive(_queue, &c, UINT32_MAX)) {
    return;
  }
  while (true) {
    bool preempted = false;

    switch (c.kind) {
    case Cmd::Kind::FlashOn: {
      _set_led_rgb(c.led, c.r, c.g, c.b);
      if (c.duration_ms > 0) {
        // Wait for the hold duration OR for a new command — whichever first.
        // A new command preempts: we abandon the off-write and restart with
        // the new command immediately so the latest touch always wins.
        Cmd next;
        if (RTOS::queue_receive(_queue, &next, c.duration_ms)) {
          c = next;
          preempted = true;
          break;
        }
        _set_led_rgb(c.led, 0, 0, 0);
      }
      break;
    }
    case Cmd::Kind::FlashOff:
      _set_led_rgb(c.led, 0, 0, 0);
      break;
    case Cmd::Kind::AllOff:
      for (uint8_t ch = 0; ch < LP5036::NUM_CHANNELS; ++ch) {
        _config.driver->set_channel(ch, 0);
      }
      break;
    }

    if (!preempted) {
      // Block until the next command.
      if (!RTOS::queue_receive(_queue, &c, UINT32_MAX)) {
        return;
      }
    }
  }
}

void LedService::_map(Led led, uint8_t &b_ch, uint8_t &g_ch, uint8_t &r_ch) {
  switch (led) {
  case Led::Select: // LED1: OUT0=B, OUT1=G, OUT2=R
    b_ch = 0;
    g_ch = 1;
    r_ch = 2;
    break;
  case Led::Left: // LED2: OUT3=B, OUT4=G, OUT5=R
    b_ch = 3;
    g_ch = 4;
    r_ch = 5;
    break;
  case Led::Right: // LED10: OUT27=B, OUT28=G, OUT29=R
    b_ch = 27;
    g_ch = 28;
    r_ch = 29;
    break;
  }
}

void LedService::_set_led_rgb(Led led, uint8_t r, uint8_t g, uint8_t b) {
  uint8_t b_ch = 0;
  uint8_t g_ch = 0;
  uint8_t r_ch = 0;
  _map(led, b_ch, g_ch, r_ch);
  // OUT registers for one LED are contiguous (B, G, R); use block write.
  _config.driver->set_rgb(b_ch, r, g, b);
  (void)g_ch;
  (void)r_ch;
}
