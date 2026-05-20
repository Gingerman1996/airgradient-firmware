/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CO2_SENSOR_HPP
#define CO2_SENSOR_HPP

#include "measures_types.h"

class CO2Sensor {
public:
  virtual ~CO2Sensor() = default;

  virtual bool init() = 0;
  virtual bool read(CO2Data &out) = 0;

  virtual bool supports_temp_hum() const { return false; }
  virtual TempHumData temp_hum_data() = 0;

  /// Return true if this sensor supports manual baseline calibration.
  virtual bool supports_calibration() const { return false; }

  /// Start a background baseline calibration.
  /// @param baseline_ppm Reference CO2 concentration in ppm.
  ///        Drivers that do not support a configurable baseline may ignore it.
  /// Returns true if the command was accepted by the sensor.
  virtual bool do_baseline_calibration(int baseline_ppm = 400) {
    (void)baseline_ppm;
    return false;
  }

  /// Poll whether a previously started calibration has finished.
  /// Returns true when calibration is complete (or if none was started).
  virtual bool is_baseline_calibration_done() { return true; }

  /// Park the sensor in its lowest-power state (true) or resume normal
  /// measurement (false). Drivers that have no separate low-power mode
  /// may leave this as a no-op. Calls are expected to be idempotent —
  /// the caller may toggle false→false or true→true without effect.
  /// Used by the product layer to suppress sensor draw during cell-side
  /// idle phases (e.g. fuel-gauge learning RELAX_1/RELAX_2).
  virtual void set_low_power(bool on) { (void)on; }

private:
};

#endif // !CO2_SENSOR_HPP
