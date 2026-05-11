/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ27427_H
#define BQ27427_H

#include <cstdint>

#include "driver/i2c_master.h"

/// TI BQ27427 single-cell System-Side Impedance Track™ Fuel Gauge with
/// Integrated Sense Resistor.  I²C peripheral, 7-bit address fixed at 0x55.
///
/// References:
///   - Datasheet  SLUSEB5B  (Dec 2022, rev Sep 2025)
///   - TRM        SLUUCD5   (Jan 2023)
///
/// Standard Command interface — 8-bit command byte selects a 16-bit LSB-first
/// word.  Datasheet §6.3.1.3: do not poll any standard command faster than
/// 2 Hz, otherwise the gauge's internal watchdog can reset.  All Standard
/// Command reads in this driver therefore use the `_read_word()` helper which
/// performs a single `cmd → repeated-start → read 2 bytes` transaction.
class BQ27427 {
public:
  /// Fixed 7-bit I²C address (datasheet §6.3.1.1).
  static constexpr uint8_t DEFAULT_ADDRESS = 0x55;

  /// Expected response from Control(DEVICE_TYPE) — used to confirm the part.
  static constexpr uint16_t DEVICE_TYPE_BQ27427 = 0x0427;

  struct Config {
    uint8_t address = DEFAULT_ADDRESS;
    uint32_t scl_speed_hz = 400000; // 100 kHz or 400 kHz both supported
    int timeout_ms = 100;
  };

  explicit BQ27427(i2c_master_bus_handle_t i2c_bus);
  BQ27427(i2c_master_bus_handle_t i2c_bus, const Config &config);
  ~BQ27427();

  BQ27427(const BQ27427 &) = delete;
  BQ27427 &operator=(const BQ27427 &) = delete;

  /// Probe the gauge, attach to the bus, and verify DEVICE_TYPE = 0x0427.
  /// Safe to call multiple times — idempotent.
  /// @return true on success.
  bool init();

  /// True when init() has succeeded and the device is attached.
  bool ready() const { return _dev != nullptr; }

  // -- Standard Command reads ------------------------------------------------
  // All return true on success.  On failure the out-parameter is left
  // unchanged so callers can keep a stale-but-valid last reading.

  /// Predicted state-of-charge, 0–100 %.  TRM §5.11 (0x1C / 0x1D).
  bool read_soc_percent(uint8_t &out);

  /// Battery terminal voltage in mV.  TRM §5.4 (0x04 / 0x05).
  bool read_voltage_mv(uint16_t &out);

  /// Average current in mA, signed (+ = charge, − = discharge).
  /// TRM §5.9 (0x10 / 0x11).
  bool read_average_current_ma(int16_t &out);

  /// Internal-sensor temperature in 0.1 K (raw).  TRM §5.12 (0x1E / 0x1F).
  bool read_internal_temperature_dk(uint16_t &out);

  /// Internal-sensor temperature in °C.  Convenience wrapper around
  /// read_internal_temperature_dk().
  bool read_internal_temperature_c(float &out);

  /// Filtered remaining battery capacity in mAh.  TRM §5.14 (0x2A / 0x2B).
  bool read_remaining_capacity_mah(uint16_t &out);

  /// Filtered full-charge capacity in mAh.  TRM §5.16 (0x2E / 0x2F).
  bool read_full_charge_capacity_mah(uint16_t &out);

  /// Average power in mW, signed.  TRM §5.10 (0x18 / 0x19).
  bool read_average_power_mw(int16_t &out);

  /// Flags register (charge/discharge/SOC1/SOCF/etc.).  TRM §5.6 (0x06 / 0x07).
  bool read_flags(uint16_t &out);

  /// Issue a Control() subcommand and read back the 16-bit result.
  /// TRM §4 / §6.1.  Sequence: write 0x00=lo, 0x01=hi → read 0x00/0x01.
  /// @return true on success.
  bool control_subcommand(uint16_t subcmd, uint16_t &result);

private:
  i2c_master_bus_handle_t _bus = nullptr;
  i2c_master_dev_handle_t _dev = nullptr;
  Config _config;

  /// Read a 16-bit standard-command word at the given command byte.
  /// Performs a single repeated-start transaction (write cmd, read 2 bytes,
  /// LSB-first).
  bool _read_word(uint8_t cmd, uint16_t &out);

  /// Write a 16-bit standard-command word.  Used for Control() subcommand
  /// selection (cmd=0x00 → write LSB+MSB).
  bool _write_word(uint8_t cmd, uint16_t value);
};

#endif // BQ27427_H
