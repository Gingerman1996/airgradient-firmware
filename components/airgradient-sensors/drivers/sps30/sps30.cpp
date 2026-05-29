/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/sps30/sps30.h"

#include <cstring>

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "SPS30";

SPS30::SPS30(i2c_master_bus_handle_t i2c_bus)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _measuring(false) {}

bool SPS30::init(bool skip_reset) {
  // Probe I2C bus to verify sensor is present (with retry for boot timing)
  bool probed = false;
  for (int i = 0; i < INIT_PROBE_RETRIES; i++) {
    esp_err_t ret = i2c_master_probe(_i2c_bus, I2C_ADDRESS, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
      probed = true;
      break;
    }
    ESP_LOGW(TAG, "Probe attempt %d/%d failed: %s", i + 1, INIT_PROBE_RETRIES,
             esp_err_to_name(ret));
    RTOS::delay_ms(INIT_PROBE_DELAY_MS);
  }
  if (!probed) {
    ESP_LOGE(TAG, "SPS30 not found at address 0x%02X", I2C_ADDRESS);
    return false;
  }

  // Add device to I2C bus
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = I2C_ADDRESS,
      .scl_speed_hz = I2C_CLOCK_HZ,
      .scl_wait_us = 0,
      .flags = {},
  };

  esp_err_t ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add SPS30 to I2C bus: %s", esp_err_to_name(ret));
    return false;
  }

  // Reset sensor (skip when re-attaching to a sensor that was kept powered
  // across deep sleep — the fan is already spinning and data is flowing)
  if (!skip_reset) {
    if (!_write_command(CMD_RESET)) {
      ESP_LOGW(TAG, "Reset failed, continuing anyway");
    }
    RTOS::delay_ms(100);
  }

  // start_measurement is idempotent — safe even if already measuring
  if (!_start_measurement()) {
    return false;
  }

  ESP_LOGI(TAG, "SPS30 initialized (skip_reset=%d)", skip_reset);
  return true;
}

bool SPS30::read(PMData &out) {
  // Initialize to invalid sentinels
  out.pm_01 = MeasuresInvalid::PM;
  out.pm_25 = MeasuresInvalid::PM;
  out.pm_10 = MeasuresInvalid::PM;
  out.pm_01_sp = MeasuresInvalid::PM;
  out.pm_25_sp = MeasuresInvalid::PM;
  out.pm_10_sp = MeasuresInvalid::PM;
  out.pm_03_pc = MeasuresInvalid::PM;
  out.pm_05_pc = MeasuresInvalid::PM;
  out.pm_01_pc = MeasuresInvalid::PM;
  out.pm_25_pc = MeasuresInvalid::PM;
  out.pm_5_pc = MeasuresInvalid::PM;
  out.pm_10_pc = MeasuresInvalid::PM;

  if (!_measuring || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized or not measuring");
    return false;
  }

  // Poll data-ready flag
  if (!_poll_data_ready()) {
    // Data not ready — restart measurement and retry once
    ESP_LOGW(TAG, "Data not ready after polling, restarting measurement");
    _stop_measurement();
    if (!_start_measurement()) {
      return false;
    }

    RTOS::delay_ms(RESTART_SETTLE_MS);

    if (!_poll_data_ready()) {
      ESP_LOGE(TAG, "Data still not ready after restart");
      return false;
    }
  }

  // Send read measurement command
  if (!_write_command(CMD_READ_MEASUREMENT)) {
    ESP_LOGE(TAG, "Failed to send read measurement command");
    return false;
  }
  RTOS::delay_ms(10);

  // Read 60 bytes: 10 floats x 6 bytes each
  uint8_t buffer[MEASUREMENT_BUFFER_SIZE];
  if (!_read_data(buffer, MEASUREMENT_BUFFER_SIZE)) {
    ESP_LOGE(TAG, "Failed to read measurement data");
    return false;
  }

  // Extract SPS30 measurements (10 floats in order):
  // [0] PM1.0 mass, [1] PM2.5 mass, [2] PM4.0 mass, [3] PM10 mass
  // [4] PM0.5 number, [5] PM1.0 number, [6] PM2.5 number, [7] PM4.0 number,
  // [8] PM10 number, [9] typical particle size

  float pm1_mass = _extract_float(&buffer[0]);
  float pm25_mass = _extract_float(&buffer[6]);
  // float pm4_mass  = _extract_float(&buffer[12]); // PM4.0 - no PMData field
  float pm10_mass = _extract_float(&buffer[18]);

  float pm05_num = _extract_float(&buffer[24]);
  float pm1_num = _extract_float(&buffer[30]);
  float pm25_num = _extract_float(&buffer[36]);
  // float pm4_num = _extract_float(&buffer[42]); // PM4.0 - no PMData field
  float pm10_num = _extract_float(&buffer[48]);
  // float typical_size = _extract_float(&buffer[54]); // not needed

  // Map to PMData atmospheric fields (ug/m^3)
  out.pm_01 = pm1_mass;
  out.pm_25 = pm25_mass;
  out.pm_10 = pm10_mass;

  // Standard particle fields left as invalid (SPS30 doesn't distinguish CF=1
  // vs atmospheric)

  // Map particle counts to same-named PMData bins.
  // SPS30 reports number concentrations in #/cm^3; PMData (PMS5003
  // convention) uses #/0.1L, so multiply by 100.
  //   SPS30 PM0.5 number -> pm_05_pc
  //   SPS30 PM1.0 number -> pm_01_pc
  //   SPS30 PM2.5 number -> pm_25_pc
  //   SPS30 PM10  number -> pm_10_pc
  // SPS30 has no PM0.3, PM5.0, and PM4.0 doesn't map to any PMData bin, so
  // pm_03_pc, pm_5_pc remain at MeasuresInvalid::PM.
  static constexpr float CM3_TO_PER_DECILITRE = 100.0f;
  out.pm_05_pc = pm05_num * CM3_TO_PER_DECILITRE;
  out.pm_01_pc = pm1_num * CM3_TO_PER_DECILITRE;
  out.pm_25_pc = pm25_num * CM3_TO_PER_DECILITRE;
  out.pm_10_pc = pm10_num * CM3_TO_PER_DECILITRE;

  ESP_LOGD(TAG, "PM1.0=%.2f PM2.5=%.2f PM10=%.2f", out.pm_01, out.pm_25, out.pm_10);

  return true;
}

