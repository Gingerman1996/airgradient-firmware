/**
 * AirGradient Go — TCA9536 implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_io_expander.h"

#include "ag_log.h"

namespace {
constexpr const char *TAG = "Tca9536";
} // namespace

TCA9536::TCA9536(IoExpI2cBusHandle bus, const Config &config) : _config(config), _bus(bus) {}

TCA9536::~TCA9536() {
#ifndef TEST_HOST
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
#endif
}

bool TCA9536::init() {
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
      AG_LOGE(TAG, "bus_add_device failed at addr 0x%02X", _config.address);
      return false;
    }
  }
#endif
  // Sync our cached defaults to the device. The chip's power-on state already
  // matches these values, so this is a defensive normalise rather than a
  // strict requirement.
  if (!_write_reg(REG_OUTPUT, _out_cache)) {
    AG_LOGE(TAG, "OUTPUT register init failed (no ACK at 0x%02X?)", _config.address);
    return false;
  }
  if (!_write_reg(REG_CONFIG, _cfg_cache)) {
    return false;
  }
  AG_LOGI(TAG, "initialised at I²C 0x%02X", _config.address);
  return true;
}

bool TCA9536::set_output(Pin pin) {
  const uint8_t mask = bit_of(pin);
  const uint8_t next = _cfg_cache & static_cast<uint8_t>(~mask);
  if (!_write_reg(REG_CONFIG, next)) {
    return false;
  }
  _cfg_cache = next;
  return true;
}

bool TCA9536::set_input(Pin pin) {
  const uint8_t next = _cfg_cache | bit_of(pin);
  if (!_write_reg(REG_CONFIG, next)) {
    return false;
  }
  _cfg_cache = next;
  return true;
}

bool TCA9536::write(Pin pin, bool high) {
  const uint8_t mask = bit_of(pin);
  const uint8_t next = high ? static_cast<uint8_t>(_out_cache | mask)
                            : static_cast<uint8_t>(_out_cache & ~mask);
  if (next == _out_cache) {
    return true; // nothing to do — saves a transaction
  }
  if (!_write_reg(REG_OUTPUT, next)) {
    return false;
  }
  _out_cache = next;
  return true;
}

bool TCA9536::pulse_low(Pin pin, uint32_t hold_ms) {
  // Drive low immediately, sleep, then return to the previous (high) state.
  // The TAU1113 datasheet says PRTRG must be pulled low for at least the
  // host-controller timing margin (we conservatively pick 10–20 ms).
  if (!write(pin, false)) {
    AG_LOGW(TAG, "pulse_low: drive-low failed");
    return false;
  }
  RTOS::delay_ms(hold_ms);
  if (!write(pin, true)) {
    AG_LOGW(TAG, "pulse_low: release-high failed");
    return false;
  }
  AG_LOGD(TAG, "pulse_low: P%u for %u ms", static_cast<unsigned>(pin),
          static_cast<unsigned>(hold_ms));
  return true;
}

bool TCA9536::_write_reg(uint8_t reg, uint8_t value) {
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
