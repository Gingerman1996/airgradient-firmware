/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BMS_DEVICE_H
#define BMS_DEVICE_H

#include "types/bms_types.h"

/// Abstract BMS (Battery Management System) device interface.
///
/// Replaces the old BatteryMgmtSensor HAL.  The abstraction is no longer
/// "a sensor" -- it models a power-management device with telemetry reads,
/// status queries, and optional hardware features such as ship mode.
///
/// Policy (watchdog cadence, sleep decisions, shutdown UX) lives above this
/// interface in product-level code.  The HAL only exposes hardware primitives.
class BmsDevice {
public:
  virtual ~BmsDevice() = default;

  /// Initialize the BMS hardware.
  /// @return true on success.
  virtual bool init() = 0;

  // -- Telemetry ------------------------------------------------------------

  /// Read voltage telemetry (battery voltage, charging voltage).
  /// @param out Populated on success; left at invalid sentinels on failure.
  /// @return true if the read succeeded.
  virtual bool read_telemetry(BmsTelemetry &out) = 0;

  /// Read charger status (charging state).
  /// @param out Populated on success; left at defaults on failure.
  /// @return true if the read succeeded.
  virtual bool read_status(BmsStatus &out) = 0;

  /// Lightweight charging-state-only query (single register read).
  /// Use for fast polling when full status is not needed.
  /// @param[out] state Populated on success.
  /// @return true if the read succeeded.
  virtual bool get_charging_state(BmsChargingState &state) = 0;

  /// Estimate battery state-of-charge as a percentage (0–100 %).
  /// @param output Populated on success; left untouched on failure.
  /// @return true if the read succeeded.
  virtual bool get_battery_percentage(float *output) = 0;

  // -- Watchdog -------------------------------------------------------------

  /// Reset the BMS hardware watchdog timer.
  /// Must be called periodically to prevent the watchdog from expiring.
  /// @return true if the reset succeeded (or no reset was needed yet).
  virtual bool update_watchdog() = 0;

  // -- Optional hardware features -------------------------------------------

  /// @return true if this device supports hardware ship mode.
  virtual bool feature_ship_available() const = 0;

  /// Request the device to enter ship mode (power off).
  /// This call is expected not to return on success.
  /// @return false if ship mode is not supported or if the request failed.
  virtual bool enter_ship_mode() = 0;

  /// Configure the PMID rail operating mode.
  ///
  /// Products can use this to switch between external-input pass-through and
  /// OTG boost depending on charger state.
  /// @return true if the request succeeded.
  virtual bool configure_pmid_mode(BmsPmidMode) { return false; }

  /// Enable or disable the charging current path.  When disabled, the
  /// charger IC holds the battery at its current SOC (no current into the
  /// cell) while still passing input current through to the system rail.
  /// Used to stop charging at "full" so the cell isn't held at 100 % when
  /// USB stays connected (better for long-term cell health).
  /// @return true if the request succeeded.  Default: not supported.
  virtual bool set_charge_enable(bool) { return false; }
};

#endif // BMS_DEVICE_H
