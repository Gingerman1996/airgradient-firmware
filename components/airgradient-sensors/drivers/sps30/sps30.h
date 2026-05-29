/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SPS30_HPP
#define SPS30_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/pm_sensor.h"

/**
 * @brief SPS30 particulate matter sensor driver (Sensirion)
 *
 * Communicates with sensor using I2C protocol (100 kHz).
 * Provides mass concentrations (PM1.0, PM2.5, PM4.0, PM10) in ug/m^3 and
 * number concentrations (PM0.5, PM1.0, PM2.5, PM4.0, PM10) in #/cm^3.
 *
 * Mapped to PMData using same-named bins only. Number concentrations are
 * scaled by 100 to convert #/cm^3 -> #/0.1L so the units match the PMS5003
 * convention used elsewhere in PMData.
 * - Atmospheric mass: pm_01 (PM1.0), pm_25 (PM2.5), pm_10 (PM10)
 * - Standard particle: left as invalid (SPS30 reports a single concentration
 *   set, not CF=1 vs atmospheric)
 * - Particle counts: pm_05_pc (PM0.5 number), pm_01_pc (PM1.0 number),
 *   pm_25_pc (PM2.5 number), pm_10_pc (PM10 number); each multiplied by 100
 * - pm_03_pc: left as invalid (SPS30 has no 0.3um particle count)
 * - pm_5_pc:  left as invalid (SPS30 has no 5.0um particle count)
 * - SPS30 PM4.0 mass and PM4.0 number are read but not exposed (no PMData
 *   field with matching size bin)
 *
 * Does not support temperature and humidity.
 */
class SPS30 : public PMSensor {
public:
  /**
   * @brief Construct SPS30 sensor with I2C bus
   * @param i2c_bus I2C master bus handle
   */
  explicit SPS30(i2c_master_bus_handle_t i2c_bus);
  virtual ~SPS30() = default;

  // PMSensor interface implementation
  bool init(bool skip_reset = false) override;
  bool read(PMData &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  bool _measuring;

  // I2C configuration
  static constexpr uint8_t I2C_ADDRESS = 0x69;
  static constexpr uint32_t I2C_CLOCK_HZ = 100000;
  static constexpr int I2C_TIMEOUT_MS = 500;

  // SPS30 I2C commands
  static constexpr uint16_t CMD_START_MEASUREMENT = 0x0010;
  static constexpr uint16_t CMD_STOP_MEASUREMENT = 0x0104;
  static constexpr uint16_t CMD_READ_DATA_READY = 0x0202;
  static constexpr uint16_t CMD_READ_MEASUREMENT = 0x0300;
  static constexpr uint16_t CMD_RESET = 0xD304;

  // Measurement data: 10 floats x 6 bytes each (2 data + 1 CRC per word, 2 words per float)
  static constexpr uint16_t MEASUREMENT_BUFFER_SIZE = 60;

  // Initialization retry parameters
  static constexpr int INIT_PROBE_RETRIES = 3;
  static constexpr int INIT_PROBE_DELAY_MS = 100;
  static constexpr int START_MEASUREMENT_RETRIES = 3;
  static constexpr int START_MEASUREMENT_RETRY_DELAY_MS = 100;
  static constexpr int STOP_MEASUREMENT_DELAY_MS = 50;

  // Number of data-ready polling attempts
  static constexpr int DATA_READY_RETRIES = 20;
  static constexpr int DATA_READY_POLL_INTERVAL_MS = 100;

  // Settle time after restarting measurement (first data available ~1 s)
  static constexpr int RESTART_SETTLE_MS = 1100;

  /**
   * @brief Poll the data-ready flag until set or retries exhausted
   * @return true if data is ready
   */
  bool _poll_data_ready();

  /**
   * @brief Start measurement in IEEE754 float output format (with retry)
   * @return true if measurement started successfully
   */
  bool _start_measurement();

  /**
   * @brief Stop an ongoing measurement
   * @return true if stop command was acknowledged
   */
  bool _stop_measurement();

  /**
   * @brief Calculate CRC-8 for Sensirion I2C protocol
   * Polynomial: 0x31, Init: 0xFF
   */
  static uint8_t _calc_crc8(const uint8_t *data, uint8_t len);

  /**
   * @brief Send a 16-bit I2C command (no data payload)
   * @return true if successful
   */
  bool _write_command(uint16_t cmd);

  /**
   * @brief Read raw data from I2C with CRC verification
   * @param buffer Output buffer (must be at least len bytes)
   * @param len Number of bytes to read (must be multiple of 3)
   * @return true if successful and all CRCs valid
   */
  bool _read_data(uint8_t *buffer, uint16_t len);

  /**
   * @brief Extract IEEE754 float from I2C 6-byte format
   * Format: [MSB_hi][MSB_lo][CRC][LSB_hi][LSB_lo][CRC]
   */
  static float _extract_float(const uint8_t *data);
};

#endif // SPS30_HPP
