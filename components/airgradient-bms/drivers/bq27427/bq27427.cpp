/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq27427/bq27427.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char *TAG = "BQ27427";

// Standard Command addresses (TRM §5).
constexpr uint8_t CMD_CONTROL = 0x00;
constexpr uint8_t CMD_TEMPERATURE = 0x02;
constexpr uint8_t CMD_VOLTAGE = 0x04;
constexpr uint8_t CMD_FLAGS = 0x06;
constexpr uint8_t CMD_AVG_CURRENT = 0x10;
constexpr uint8_t CMD_AVG_POWER = 0x18;
constexpr uint8_t CMD_SOC = 0x1C;
constexpr uint8_t CMD_INT_TEMP = 0x1E;
constexpr uint8_t CMD_REMAIN_CAP = 0x2A;        // Filtered
constexpr uint8_t CMD_FULL_CHARGE_CAP = 0x2E;   // Filtered

// Control() subcommands (TRM §4).
constexpr uint16_t CTRL_DEVICE_TYPE = 0x0001;

// Per datasheet §6.3.1.3, the Control() subcommand result is not ready
// immediately after the write — but the spec says only that read-WRITE
// standard commands need 2 s.  Control() is treated as read-only after the
// subcommand select, and works without delay in practice.  Add a small
// settle wait to be conservative.
constexpr int CONTROL_SETTLE_MS = 2;
} // namespace

BQ27427::BQ27427(i2c_master_bus_handle_t i2c_bus) : _bus(i2c_bus) {}

BQ27427::BQ27427(i2c_master_bus_handle_t i2c_bus, const Config &config)
    : _bus(i2c_bus), _config(config) {}

BQ27427::~BQ27427() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
}

bool BQ27427::init() {
  if (_dev != nullptr) {
    return true; // already attached
  }

  esp_err_t err = i2c_master_probe(_bus, _config.address, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "probe at 0x%02X failed: %s", _config.address, esp_err_to_name(err));
    return false;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _config.address,
      .scl_speed_hz = _config.scl_speed_hz,
      .scl_wait_us = 0,
      .flags = {},
  };
  err = i2c_master_bus_add_device(_bus, &dev_cfg, &_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
    _dev = nullptr;
    return false;
  }

  uint16_t device_type = 0;
  if (!control_subcommand(CTRL_DEVICE_TYPE, device_type)) {
    ESP_LOGE(TAG, "DEVICE_TYPE read failed");
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }
  if (device_type != DEVICE_TYPE_BQ27427) {
    ESP_LOGE(TAG, "DEVICE_TYPE=0x%04X, expected 0x%04X — not a BQ27427", device_type,
             DEVICE_TYPE_BQ27427);
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }

  ESP_LOGI(TAG, "BQ27427 found at 0x%02X (DEVICE_TYPE=0x%04X)", _config.address, device_type);
  return true;
}

// ---------------------------------------------------------------------------
// Standard Command reads
// ---------------------------------------------------------------------------

bool BQ27427::read_soc_percent(uint8_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_SOC, raw)) {
    return false;
  }
  if (raw > 100) {
    // Out-of-range — gauge may not be initialised yet.  Don't lie to caller.
    return false;
  }
  out = static_cast<uint8_t>(raw);
  return true;
}

bool BQ27427::read_voltage_mv(uint16_t &out) { return _read_word(CMD_VOLTAGE, out); }

bool BQ27427::read_average_current_ma(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_CURRENT, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw); // signed 16-bit
  return true;
}

bool BQ27427::read_internal_temperature_dk(uint16_t &out) { return _read_word(CMD_INT_TEMP, out); }

bool BQ27427::read_internal_temperature_c(float &out) {
  uint16_t dk = 0;
  if (!read_internal_temperature_dk(dk)) {
    return false;
  }
  out = (static_cast<float>(dk) * 0.1f) - 273.15f;
  return true;
}

bool BQ27427::read_remaining_capacity_mah(uint16_t &out) { return _read_word(CMD_REMAIN_CAP, out); }

bool BQ27427::read_full_charge_capacity_mah(uint16_t &out) {
  return _read_word(CMD_FULL_CHARGE_CAP, out);
}

bool BQ27427::read_average_power_mw(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_POWER, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw);
  return true;
}

bool BQ27427::read_flags(uint16_t &out) { return _read_word(CMD_FLAGS, out); }

bool BQ27427::control_subcommand(uint16_t subcmd, uint16_t &result) {
  if (!_write_word(CMD_CONTROL, subcmd)) {
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(CONTROL_SETTLE_MS));
  return _read_word(CMD_CONTROL, result);
}

// ---------------------------------------------------------------------------
// I²C helpers
// ---------------------------------------------------------------------------

bool BQ27427::_read_word(uint8_t cmd, uint16_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {0, 0};
  // Single transaction: write 1-byte command, repeated-start, read 2 bytes.
  esp_err_t err = i2c_master_transmit_receive(_dev, &cmd, 1, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  // BQ27xxx standard commands are little-endian (LSB at cmd, MSB at cmd+1).
  out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
  return true;
}

bool BQ27427::_write_word(uint8_t cmd, uint16_t value) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[3] = {cmd, static_cast<uint8_t>(value & 0xFF),
                    static_cast<uint8_t>((value >> 8) & 0xFF)};
  esp_err_t err = i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  return true;
}
