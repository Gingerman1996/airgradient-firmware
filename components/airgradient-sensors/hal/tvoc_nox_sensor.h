/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TVOC_NOX_SENSOR_HPP
#define TVOC_NOX_SENSOR_HPP

#include "measures_types.h"

class TVOCNOxSensor {
public:
  virtual ~TVOCNOxSensor() = default;

  virtual bool init() = 0;
  virtual bool read(TVOCNOxData &out) = 0;

  /// Run one conditioning / warmup cycle.
  ///
  /// Drivers that need an initial conditioning period (e.g. SGP41) should
  /// override this to send their conditioning command. The default is a
  /// no-op that succeeds so sensors without a conditioning phase can ignore
  /// it. Callers invoke this repeatedly during the warmup window.
  virtual bool run_conditioning() { return true; }

  /// Update on-driver temperature/humidity compensation used during
  /// raw-signal acquisition. Stored values persist across read() calls
  /// until the next set_compensation() call. Default no-op for drivers
  /// that do not compensate.
  virtual void set_compensation(float temperature_c, float humidity_pct) {
    (void)temperature_c;
    (void)humidity_pct;
  }

private:
};

#endif // !TVOC_NOX_SENSOR_HPP
