/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BMS_TYPES_H
#define BMS_TYPES_H

#include <cstdint>

// ---------------------------------------------------------------------------
// Invalid sentinels
// ---------------------------------------------------------------------------

namespace BmsInvalid {
static constexpr float VOLT = -1.0f;
static constexpr float PERCENT = -1.0f;
static constexpr int16_t CURRENT_MA = -32768; // INT16_MIN
static constexpr uint16_t VOLTAGE_MV = 65535; // UINT16_MAX
static constexpr int16_t TEMPERATURE_C = -32768;
/// Sentinel for float battery temperature (well below any plausible real
/// reading; anything >= TEMPERATURE_FLOAT_C_MIN_VALID is considered valid).
static constexpr float TEMPERATURE_FLOAT_C = -999.0f;
static constexpr float TEMPERATURE_FLOAT_C_MIN_VALID = -100.0f;
} // namespace BmsInvalid

// ---------------------------------------------------------------------------
// Validation range
// ---------------------------------------------------------------------------

namespace BmsRange {
static constexpr float MIN_VALID_VOLT = 0.0f;
} // namespace BmsRange

// ---------------------------------------------------------------------------
// BmsChargingState
// ---------------------------------------------------------------------------

/// Shared public charging-state enum.  Replaces the driver-local
/// BQ25XX::ChargingStatus so that higher-level code does not need to
/// depend on a specific charger driver header.
enum class BmsChargingState : uint8_t {
  Unknown,
  NotCharging,
  TrickleCharge,
  PreCharge,
  FastCharge,
  TaperCharge,
  TopOffTimerActiveCharging,
  ChargeTerminationDone,
};