bool SPS30::supports_temp_hum() const { return false; }

TempHumData SPS30::temp_hum_data() {
  TempHumData data;
  data.temperature = MeasuresInvalid::TEMPERATURE;
  data.humidity = MeasuresInvalid::HUMIDITY;
  return data;
}

bool SPS30::_poll_data_ready() {
  for (int i = 0; i < DATA_READY_RETRIES; i++) {
    if (!_write_command(CMD_READ_DATA_READY)) {
      ESP_LOGW(TAG, "Failed to send data-ready command");
      return false;
    }
    RTOS::delay_ms(5);

    uint8_t ready_buf[3];
    if (!_read_data(ready_buf, 3)) {
      ESP_LOGW(TAG, "Failed to read data-ready flag");
      return false;
    }

    uint16_t flag = (static_cast<uint16_t>(ready_buf[0]) << 8) | ready_buf[1];
    if (flag == 0x0001) {
      return true;
    }

    RTOS::delay_ms(DATA_READY_POLL_INTERVAL_MS);
  }

  return false;
}

bool SPS30::_start_measurement() {
  if (_dev_handle == nullptr) {
    return false;
  }

  // Command: [0x00][0x10] + Data: [0x03][0x00] + CRC
  uint8_t start_buf[5];
  start_buf[0] = (CMD_START_MEASUREMENT >> 8) & 0xFF;
  start_buf[1] = CMD_START_MEASUREMENT & 0xFF;
  start_buf[2] = 0x03; // IEEE754 float output format
  start_buf[3] = 0x00; // Dummy byte
  start_buf[4] = _calc_crc8(&start_buf[2], 2);

  for (int i = 0; i < START_MEASUREMENT_RETRIES; i++) {
    esp_err_t ret = i2c_master_transmit(_dev_handle, start_buf, 5, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
      RTOS::delay_ms(20);
      _measuring = true;
      return true;
    }
    ESP_LOGW(TAG, "Start measurement attempt %d/%d failed: %s", i + 1, START_MEASUREMENT_RETRIES,
             esp_err_to_name(ret));
    RTOS::delay_ms(START_MEASUREMENT_RETRY_DELAY_MS);
  }

  ESP_LOGE(TAG, "Failed to start measurement after %d attempts", START_MEASUREMENT_RETRIES);
  return false;
}

bool SPS30::_stop_measurement() {
  if (!_write_command(CMD_STOP_MEASUREMENT)) {
    ESP_LOGW(TAG, "Failed to send stop measurement command");
    return false;
  }
  _measuring = false;
  RTOS::delay_ms(STOP_MEASUREMENT_DELAY_MS);
  return true;
}

uint8_t SPS30::_calc_crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x31;
      } else {
        crc = (crc << 1);
      }
    }
  }
  return crc;
}

bool SPS30::_write_command(uint16_t cmd) {
  if (_dev_handle == nullptr) {
    return false;
  }

  uint8_t buf[2];
  buf[0] = (cmd >> 8) & 0xFF;
  buf[1] = cmd & 0xFF;

  esp_err_t ret = i2c_master_transmit(_dev_handle, buf, 2, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C write command 0x%04X failed: %s", cmd, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool SPS30::_read_data(uint8_t *buffer, uint16_t len) {
  if (_dev_handle == nullptr) {
    return false;
  }

  esp_err_t ret = i2c_master_receive(_dev_handle, buffer, len, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C read failed: %s", esp_err_to_name(ret));
    return false;
  }

  // Verify CRC for every 3-byte word (data[0:1] + crc[2])
  for (uint16_t i = 0; i + 2 < len; i += 3) {
    uint8_t expected_crc = _calc_crc8(&buffer[i], 2);
    if (buffer[i + 2] != expected_crc) {
      ESP_LOGW(TAG, "CRC mismatch at offset %d: got 0x%02x, expected 0x%02x", i, buffer[i + 2],
               expected_crc);
      return false;
    }
  }

  return true;
}

float SPS30::_extract_float(const uint8_t *data) {
  // I2C format: [B3][B2][CRC][B1][B0][CRC]
  // Reassemble 4 data bytes, skipping CRC bytes at positions 2 and 5
  uint32_t value = (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
                   (static_cast<uint32_t>(data[3]) << 8) | static_cast<uint32_t>(data[4]);

  float result;
  memcpy(&result, &value, sizeof(float));
  return result;
}
