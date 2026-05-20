/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SCD4X_HPP
#define SCD4X_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/co2_sensor.h"

/**
 * @brief Sensirion SCD4x CO2 sensor adapter (SCD40 / SCD41 / SCD43)
 *
 * Wraps the shared `embedded-i2c-scd4x` Sensirion driver and adapts it to the
 * `CO2Sensor` interface used by the rest of the firmware. Runs the sensor in
 * periodic-measurement mode (5 s update interval) and exposes the integrated
 * temperature and humidity readings via `supports_temp_hum()` /
 * `temp_hum_data()`.
 *
 * Raw data is already converted to physical units by the Sensirion driver:
 * - CO2: ppm (uint16)
 * - Temperature: milli-degrees Celsius (int32)
 * - Humidity: milli-percent RH (int32)
 *
 * Calibration is implemented via Sensirion's Forced Recalibration (FRC):
 * do_baseline_calibration() stops periodic measurement, issues the FRC
 * command synchronously, restarts periodic measurement and returns the
 * overall success. The base-class is_baseline_calibration_done() default
 * (always true) matches the synchronous semantic, so no override is needed.
 * The caller must have operated the sensor in periodic measurement mode for
 * at least 3 minutes in a stable CO2 environment before invoking FRC.
 *
 * @note Singleton constraint: the underlying `embedded-i2c-scd4x` component
 *       keeps the I2C bus handle and the I2C device address in file-scope
 *       globals. Only one `SCD4x` instance can be used at a time. Constructing
 *       a second instance (even on a different bus) will rebind those globals
 *       and break the first instance.
 */
class SCD4x : public CO2Sensor {
public:
  /**
   * @brief Default I2C address shared by SCD40 / SCD41 / SCD43
   */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x62;

  /**
   * @brief Construct SCD4x sensor adapter with I2C bus
   * @param i2c_bus I2C master bus handle
   * @param address I2C address (default 0x62)
   */
  explicit SCD4x(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_DEFAULT);
  virtual ~SCD4x() = default;

  // CO2Sensor interface implementation
  bool init() override;
  bool read(CO2Data &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

  /**
   * @brief Report that this driver supports manual baseline calibration.
   */
  bool supports_calibration() const override { return true; }

  /**
   * @brief Perform a Sensirion Forced Recalibration (FRC) of the SCD4x.
   *
   * The full FRC sequence runs synchronously: stop periodic measurement,
   * wait the required idle time, issue the FRC command with the requested
   * target concentration, log the returned correction (in ppm), and
   * restart periodic measurement so normal reads resume.
   *
   * @param baseline_ppm Reference CO2 concentration in ppm
   *                     (clamped to 400 ppm when <= 0).
   * @return true if the sensor accepted the recalibration; false if the
   *         sensor was not initialized, the FRC command errored, the
   *         sensor reported the FRC-failed sentinel (0xFFFF, i.e. the
   *         sensor had not been operated long enough), or periodic
   *         measurement could not be restarted.
   *
   * @note Sensirion requires the sensor to have been running in periodic
   *       measurement for at least 3 minutes in a stable CO2 environment
   *       before FRC can succeed. Enforcing that precondition is up to
   *       the product layer / user workflow.
   */
  bool do_baseline_calibration(int baseline_ppm = 400) override;

  /// Stop the SCD4x's periodic measurement to save ~17 mA average
  /// (idle ~0.4 mA vs periodic ~17 mA). On exit, restart periodic
  /// measurement so subsequent reads succeed. Idempotent.
  void set_low_power(bool on) override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  uint8_t _address;
  bool _measuring;

  // Cached last temp/hum reading from the most recent successful read()
  TempHumData _last_temp_hum;

  // I2C probe configuration
  static constexpr int I2C_PROBE_TIMEOUT_MS = 500;

  // Initialization retry parameters
  static constexpr int INIT_PROBE_RETRIES = 3;
  static constexpr int INIT_PROBE_DELAY_MS = 100;
  static constexpr int START_MEASUREMENT_RETRIES = 3;
  static constexpr int START_MEASUREMENT_RETRY_DELAY_MS = 100;

  // Clean-state sequence timings (per Sensirion example)
  static constexpr int WAKE_UP_DELAY_MS = 30;
  static constexpr int STOP_PERIODIC_DELAY_MS = 500;

  // Read retry parameters
  static constexpr int READ_MEASUREMENT_RETRIES = 3;
  static constexpr int READ_MEASUREMENT_RETRY_DELAY_MS = 10;

  // Serial number is 3 words (6 bytes) — Sensirion driver API requires the
  // caller to pass a word buffer and its word length.
  static constexpr uint16_t SERIAL_NUMBER_WORD_COUNT = 3;

  // --- Forced Recalibration (FRC) -------------------------------------------
  // Default reference concentration used when the caller passes <= 0 ppm.
  static constexpr uint16_t CAL_DEFAULT_TARGET_PPM = 400;
  // frc_correction sentinel returned by the sensor when FRC fails (e.g. the
  // sensor had not been operated in periodic mode long enough before FRC).
  static constexpr uint16_t FRC_FAILED_SENTINEL = 0xFFFF;
  // The raw frc_correction word encodes the applied correction as
  //   correction_ppm = raw - FRC_CORRECTION_OFFSET
  // per the Sensirion SCD4x datasheet.
  static constexpr uint16_t FRC_CORRECTION_OFFSET = 0x8000;

  /**
   * @brief Start periodic measurement with retry.
   *
   * Shared between init() and the FRC post-amble in do_baseline_calibration().
   * Updates `_measuring = true` on success. On failure leaves `_measuring`
   * unchanged (caller decides whether to clear it).
   *
   * @return true if the sensor accepted start_periodic_measurement within
   *         START_MEASUREMENT_RETRIES attempts.
   */
  bool _start_periodic_measurement();
};

#endif // SCD4X_HPP