/// Human-readable label for a BmsChargingState value.
inline const char *bms_charging_state_str(BmsChargingState s) {
  switch (s) {
  case BmsChargingState::Unknown:
    return "Unknown";
  case BmsChargingState::NotCharging:
    return "NotCharging";
  case BmsChargingState::TrickleCharge:
    return "TrickleCharge";
  case BmsChargingState::PreCharge:
    return "PreCharge";
  case BmsChargingState::FastCharge:
    return "FastCharge";
  case BmsChargingState::TaperCharge:
    return "TaperCharge";
  case BmsChargingState::TopOffTimerActiveCharging:
    return "TopOff";
  case BmsChargingState::ChargeTerminationDone:
    return "Done";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BmsTelemetry
// ---------------------------------------------------------------------------

/// ADC telemetry snapshot from a BMS device.
///
/// Contains voltage, current, and temperature readings from the charger
/// IC's integrated ADC.  All fields default to invalid sentinels so that
/// implementations that only populate a subset still produce a well-defined
/// result.
struct BmsTelemetry {
  // --- Voltages (V, float) ---
  float battery_voltage = BmsInvalid::VOLT;  ///< Battery terminal voltage (V)
  float charging_voltage = BmsInvalid::VOLT; ///< Input / VBUS voltage (V)

  // --- Currents (mA, signed) ---
  int16_t input_current_ma = BmsInvalid::CURRENT_MA;   ///< Input bus current
  int16_t battery_current_ma = BmsInvalid::CURRENT_MA; ///< Battery current (+charge / -discharge)

  // --- Additional voltages (mV, unsigned) ---
  uint16_t system_voltage_mv = BmsInvalid::VOLTAGE_MV; ///< System rail voltage
  uint16_t pmid_voltage_mv = BmsInvalid::VOLTAGE_MV;   ///< PMID / power-path voltage

  // --- Temperature ---
  float ts_percent = BmsInvalid::PERCENT;                ///< Thermistor ADC reading (%)
  int16_t die_temperature_c = BmsInvalid::TEMPERATURE_C; ///< IC die temperature (°C)
  /// Battery temperature in °C derived from the TS-pin NTC.  Drivers that
  /// don't expose a NTC conversion leave this at sentinel.  Use
  /// is_battery_temperature_valid() before consuming.
  float battery_temperature_c = BmsInvalid::TEMPERATURE_FLOAT_C;

  // --- Validation helpers ---
  bool is_battery_voltage_valid() const { return battery_voltage >= BmsRange::MIN_VALID_VOLT; }

  bool is_charging_voltage_valid() const { return charging_voltage >= BmsRange::MIN_VALID_VOLT; }

  bool is_battery_temperature_valid() const {
    return battery_temperature_c >= BmsInvalid::TEMPERATURE_FLOAT_C_MIN_VALID;
  }

  bool is_valid() const { return is_battery_voltage_valid() && is_charging_voltage_valid(); }
};

// ---------------------------------------------------------------------------
// BmsPowerSource
// ---------------------------------------------------------------------------

/// Generic power source / adapter type detected by the charger IC.
/// Maps from device-specific VBUS status registers.
enum class BmsPowerSource : uint8_t {
  Unknown,        ///< Status not read or not supported
  None,           ///< No adapter connected
  UsbSdp,         ///< USB Standard Downstream Port (500 mA)
  UsbCdp,         ///< USB Charging Downstream Port (1.5 A)
  UsbDcp,         ///< USB Dedicated Charging Port (1.5 A)
  UnknownAdapter, ///< Adapter detected, type unknown (500 mA)
  NonStandard,    ///< Non-standard adapter (1 A / 2.1 A / 2.4 A)
  OtgMode,        ///< OTG boost mode active (device is source)
};

/// Human-readable label for a BmsPowerSource value.
inline const char *bms_power_source_str(BmsPowerSource s) {
  switch (s) {
  case BmsPowerSource::Unknown:
    return "Unknown";
  case BmsPowerSource::None:
    return "None";
  case BmsPowerSource::UsbSdp:
    return "USB_SDP";
  case BmsPowerSource::UsbCdp:
    return "USB_CDP";
  case BmsPowerSource::UsbDcp:
    return "USB_DCP";
  case BmsPowerSource::UnknownAdapter:
    return "UnknownAdapter";
  case BmsPowerSource::NonStandard:
    return "NonStandard";
  case BmsPowerSource::OtgMode:
    return "OTG";
  }
  return "?";
}

/// Return true when the charger reports an external input source.
///
/// OTG mode means the device itself is sourcing power on PMID, so it is not
/// treated as an external input.
inline bool bms_power_source_has_external_input(BmsPowerSource s) {
  switch (s) {
  case BmsPowerSource::UsbSdp:
  case BmsPowerSource::UsbCdp:
  case BmsPowerSource::UsbDcp:
  case BmsPowerSource::UnknownAdapter:
  case BmsPowerSource::NonStandard:
    return true;
  case BmsPowerSource::Unknown:
  case BmsPowerSource::None:
  case BmsPowerSource::OtgMode:
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// BmsPmidMode
// ---------------------------------------------------------------------------

/// Requested PMID rail operating mode.
enum class BmsPmidMode : uint8_t {
  Unknown,
  PassThrough,
  Boost,
};

/// Human-readable label for a BmsPmidMode value.
inline const char *bms_pmid_mode_str(BmsPmidMode mode) {
  switch (mode) {
  case BmsPmidMode::Unknown:
    return "Unknown";
  case BmsPmidMode::PassThrough:
    return "PassThrough";
  case BmsPmidMode::Boost:
    return "Boost";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BmsStatus
// ---------------------------------------------------------------------------

/// Charger status snapshot from a BMS device.
///
/// Contains charging state, power source identification, and charger
/// regulation / fault flags.  All boolean flags default to false and
/// enum fields default to Unknown so that implementations that only
/// populate a subset still produce a well-defined result.
struct BmsStatus {
  BmsChargingState charging_state = BmsChargingState::Unknown;
  BmsPowerSource power_source = BmsPowerSource::Unknown;

  bool thermal_regulation = false;       ///< Charger in thermal regulation
  bool vsys_regulation = false;          ///< VSYS at minimum system voltage
  bool input_current_regulation = false; ///< Input current limit active
  bool input_voltage_regulation = false; ///< Input voltage limit active
  bool safety_timer_expired = false;     ///< Safety timer has expired
  bool watchdog_expired = false;         ///< Watchdog timer has expired

  bool is_charging_state_valid() const { return charging_state != BmsChargingState::Unknown; }

  bool is_power_source_valid() const { return power_source != BmsPowerSource::Unknown; }

  bool is_valid() const { return is_charging_state_valid(); }
};

#endif // BMS_TYPES_H
