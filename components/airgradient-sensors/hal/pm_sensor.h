/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PM_SENSOR_HPP
#define PM_SENSOR_HPP

#include "measures_types.h"

class PMSensor {
public:
  virtual ~PMSensor() = default;

  virtual bool init(bool skip_reset = false) = 0;
  virtual bool read(PMData &out) = 0;

  virtual bool supports_temp_hum() const { return false; }
  virtual TempHumData temp_hum_data() = 0;

  /// Park the sensor in its lowest-power state. Drivers that cannot lower
  /// their draw without a power cycle leave this as a no-op returning false.
  /// Used by the product layer to cut PM draw between widely-spaced
  /// measurements (e.g. SPS30 Sleep at >= 20 s intervals).
  /// Named enter_sleep/exit_sleep (not sleep/wake) to avoid colliding with
  /// PMS5003Base's pre-existing non-virtual sleep()/wake_up() helpers.
  /// @return true if the sensor entered its low-power state.
  virtual bool enter_sleep() { return false; }

  /// Resume from the low-power state back to a measuring/ready state.
  /// No-op default returning false for drivers without a sleep mode.
  /// Must be called before the PM warmup so the sensor is live in time.
  /// @return true if the sensor was resumed.
  virtual bool exit_sleep() { return false; }

private:
};
#endif // !PM_SENSOR_HPP
