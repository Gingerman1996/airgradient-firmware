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

  // -- Data Memory configuration --------------------------------------------
  // The chip's Impedance Track algorithm needs to know the cell's nominal
  // capacity (and chemistry).  These methods access "Data Memory" via the
  // Extended Command interface (TRM §4.1 / §6).

  /// Read the current Design Capacity (mAh) from data memory.  Does not enter
  /// CFGUPDATE mode and so does not perturb the gauge's learned state.
  /// @return true on success.
  bool read_design_capacity_mah(uint16_t &out);

  /// Number of grid points in the learned Ra (impedance) table.
  /// TRM §7.4.3 (p48): the Ra Table class has 15 values.
  static constexpr int RA_TABLE_SIZE = 15;

  /// Read the learned Qmax Cell 0 value from Data Memory (State subclass 0x52,
  /// offset 0/1; TRM §7.4.2.3.1, p43).  The stored value is fixed-point, NOT
  /// mAh — convert with `Qmax(mAh) = raw × Design Capacity / 2^14` (factory
  /// default raw = 16384 = 1× Design Capacity).  Pure read: a Data Memory
  /// block transfer in UNSEALED mode (TRM §7.1.1–7.1.2, p29) — does NOT enter
  /// CFGUPDATE and does NOT perturb learned state.
  /// @return true on success.
  bool read_qmax_cell0(uint16_t &raw_out);

  /// Read the learned Ra impedance grid — RA_TABLE_SIZE signed 16-bit values
  /// from the Ra0 RAM subclass (0x59, offsets 0..29; TRM §7.4.3, p48).  Values
  /// are internal units (mΩ normalized to 25 °C, scaled by Design Capacity);
  /// a healthy table has no non-positive entries and smooth grid-to-grid
  /// transitions.  Pure UNSEALED block read (TRM §7.1.1–7.1.2, p29) — no
  /// CFGUPDATE, no perturbation of learned state.
  /// @param out  Caller-provided array of at least RA_TABLE_SIZE elements.
  /// @return true on success.
  bool read_ra_table(int16_t out[RA_TABLE_SIZE]);

  /// Read the active chemistry profile ID via Control(CHEM_ID) (TRM §5.1.15,
  /// p7).  Default is 0x3230 (4.35 V); the 4.2 V profile is 0x1202.  Pure
  /// read — does not enter CFGUPDATE.
  /// @return true on success.
  bool read_chem_id(uint16_t &out);

  /// Ensure the gauge is on the 4.2 V chemistry profile (Chem ID 1202, via
  /// CHEM_B).  The chip defaults to the 4.35 V profile (0x3230); on a board
  /// whose charger tops the cell at 4.20 V that profile never reaches its
  /// Taper Voltage, so Full-Charge never latches and SOC saturates below
  /// 100 % (TRM p7/p18/p46/p49).
  ///
  /// Idempotent: when the active profile is already 4.2 V this is a no-op and
  /// does NOT enter CFGUPDATE, so learned state is preserved.  When a switch
  /// is needed it runs UNSEAL → CFGUPDATE → CHEM_B → SOFT_RESET, which
  /// **resets Impedance-Track learning** (Qmax/Ra are chemistry-specific) — so
  /// any learning cycle must be (re)run afterwards.
  /// @return true on success or when no change was needed.
  bool select_chemistry_4v2();

  /// Enable or disable from-scratch Impedance-Track learning by writing the
  /// Update Status byte's bit0+bit1 (subclass 0x52 / State, offset 2; TRM
  /// §7.4.2.3.2, p43).  Per the TRM: *"Only if a learning cycle is to be
  /// completed during initial configuration of the gauge's golden file should
  /// bit 0 and bit 1 be set"* — setting them lifts the per-update change
  /// limits (Max Qmax Change, Qmax Max Delta%, Ra Filter, Ra Max Delta) so
  /// Qmax and the whole Ra grid can move freely in as few cycles as possible
  /// (`fg_learning_sequence.md:55`).  Clear them at completion so the shipped
  /// unit applies the normal bounded field-refinement limits
  /// (`fg_learning_sequence.md:93`).
  ///
  /// Drives the same UNSEAL → CFGUPDATE → block-write → checksum → SOFT_RESET
  /// path as configure_cell().  Idempotent: a no-op (no CFGUPDATE entered, so
  /// learned state is preserved) when the two bits already hold the requested
  /// value.
  /// @param enable  true → set bit0+bit1 (learning armed); false → clear them.
  /// @return true on success or when no change was needed.
  bool set_update_status_learning(bool enable);

  /// Write a new Design Capacity (mAh) to data memory if the current value
  /// differs from `mah`.  Drives the full CFGUPDATE → write block → checksum
  /// → SOFT_RESET sequence (TRM §4.1).  Idempotent: a no-op when already
  /// correct so it's safe to call on every boot.  Skips UNSEAL/SEAL — the
  /// chip ships unsealed from the factory.
  /// @return true on success or when no change was needed.
  bool set_design_capacity_mah(uint16_t mah);

  /// Cell configuration parameters living in the State subclass (0x52).
  /// Each field is a 16-bit big-endian (MSB-first) integer in Data Memory.
  struct CellConfig {
    uint16_t design_capacity_mah;   ///< Initial Qmax estimate (range 0..8000)
    uint16_t design_energy_mwh;     ///< For constant-power load models
    uint16_t terminate_voltage_mv;  ///< Cell-empty cutoff (range 2500..3700)
    uint16_t sleep_current_ma;      ///< AverageCurrent below this = relaxation
                                    ///<   (range 0..1000, default 10).
  };

  /// Apply a full cell-configuration block in a single CFGUPDATE session.
  /// Atomic: either every field is updated, or none is (if any step fails the
  /// chip stays on its pre-write values, since the BlockDataChecksum commit
  /// is the last step).  Idempotent: if all fields already match the
  /// requested values, no CFGUPDATE is entered (Qmax learning is preserved).
  /// @return true on success or when no change was needed.
  bool configure_cell(const CellConfig &cfg);

  /// Issue `Control(RESET = 0x0041)` from inside CFGUPDATE.  Per TRM §5.1.16
  /// this performs a full device reset and reloads all RAM data memory from
  /// the chip's ROM defaults.  Used to recover when data memory has been
  /// corrupted by a partial / aborted CFGUPDATE write.
  bool reset_to_factory_defaults();

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

  /// Read a single 8-bit register.  Used for Data Memory block access.
  bool _read_byte(uint8_t reg, uint8_t &out);

  /// Write a single 8-bit register.
  bool _write_byte(uint8_t reg, uint8_t value);

  /// Read `len` bytes starting at `reg` (auto-increment) in one transaction.
  bool _read_block(uint8_t reg, uint8_t *buf, size_t len);

  /// Write `len` bytes starting at `reg` (auto-increment) in one transaction.
  bool _write_block(uint8_t reg, const uint8_t *buf, size_t len);

  /// Set up the data memory block window: select subclass, block offset,
  /// and clear BlockDataControl so the chip honours raw block I/O.
  bool _select_data_block(uint8_t subclass, uint8_t block_offset);

  /// Poll the Flags() register until the CFGUPDATE bit (bit 4) matches
  /// `expected_set`.  Returns false on timeout.
  bool _wait_cfgupdate_flag(bool expected_set, uint32_t timeout_ms);

  /// Send the UNSEAL sequence (key `0x8000` to Control() twice, per TRM
  /// §7.1.3).  No-op if the chip is already UNSEALED.  Required before any
  /// Data Memory write because BlockDataChecksum (0x60) is "UNSEALED Access"
  /// per TRM §6.4.
  bool _unseal();
};

#endif // BQ27427_H
