/**
 * AirGradient Go — LIS2DH12 implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_accel.h"

#include "ag_log.h"

namespace {
constexpr const char *TAG = "Lis2dh12";
} // namespace

LIS2DH12::LIS2DH12(AccelI2cBusHandle bus, const Config &config) : _config(config), _bus(bus) {}

LIS2DH12::~LIS2DH12() {
#ifndef TEST_HOST
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
#endif
}

bool LIS2DH12::init() {
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

  const uint8_t id = who_am_i();
  if (id != WHO_AM_I_EXPECTED) {
    AG_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X expected 0x%02X", id, WHO_AM_I_EXPECTED);
    return false;
  }

  if (!_write_reg(REG_CTRL_REG1, CTRL_REG1_CONFIG)) {
    AG_LOGE(TAG, "CTRL_REG1 write failed");
    return false;
  }
  if (!_write_reg(REG_CTRL_REG4, CTRL_REG4_CONFIG)) {
    AG_LOGE(TAG, "CTRL_REG4 write failed");
    return false;
  }

  // The first conversion after enabling needs one ODR period — at 100 Hz
  // that's 10 ms. Wait a bit longer so the first read is meaningful.
  RTOS::delay_ms(20);

  AG_LOGI(TAG, "initialised at I²C 0x%02X (WHO_AM_I=0x%02X)", _config.address, id);
  return true;
}

uint8_t LIS2DH12::who_am_i() {
  uint8_t v = 0;
  if (!_read_reg(REG_WHO_AM_I, v)) {
    return 0;
  }
  return v;
}

bool LIS2DH12::read(Reading &out) {
  uint8_t buf[6] = {};
  if (!_read_block(REG_OUT_X_L | AUTO_INCR, buf, sizeof(buf))) {
    return false;
  }
  // Each axis is a signed 16-bit value left-justified to 12 bits at ±2 g.
  // 1 mg / LSB after the >> 4 right-shift. (Datasheet table 3.)
  const int16_t raw_x = static_cast<int16_t>(buf[0] | (buf[1] << 8));
  const int16_t raw_y = static_cast<int16_t>(buf[2] | (buf[3] << 8));
  const int16_t raw_z = static_cast<int16_t>(buf[4] | (buf[5] << 8));
  out.x_mg = static_cast<int16_t>(raw_x >> 4);
  out.y_mg = static_cast<int16_t>(raw_y >> 4);
  out.z_mg = static_cast<int16_t>(raw_z >> 4);
  return true;
}

bool LIS2DH12::_write_reg(uint8_t reg, uint8_t value) {
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

bool LIS2DH12::_read_reg(uint8_t reg, uint8_t &out) {
#ifndef TEST_HOST
  if (_dev == nullptr) {
    return false;
  }
  return i2c_master_transmit_receive(_dev, &reg, 1, &out, 1, _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  out = 0;
  return true;
#endif
}

bool LIS2DH12::_read_block(uint8_t reg, uint8_t *buf, size_t len) {
#ifndef TEST_HOST
  if (_dev == nullptr || buf == nullptr || len == 0) {
    return false;
  }
  return i2c_master_transmit_receive(_dev, &reg, 1, buf, len, _config.timeout_ms) == ESP_OK;
#else
  (void)reg;
  (void)len;
  if (buf != nullptr) {
    for (size_t i = 0; i < len; ++i) buf[i] = 0;
  }
  return true;
#endif
}
