# Board v1 BMS Support Spec

> **This is a spec.** It describes how v1-only BMS support will be
> built, not what currently exists. Once shipped, the corresponding
> component / service docs become the source of truth and this spec is
> deleted.

Add v1-only BMS behaviors that come online when the board carries a
BQ27427 fuel gauge. Specifically: a new BQ27427 driver, boot-time
fuel-gauge bring-up (corruption recovery + idempotent cell-config
write) in `GoHardwareBoard::init_bms()`, `PowerService` attaches the
fuel gauge for runtime use, the existing `poll_bms()` prefers FG-derived
SOC and surfaces FG telemetry, and the orchestrator's BMS poll cadence
shortens from 60 s to 30 s for both boards. Every behavior is gated on
`board.variant() == BoardVariant::V1`. The prototype path is
bit-identical to today.

## Problem

- v1 silicon adds a TI BQ27427 Impedance-Track fuel gauge at fixed I²C
  address `0x55`. Today's firmware has no driver. SOC on v1 would
  silently fall back to the BQ25629 voltage estimate — coarse,
  inaccurate, and inverts near the SOC extremes
- The BQ27427 has product-level bring-up needs the driver cannot
  satisfy on its own:
  - The chip stores cell-specific parameters (Design Capacity, Design
    Energy, Terminate Voltage, Sleep Current) that need to match our
    cell. Writing them every boot would wipe the chip's learned Qmax /
    Ra impedance table that takes weeks of real-world cycling to
    build. The product must compare before writing
  - A previous aborted CFGUPDATE write can leave the chip with garbage
    persistent state (e.g. Design Capacity = `0x00D0` = 208 mAh,
    FullChargeCapacity in the tens of thousands). The product must
    detect this at boot and reset the chip to factory defaults before
    applying cell config — otherwise the gauge reports nonsense
    forever
- The runtime poll path in `PowerService::poll_bms()` has no FG
  awareness today: no FG read, no SOC source preference, no `src=`
  marker in the log line. Field diagnostics on v1 would be unable to
  tell whether a reported percentage came from the FG or the BQ25629
  fallback
- The orchestrator's BMS poll cadence of 60 s leaves too long a gap
  between telemetry snapshots during bring-up and field debugging

## Goals

- Single firmware binary supports both boards. The v1 path activates
  only when `board.variant() == BoardVariant::V1`
- BQ27427 driver lives in `components/airgradient-bms/drivers/bq27427/`
  and follows the same code-style and error-handling conventions as
  the existing `BQ25629Bms` (constructor takes I²C bus + `Config`,
  `bool init()`, methods return `bool`, file-local `TAG`,
  `esp_log.h` for driver-layer logging, `RTOS::delay_ms` for delays)
- Product-level bring-up logic — corruption-recovery decision and
  idempotent cell-config decision — lives in `GoHardwareBoard` as a
  pure helper (`evaluate_fg_state`) that is host-testable in
  isolation. The driver remains a thin chip-primitive layer
- `PowerService` learns to consume the fuel gauge through a single
  setter (`set_fuel_gauge(FuelGaugeDevice *)`). It does not own bring-up
  logic — just runtime usage
- `poll_bms()` reads FG telemetry into the existing `PowerSnapshot`,
  prefers FG SOC, falls back to BQ25629 voltage-curve estimate on
  read failure, and tags the log line with `src=FG|BMS`
- Orchestrator shortens `BMS_POLL_INTERVAL_MS` from 60 s to 30 s for
  both boards — the shorter cadence benefits FG telemetry on V1 and
  has negligible cost on Prototype
- No host tests for the BQ27427 driver itself (bus protocol —
  HIL-tested). Host tests cover only the product-side pure logic

## Non-Goals

- This spec does **not** add SHT40, LIS2DH12, TCA9536, LP5036, or the
  BuzzerService — each becomes its own future spec consuming
  `board.variant()`
- It does **not** wire any UX — Charge Cutoff toggle, admin Charge
  Current row, Battery Learning Mode, charge-done UX. Those are later
  checkpoints that consume FG telemetry via `PowerService::poll_bms`
- It does **not** ship a `.gg` golden-image export workflow or
  bqStudio integration. The chip's learned state is preserved across
  reboots by the idempotent cell-config path; manual factory-image
  baking is out of firmware scope
- It does **not** persist FG state to NVS or RTC memory. The BQ27427
  stores its own learned Qmax / Ra in its on-chip RAM; firmware does
  not snapshot it
- It does **not** add an FG-driven safety trip. The EDV cutoff
  introduced by spec 1 (`bms_cross_board_improvements.md`) uses the
  BQ25629's `battery_voltage`, which is available on both boards and
  continues to be the trip source on v1. The FG-reported voltage is
  corroboratory only
- It does **not** migrate other drivers (SPS30, SGP41, SCD4x, …) to
  the new `ag_log.h` or any other style convention. The new BQ27427
  driver follows existing `BQ25629Bms` style as-is

## Dependencies

**Depends on (hard prerequisite):**
`board_v1_pm_polarity.md` (spec 2). Every behavior in this spec is
gated on `board.variant()`, which spec 2 introduces. Spec 3 cannot
merge before spec 2.

**Does not depend on spec 1.** Spec 3's FG additions are orthogonal
to spec 1's BMS HAL extensions, safety trips, and PMID rewrite:
spec 3 does not call `set_pmid_enabled`, `set_charge_enable`,
`set_charge_current_ma`, `set_watchdog_timeout_ms`, or any of the
new trip paths. It only adds `set_fuel_gauge(FuelGaugeDevice*)` to
`PowerService` (independent of spec 1's `PowerService` edits), reads
from the `BmsDevice` HAL surface that exists today
(`get_battery_percentage`), and adds the orchestrator cadence gate.
Spec 3 could technically land before spec 1, but in practice we
recommend after.

**Recommended merge order:** spec 2 → spec 1 → spec 3. Spec 2 first
because spec 1 reads its `pm_power_on_level` field. Spec 1 second
because it touches `PowerService` more broadly and landing it before
spec 3 reduces rebase surface in `go_power.{h,cpp}`. Spec 3 last for
the same reason — but not because of a hard code dependency on
spec 1.

**Will be consumed by:** later UX checkpoints (Charge Cutoff toggle,
admin Charge Current, Battery Learning Mode, charge-done UX,
e-paper power dashboard). Each reads FG telemetry through
`PowerSnapshot` fields landed by this spec.

## Design

**Source-code attribution rule.** All code sketches in this spec are
deliberately free of references to the v0.3 hardware partner's branch
(`tmp/airgradient-firmware/`) and to this spec document itself. Any
provenance, validation history, or cross-spec coordination context
belongs in the surrounding prose and the References section at the
bottom of this spec — **never** in source-file comments. Implementers
must not paste partner-branch paths, spec filenames, or section
references into `.h` / `.cpp` comments while implementing this spec.

### HAL interface — `FuelGaugeDevice`

To keep `PowerService` host-testable without linking the real BQ27427,
we introduce a small abstract interface in the BMS HAL layer parallel
to `BmsDevice`. Only the **runtime-poll** API needs polymorphism;
boot-time Data Memory access stays on the concrete `BQ27427` class
(only `GoHardwareBoard::init_bms` calls those, no test surface
needed).

```cpp
// components/airgradient-bms/hal/fuel_gauge_device.h
class FuelGaugeDevice {
public:
  virtual ~FuelGaugeDevice() = default;

  /// True when the chip has been successfully attached and identified.
  virtual bool ready() const = 0;

  // -- Runtime poll surface (used by PowerService::poll_bms) ---------
  virtual bool read_soc_percent(uint8_t &out) = 0;
  virtual bool read_voltage_mv(uint16_t &out) = 0;
  virtual bool read_average_current_ma(int16_t &out) = 0;
  virtual bool read_average_power_mw(int16_t &out) = 0;
  virtual bool read_remaining_capacity_mah(uint16_t &out) = 0;
  virtual bool read_full_charge_capacity_mah(uint16_t &out) = 0;
  virtual bool read_internal_temperature_c(float &out) = 0;
  virtual bool read_flags(uint16_t &out) = 0;
};
```

`PowerService` stores a `FuelGaugeDevice *_fg`. Tests substitute a
hand-written subclass with scripted return values, the same way they
substitute `BmsDevice`.

### Driver — `BQ27427`

The driver is a thin chip-primitive layer that **implements**
`FuelGaugeDevice` for the runtime surface and exposes additional Data
Memory operations as non-virtual methods on the concrete class. It
owns the bus protocol, the chip-specific access constraints
(BlockData starting at register `0x40`, UNSEAL before any DM write,
the per-command 2 Hz polling cap and `t(BUF)` inter-packet gap
discussed below), and the device-type verification on `init()`. It
does **not** know what cell the product uses, what counts as a
corrupted value, or whether `write_cell_config` should run.

Code style follows the existing `BQ25629Bms` driver verbatim:
constructor takes the I²C bus handle and a `Config`, `bool init()`
returns success, all public methods return `bool`, file-local
`static constexpr const char *TAG = "BQ27427";`, `esp_log.h` for
logging, `RTOS::delay_ms` for any delay. Header location:
`components/airgradient-bms/drivers/bq27427/bq27427.h`.

```cpp
#include "hal/fuel_gauge_device.h"
#include "types/bms_types.h"  // for FgCellConfig

class BQ27427 : public FuelGaugeDevice {
public:
  /// Fixed 7-bit I²C address.
  static constexpr uint8_t DEFAULT_ADDRESS = 0x55;

  /// Expected response from Control(DEVICE_TYPE) used to confirm part.
  static constexpr uint16_t DEVICE_TYPE_BQ27427 = 0x0427;

  struct Config {
    uint8_t address = DEFAULT_ADDRESS;
    uint32_t scl_speed_hz = 400000;
    int timeout_ms = 100;
  };

  // NOTE: the cell-configuration struct lives in
  // `components/airgradient-bms/types/bms_types.h` as `FgCellConfig`
  // so the host-side `evaluate_fg_state` helper (declared in
  // `products/go/main/go_board.h`) can use it without pulling in
  // this driver header.

  explicit BQ27427(i2c_master_bus_handle_t bus);
  BQ27427(i2c_master_bus_handle_t bus, const Config &config);
  ~BQ27427() override;

  BQ27427(const BQ27427 &) = delete;
  BQ27427 &operator=(const BQ27427 &) = delete;

  /// Probe, attach to the bus, verify Control(DEVICE_TYPE) == 0x0427.
  /// Idempotent.  @return true on success.
  bool init();

  // -- FuelGaugeDevice (runtime poll surface) ------------------------
  bool ready() const override;
  bool read_soc_percent(uint8_t &out) override;              // 0x1C
  bool read_voltage_mv(uint16_t &out) override;              // 0x04
  bool read_average_current_ma(int16_t &out) override;       // 0x10
  bool read_average_power_mw(int16_t &out) override;         // 0x18
  bool read_remaining_capacity_mah(uint16_t &out) override;  // 0x2A
  bool read_full_charge_capacity_mah(uint16_t &out) override;// 0x2E
  bool read_internal_temperature_c(float &out) override;     // 0x1E (0.1 K raw)
  bool read_flags(uint16_t &out) override;                   // 0x06

  // -- Control() subcommand (non-virtual; concrete class only) -------
  bool control_subcommand(uint16_t subcmd, uint16_t &result);

  // -- Data Memory (non-virtual; concrete class only) ----------------
  /// DM read; does not enter CFGUPDATE; does not perturb learned state.
  bool read_design_capacity_mah(uint16_t &out);

  /// DM read of the full 4-field FgCellConfig block.  Same property
  /// as read_design_capacity_mah — non-perturbing.
  bool read_cell_config(FgCellConfig &out);

  /// Atomic write of all four FgCellConfig fields in a single
  /// CFGUPDATE session.  ALWAYS enters CFGUPDATE — caller is
  /// responsible for only invoking this when the persisted values
  /// actually differ from the desired target (see `evaluate_fg_state`
  /// in `go_board.h` for the gating decision).
  bool write_cell_config(const FgCellConfig &cfg);

  /// Issue Control(RESET = 0x0041).  Full device reset; reloads all
  /// RAM data memory from chip ROM defaults.  Used to recover from
  /// corrupted persistent state.
  bool reset_to_factory_defaults();

private:
  // ... internal helpers: _read_word, _write_word, _read_block,
  // _write_block, _select_data_block, _wait_cfgupdate_flag, _unseal ...
};
```

Notes the driver enforces internally — not visible to callers:

- BlockData reads start at register `0x40` exactly. Reads starting at
  higher offsets silently return zeros; the driver hides this from
  consumers by always starting block I/O at `0x40` (TRM §4.1 / §6.4).
- UNSEAL (`Control(0x8000)` twice) runs before every DM write because
  `BlockDataChecksum (0x60)` requires UNSEALED access. Idempotent — a
  no-op when the chip is already UNSEALED (TRM §7.1.3).
- **BQ27427 polling constraints (TRM §6.3.1.3).** Three things to
  honour, none of which require driver-side throttling at our chosen
  cadence:
  - **Per-command 2 Hz cap on Standard Commands.** "The host must not
    issue any standard command more than two times per second"
    (verbatim from the TRM). This is **per-command**, not aggregate —
    reading 8 different Standard Commands back-to-back in a single
    poll is fine, only re-polling the same command faster than twice
    per second is forbidden. At our 30 s outer cadence, each command
    runs at 0.033 Hz — ~60× below the per-command ceiling.
  - **`t(BUF) ≥ 66 µs` inter-packet bus-free time at 400 kHz.** The
    ESP-IDF I²C driver's transaction framing already inserts a small
    gap between `i2c_master_transmit` / `i2c_master_receive` calls;
    no additional sleep needed in the BQ27427 driver. Verify on
    bench during bring-up.
  - **2-second wait between Control() subcommand write and result
    read.** Applies to read-write subcommands and any
    `Control()`-driven status read. We use this pattern only at
    `init()` (`Control(DEVICE_TYPE)`); runtime polling never issues
    read-write commands.
- **I²C traffic budget — caveat for both BQ27427 and BQ25629.** Even
  though no per-command throttling is mandated by the TRM, the shared
  I²C bus carries traffic for every peripheral (BMS, FG on v1, SHT40
  on v1, SCD4x, SGP41, DPS368, CAP1203, etc.). Bursting 8 + N reads
  per poll without any inter-command spacing has been observed by the
  partner branch and at least appears to work, but if a future
  hardware issue (bus contention, signal integrity at 400 kHz) makes
  the burst pattern problematic, the fix is to add a small
  `RTOS::delay_ms` between commands inside `poll_bms()`. Not done
  pre-emptively — flagged for the implementer to revisit if bench
  measurements show the burst is too aggressive.
- `Control(DEVICE_TYPE) == 0x0427` is checked inside `init()`. On
  mismatch, `init()` returns false and `ready()` continues to return
  false.

### Product-level bring-up — `evaluate_fg_state`

A pure helper that looks at the FG's current persistent state and
decides what to do. Lives **inline in `products/go/main/go_board.h`**
— host-test-friendly (the header has no ESP-IDF dependencies), zero
new files. `BQ27427`'s `CellConfig` struct **also moves** to a
host-safe location, `components/airgradient-bms/types/bms_types.h`,
renamed to `FgCellConfig`, so the helper's signature does not need
to include `bq27427.h` (which is target-only).

```cpp
// components/airgradient-bms/types/bms_types.h — addition

/// Fuel-gauge cell configuration block.  Used by the BQ27427 driver
/// (`read_cell_config` / `write_cell_config`) and by the host-side
/// `evaluate_fg_state` helper.  Lives here (not in the driver
/// header) so host tests can use it without pulling in ESP-IDF.
struct FgCellConfig {
  uint16_t design_capacity_mah;
  uint16_t design_energy_mwh;
  uint16_t terminate_voltage_mv;
  uint16_t sleep_current_ma;
};

inline bool operator==(const FgCellConfig &a, const FgCellConfig &b) {
  return a.design_capacity_mah  == b.design_capacity_mah
      && a.design_energy_mwh    == b.design_energy_mwh
      && a.terminate_voltage_mv == b.terminate_voltage_mv
      && a.sleep_current_ma     == b.sleep_current_ma;
}
inline bool operator!=(const FgCellConfig &a, const FgCellConfig &b) {
  return !(a == b);
}
```

```cpp
// products/go/main/go_board.h — additions, after BoardVariant

struct FgRecoveryDecision {
  bool needs_factory_reset; ///< True when DC or FCC is out of range
  bool needs_config_write;  ///< True when current FgCellConfig differs
                            ///< from target
};

/// Decide whether the BQ27427 needs a factory reset and / or a fresh
/// cell-config write based on what it currently has in persistent
/// memory.  Pure function, header-only so host tests can link it
/// without an extra translation unit.
///
/// Each input value is paired with a `_ok` validity flag so the
/// helper can distinguish "I read this and it's bad" from "I could
/// not read this at all".  Transient I²C errors must NOT trigger a
/// destructive reset / config-write — only confidently-observed
/// corruption does.
///
/// Sanity ranges and target FgCellConfig are passed in so the helper
/// has no implicit dependency on file-scope constants and so tests
/// can exercise the full decision space.
inline FgRecoveryDecision evaluate_fg_state(
    uint16_t current_design_capacity_mah,         bool dc_ok,
    uint16_t current_full_charge_capacity_mah,    bool fcc_ok,
    const FgCellConfig &current_cell_config,      bool cfg_ok,
    const FgCellConfig &target_cell_config,
    uint16_t dc_sanity_min_mah,
    uint16_t dc_sanity_max_mah,
    uint16_t fcc_sanity_max_mah) {
  FgRecoveryDecision out{false, false};
  const bool dc_corrupt  = dc_ok  && (current_design_capacity_mah < dc_sanity_min_mah
                                   || current_design_capacity_mah > dc_sanity_max_mah);
  const bool fcc_corrupt = fcc_ok && (current_full_charge_capacity_mah > fcc_sanity_max_mah);
  out.needs_factory_reset = dc_corrupt || fcc_corrupt;
  out.needs_config_write  = cfg_ok && (current_cell_config != target_cell_config);
  return out;
}
```

Decision rules (in plain English; the implementation is a few
`if`-statements):

- `needs_factory_reset = true` only when a DM read **succeeded** AND
  the value is out of range. Specifically:
  - `(dc_ok  AND (dc < dc_sanity_min_mah OR dc > dc_sanity_max_mah))`
  - OR `(fcc_ok AND fcc > fcc_sanity_max_mah)`
  - If a read failed (`_ok == false`), the corresponding check
    short-circuits to "no evidence of corruption" — we will not
    trigger a destructive reset on a transient I²C error
- `needs_config_write = true` only when `cfg_ok` AND any of the four
  target fields differs from the current value. If we couldn't read
  the cell config, we **do not** write a new one — the existing
  chip state is preserved
- Special-case: when `needs_factory_reset == true`, the caller
  re-reads the cell config after performing the reset; this re-read
  becomes the new `current_cell_config` input on a follow-up call to
  `evaluate_fg_state`, which will then correctly flag
  `needs_config_write` against the freshly-defaulted ROM values

The "post-reset re-read" pattern is what the caller in
`GoHardwareBoard::init_bms()` does: call `evaluate_fg_state`, perform
`reset_to_factory_defaults()` if requested, then re-read DC / FCC /
cell config and call `evaluate_fg_state` once more to decide the
config write. Two passes maximum, no recursion.

File-scope constants in `go_hardware_board.cpp` carry the actual
numbers.

**Constant provenance** (spec context, deliberately kept out of the
source comments): both `kAgoCellConfig` (DC=2000 mAh, DE=7400 mWh,
TermV=3000 mV, SleepI=50 mA) and the FG DM sanity ranges
(`FG_DC_SANITY_MIN_MAH = 500`, `FG_DC_SANITY_MAX_MAH = 8000`,
`FG_FCC_SANITY_MAX_MAH = 8500`) are inherited from the v0.3 hardware
partner's branch
(`tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp`
in the `init_bms()` block), where they have been validated on
hardware against AGo's single-cell 2000 mAh Li-ion pack. Code
comments below intentionally describe what each constant **does**
without naming external code locations — see the References section
at the bottom of this spec for partner-branch cross-references.

```cpp
// products/go/main/go_hardware_board.cpp — file-scope constants

static constexpr FgCellConfig kAgoCellConfig = {
    .design_capacity_mah  = 2000,
    .design_energy_mwh    = 7400,
    .terminate_voltage_mv = 3000,
    .sleep_current_ma     = 50,
};

// FG DM corruption sanity ranges.  A reading outside any of these
// ranges is treated as evidence of a corrupted persistent block
// (most commonly a prior aborted CFGUPDATE).  Validated on hardware
// against AGo's single-cell 2000 mAh Li-ion pack.  Revisit if cell
// sourcing changes.
static constexpr uint16_t FG_DC_SANITY_MIN_MAH  = 500;
static constexpr uint16_t FG_DC_SANITY_MAX_MAH  = 8000;
static constexpr uint16_t FG_FCC_SANITY_MAX_MAH = 8500;
```

### Boot-time integration — `init_bms()`

`GoHardwareBoard::init_bms()` gains a V1 branch that constructs the
fuel gauge, reads its current state, runs `evaluate_fg_state`, and
applies whatever recovery / write the helper recommends. The
BQ25629-side init is unchanged.

Sequence on V1:

```cpp
// products/go/main/go_hardware_board.cpp — init_bms() additions
// (sketch; final code follows existing style)

if (_variant == BoardVariant::V1) {
  _fuel_gauge = new BQ27427(_i2c_bus);
  if (!_fuel_gauge->init()) {
    AG_LOGE(TAG, "BQ27427 init failed — FG offline");
    // Continue: _fuel_gauge stays non-null but ready() == false.
    // Caller checks _fuel_gauge->ready() before attaching to
    // PowerService.
  } else {
    // Pass 1: read state with validity flags; never overwrite values
    // with zero on a failed read — that was a footgun that would
    // make evaluate_fg_state recommend a destructive reset on a
    // transient I²C error.
    uint16_t dc = 0, fcc = 0;
    FgCellConfig current{};
    const bool dc_ok  = _fuel_gauge->read_design_capacity_mah(dc);
    const bool fcc_ok = _fuel_gauge->read_full_charge_capacity_mah(fcc);
    const bool cfg_ok = _fuel_gauge->read_cell_config(current);

    FgRecoveryDecision decision = evaluate_fg_state(
        dc, dc_ok, fcc, fcc_ok, current, cfg_ok,
        kAgoCellConfig,
        FG_DC_SANITY_MIN_MAH, FG_DC_SANITY_MAX_MAH,
        FG_FCC_SANITY_MAX_MAH);

    // Tracks whether `current` reflects a successful CellConfig read
    // from the chip.  Updated by the pass-2 re-read below if reset
    // succeeded.  Used by the post-write logging branch.
    bool cfg_current_ok = cfg_ok;

    if (decision.needs_factory_reset) {
      AG_LOGW(TAG, "BQ27427 corrupted state (dc=%u fcc=%u dc_ok=%d fcc_ok=%d) — resetting",
              dc, fcc, dc_ok, fcc_ok);
      if (!_fuel_gauge->reset_to_factory_defaults()) {
        AG_LOGE(TAG, "BQ27427 reset_to_factory_defaults() failed — "
                     "FG may be in inconsistent state; skipping config write");
        // Explicit clear so the fall-through to needs_config_write
        // below cannot act on the stale pass-1 decision (which may
        // have been "yes, write" against a chip that just failed to
        // reset).  Boot continues without applying our cell config;
        // runtime polling will still produce SOC reads from whatever
        // the chip currently has.
        decision = {false, false};
      } else {
        // Pass 2: post-reset, the chip now holds ROM defaults.
        // Re-read to drive needs_config_write against the fresh state.
        const bool dc2_ok  = _fuel_gauge->read_design_capacity_mah(dc);
        const bool fcc2_ok = _fuel_gauge->read_full_charge_capacity_mah(fcc);
        const bool cfg2_ok = _fuel_gauge->read_cell_config(current);
        cfg_current_ok = cfg2_ok;
        decision = evaluate_fg_state(
            dc, dc2_ok, fcc, fcc2_ok, current, cfg2_ok,
            kAgoCellConfig,
            FG_DC_SANITY_MIN_MAH, FG_DC_SANITY_MAX_MAH,
            FG_FCC_SANITY_MAX_MAH);
      }
    }

    if (decision.needs_config_write) {
      AG_LOGI(TAG, "BQ27427 applying cell config");
      if (!_fuel_gauge->write_cell_config(kAgoCellConfig)) {
        AG_LOGW(TAG, "BQ27427 write_cell_config() failed — cell parameters "
                     "not updated; runtime polling continues with whatever "
                     "the chip currently has");
      }
    } else if (cfg_current_ok) {
      AG_LOGI(TAG, "BQ27427 cell config already correct — preserved");
    } else {
      AG_LOGW(TAG, "BQ27427 cell config unreadable — left as-is");
    }

    // One-shot diagnostic snapshot.
    uint8_t soc = 0; uint16_t mv = 0; int16_t ma = 0; float tc = 0.0f;
    _fuel_gauge->read_soc_percent(soc);
    _fuel_gauge->read_voltage_mv(mv);
    _fuel_gauge->read_average_current_ma(ma);
    _fuel_gauge->read_internal_temperature_c(tc);
    AG_LOGI(TAG, "BQ27427 boot: soc=%u%% v=%umV i=%dmA t=%.1fC",
            soc, mv, ma, tc);
  }
}
```

The "cell config already correct — preserved" log line is the field
signal that the idempotent path saved the chip's learned Qmax / Ra
across this boot.

### `PowerService` integration

`PowerService` gains a single setter, a non-owning pointer field, and
FG awareness inside `poll_bms()`. No bring-up logic — that's done by
the time `set_fuel_gauge()` is called.

```cpp
// products/go/main/go_power.h — additions

class PowerService {
public:
  /// Attach an already-initialised fuel gauge for runtime use.
  /// Non-owning: the fuel gauge must outlive PowerService.
  /// Pass nullptr (or skip the call entirely) on prototype boards.
  void set_fuel_gauge(FuelGaugeDevice *fg);

  // ... existing API ...

private:
  FuelGaugeDevice *_fg = nullptr;
  // ... existing members ...
};
```

`PowerSnapshot` gains FG telemetry fields. Existing fields keep their
meaning — `battery_percentage` is still the canonical SOC value; the
FG-specific fields carry additional detail for display and logging.

```cpp
// products/go/main/go_power.h — PowerSnapshot additions

enum class BatteryPercentSource : uint8_t {
  Unknown,
  FuelGauge,      ///< Read from BQ27427 (V1 with FG attached, read OK)
  BatteryCharger, ///< Voltage-curve estimate from BQ25629 (fallback)
};

const char *bms_battery_percent_source_str(BatteryPercentSource s);

struct PowerSnapshot {
  // ... existing fields ...

  BatteryPercentSource battery_percent_source = BatteryPercentSource::Unknown;

  // FG snapshot (populated only when an FG is attached and the read
  // succeeded).  Invalid sentinels otherwise.
  uint8_t  fg_soc_percent           = BmsInvalid::SOC_PERCENT;
  uint16_t fg_voltage_mv            = BmsInvalid::VOLTAGE_MV;
  int16_t  fg_current_ma            = BmsInvalid::CURRENT_MA;
  int16_t  fg_power_mw              = BmsInvalid::POWER_MW;
  uint16_t fg_remaining_capacity_mah = BmsInvalid::CAPACITY_MAH;
  uint16_t fg_full_charge_capacity_mah = BmsInvalid::CAPACITY_MAH;
  float    fg_internal_temperature_c = BmsInvalid::FG_TEMP_C;
  uint16_t fg_flags                 = 0;
};
```

The sentinel additions go into
`components/airgradient-bms/types/bms_types.h` alongside the existing
ones:

```cpp
namespace BmsInvalid {
  // ... existing ...
  static constexpr uint8_t  SOC_PERCENT = 255;       // SOC is 0..100
  static constexpr int16_t  POWER_MW    = INT16_MIN; // signed power
  static constexpr uint16_t CAPACITY_MAH = UINT16_MAX;
  static constexpr float    FG_TEMP_C   = -273.16f;  // below abs zero
}
```

Distinct sentinels matter because the partner's reuse of
`BmsInvalid::PERCENT (-1.0f)` for `fg_internal_temperature_c` would
collide with valid sub-zero ambient temperatures (cold storage,
outdoor use), and `BmsInvalid::CURRENT_MA` for `fg_power_mw` is a
semantic mismatch (power is not current). Each new sentinel is
unambiguous within its field's value domain.

`poll_bms()` reads the FG snapshot before deciding SOC source:

```cpp
// products/go/main/go_power.cpp — poll_bms() additions (sketch)

PowerSnapshot PowerService::poll_bms() {
  PowerSnapshot status{};

  // ... existing BMS telemetry / status reads ...

  // FG snapshot (V1 path).  Reads are independent; partial failures
  // leave individual fields at their invalid sentinels.
  bool fg_soc_ok = false;
  uint8_t fg_soc = BmsInvalid::SOC_PERCENT;
  if (_fg != nullptr && _fg->ready()) {
    fg_soc_ok = _fg->read_soc_percent(fg_soc);
    if (fg_soc_ok) {
      status.fg_soc_percent = fg_soc;
    }
    _fg->read_voltage_mv(status.fg_voltage_mv);
    _fg->read_average_current_ma(status.fg_current_ma);
    _fg->read_average_power_mw(status.fg_power_mw);
    _fg->read_remaining_capacity_mah(status.fg_remaining_capacity_mah);
    _fg->read_full_charge_capacity_mah(status.fg_full_charge_capacity_mah);
    _fg->read_internal_temperature_c(status.fg_internal_temperature_c);
    _fg->read_flags(status.fg_flags);
  }

  // SOC source preference: FG first; BQ25629 voltage estimate fallback.
  if (fg_soc_ok) {
    status.battery_percentage    = static_cast<float>(fg_soc);
    status.battery_percent_source = BatteryPercentSource::FuelGauge;
  } else {
    float pct = -1.0f;
    if (_bms.get_battery_percentage(&pct)) {
      status.battery_percentage    = pct;
      status.battery_percent_source = BatteryPercentSource::BatteryCharger;
    }
  }
  status.critical = (status.battery_percentage >= 0.0f &&
                     status.battery_percentage < BATTERY_CRITICAL_PERCENT);

  // ... existing log line, with src= marker appended ...
  AG_LOGI(TAG,
          "poll_bms: perc=%.1f%% src=%s vbat=%.1fV ...",
          status.battery_percentage,
          bms_battery_percent_source_str(status.battery_percent_source),
          status.battery_voltage /* ... */);

  return status;
}
```

Wiring in `GoHardwareBoard::power()`:

```cpp
PowerService &GoHardwareBoard::power() {
  if (!_power) {
    _power = new PowerService(*_bms_driver, gpio::native::hal, { /* ... */ });
    _power->init_ext_watchdog();
    if (_fuel_gauge != nullptr && _fuel_gauge->ready()) {
      _power->set_fuel_gauge(_fuel_gauge);
    }
  }
  return *_power;
}
```

### Orchestrator cadence

`BMS_POLL_INTERVAL_MS` changes from 60 s to 30 s for both boards.
The shorter cadence benefits FG telemetry freshness on V1 and has
negligible cost on Prototype (one extra I2C burst per minute).

```cpp
// products/go/main/go_orchestrator.h
static constexpr uint32_t BMS_POLL_INTERVAL_MS = 30000;
```

No variant gating, no instance member, no constructor change.
The `BMS_STATUS_POLL_INTERVAL_MS` constant (5 s, spec 1's
responsibility) is **not** changed by this spec.

### What this spec does not change

- `BmsDevice` HAL surface — unchanged. Spec 1 owns those additions
- `BMS_STATUS_POLL_INTERVAL_MS` — unchanged. Spec 1 owns it
- EDV / OT ship-mode trips — unchanged. They use BQ25629 voltage /
  battery_temperature_c, which are present on both boards. Spec 1
  owns them
- Charging defaults (`BQ25629_Config`) — unchanged
- Sleep / wake / `set_pm_power` polarity — owned by specs 1 and 2

## Implementation Plan

Each step is a focused commit.

1. **New `FuelGaugeDevice` HAL interface, `FgCellConfig` type, and
   `BQ27427` driver skeleton** —
   - Add `struct FgCellConfig` (+ `==` / `!=` operators) to
     `components/airgradient-bms/types/bms_types.h` (host-safe — no
     ESP-IDF includes)
   - Create `components/airgradient-bms/hal/fuel_gauge_device.h` with
     the abstract interface (eight `read_*` pure virtuals + `ready()`)
   - Create `bq27427.h` and `bq27427.cpp` with `class BQ27427 : public
     FuelGaugeDevice`, constructor, destructor, `init()`, `ready()`
     declarations and stub bodies. `bq27427.h` includes
     `types/bms_types.h` for `FgCellConfig`
   - Add the new sources to `components/airgradient-bms/CMakeLists.txt`
   - Builds clean, no behavior change
2. **Standard Command reads** — implement `_read_word` plus the eight
   Standard Command read methods (`read_soc_percent`,
   `read_voltage_mv`, `read_average_current_ma`,
   `read_average_power_mw`, `read_remaining_capacity_mah`,
   `read_full_charge_capacity_mah`, `read_internal_temperature_c`,
   `read_flags`). No caller yet.
3. **`Control()` subcommand + DEVICE_TYPE verify in `init()`** —
   implement `control_subcommand` and have `init()` confirm
   `Control(DEVICE_TYPE) == 0x0427`. On mismatch, `init()` returns
   false and `ready()` stays false.
4. **Data Memory plumbing** — UNSEAL helper, BlockData read/write
   starting at `0x40`, `_select_data_block`, `_wait_cfgupdate_flag`.
   Pure internal helpers.
5. **DM read APIs** — `read_design_capacity_mah` and
   `read_cell_config`. Non-perturbing reads (no CFGUPDATE entry).
6. **DM write API + factory reset** — `write_cell_config` (full
   CFGUPDATE session, always writes), `reset_to_factory_defaults`
   (`Control(0x0041)` and recovery confirmation). Self-contained — no
   integration yet.
7. **`evaluate_fg_state` helper + product constants** —
   - Declare and **define inline** `FgRecoveryDecision` and
     `evaluate_fg_state` (with `_ok` validity flags on each input
     value) in `products/go/main/go_board.h`. Header-only — no
     separate `.cpp`. `go_board.h` stays host-test-safe (it includes
     only `types/bms_types.h` for `FgCellConfig`)
   - Add `kAgoCellConfig`, `FG_DC_SANITY_MIN_MAH`,
     `FG_DC_SANITY_MAX_MAH`, `FG_FCC_SANITY_MAX_MAH` as file-scope
     constants in `products/go/main/go_hardware_board.cpp`. Code
     comments describe what each constant **does** without naming
     external code locations — the partner-branch attribution lives
     in this spec's prose and References section, not in the source
8. **`init_bms()` V1 branch** — construct `BQ27427`, run the helper
   (with the two-pass post-reset re-read pattern), apply recovery /
   write config, emit the boot snapshot log line. `_fuel_gauge` field
   added to `GoHardwareBoard`. Gated on `_variant == V1`.
9. **`PowerService::set_fuel_gauge(FuelGaugeDevice*)` + `_fg` field**
   — pointer setter, non-owning, takes the abstract interface so
   tests can substitute. No `poll_bms` change yet.
10. **`PowerSnapshot` FG fields + `BatteryPercentSource` enum +
    `BmsInvalid` sentinels** — extend the struct, add the enum and
    its `_str()` helper, add `BmsInvalid::SOC_PERCENT`, `POWER_MW`,
    `CAPACITY_MAH`, `FG_TEMP_C` in
    `components/airgradient-bms/types/bms_types.h`. No `poll_bms`
    change yet.
11. **`poll_bms` FG read + SOC preference + `src=` log marker** —
    read FG into the snapshot when `_fg != nullptr && _fg->ready()`,
    choose SOC source, append the marker to the log line.
12. **Wire FG attach in `GoHardwareBoard::power()`** — call
    `_power->set_fuel_gauge(_fuel_gauge)` when the FG is present and
    ready.
13. **Orchestrator cadence** — change
    `static constexpr BMS_POLL_INTERVAL_MS` from 60000 to 30000 for
    both boards. No variant gating, no instance member. Existing
    `BMS_STATUS_POLL_INTERVAL_MS` (spec 1) untouched.
14. **Host tests — extend
    `products/go/tests/go_power.tests.cpp`** —
    - `evaluate_fg_state` truth table (the helper is inline in
      `go_board.h`, which `go_power.tests.cpp` already transitively
      pulls in). No new test file
    - SOC source switching in `poll_bms` (FG attached / read OK / FG
      attached / read fails / FG absent)
    - `set_fuel_gauge` null guard
15. _(Removed — cadence is now a simple constant change, no variant
    gating to test.)_
16. **Documentation** — `components/airgradient-bms/README.md` gains
    the BQ27427 driver section (public methods, invariants, polling
    constraints per TRM §6.3.1.3);
    `products/go/main/go_power.{h,cpp}` doc comments cover the FG
    fields and SOC source marker; `products/go/README.md` "Hardware
    Notes" notes the FG-attached behavior on v1.

Step ordering keeps the firmware buildable at every commit. Steps 1–6
are additive driver work — invisible to the running system. Step 7
adds the inline product-level helper with no caller. Step 8 lights up
the v1 boot path. Steps 9–12 wire runtime FG consumption. Step 13 is
the cadence change. Steps 14–15 are tests. Step 16 is docs.

## Testing Strategy

### Host tests (must pass before merge)

- `go_power.tests.cpp::evaluate_fg_state` — truth table.
  No new test file — `go_power.tests.cpp` transitively includes
  `go_board.h` (where the helper lives inline) via its other tests.
  Cases:
  - All reads OK, DC at lower bound (== `FG_DC_SANITY_MIN_MAH`):
    not corrupted
  - All reads OK, DC just below lower bound: corrupted,
    `needs_factory_reset == true`
  - All reads OK, DC at upper bound (== `FG_DC_SANITY_MAX_MAH`):
    not corrupted
  - All reads OK, DC just above upper bound: corrupted
  - All reads OK, FCC at upper bound: not corrupted
  - All reads OK, FCC just above upper bound: corrupted
  - All reads OK, both DC and FCC corrupted: corrupted (single trip)
  - All reads OK, in-range, CellConfig matches target:
    `{ false, false }` — no action
  - All reads OK, in-range, CellConfig differs in one field:
    `{ false, true }`
  - All reads OK, DC corrupted, CellConfig matches target:
    `{ true, false }` — pass-1 result. `needs_factory_reset` is true
    because of the corrupted DC; `needs_config_write` is false
    because `current_cell_config == target_cell_config`. The caller's
    post-reset re-read (pass 2) is what later flags
    `needs_config_write` against the freshly-defaulted ROM values
  - **Post-reset (pass 2) call** — caller has just performed
    `reset_to_factory_defaults()`. Inputs: all reads OK; DC / FCC
    back in range (chip ROM defaults); `current_cell_config` is now
    the ROM-default block (which never matches AGo's target):
    `{ false, true }` — no further reset needed, but the config
    must be written
  - **`dc_ok == false`, DC value would be out-of-range:**
    `needs_factory_reset == false` — never trip on an unverified
    read
  - **`fcc_ok == false`, FCC value would be out-of-range:** same —
    `needs_factory_reset == false`
  - **`cfg_ok == false`, CellConfig differs from target:**
    `needs_config_write == false` — never overwrite based on an
    unread value
  - All three `_ok` flags false (totally flaky bus):
    `{ false, false }` — no destructive action
- `go_power.tests.cpp::set_fuel_gauge` — null-pointer guard;
  attaching twice is idempotent (second call just overwrites)
- `go_power.tests.cpp::poll_bms_soc_source`:
  - No FG attached: `src == BatteryCharger`, percentage from
    `_bms.get_battery_percentage()`; all FG-side fields stay at
    sentinels
  - FG attached, FG SOC read OK: `src == FuelGauge`, percentage from
    FG SOC
  - FG attached, FG SOC read fails but other FG reads succeed:
    `src == BatteryCharger`, percentage from BMS fallback; **only**
    `fg_soc_percent` is at its sentinel; `fg_voltage_mv`,
    `fg_current_ma`, `fg_power_mw`, `fg_remaining_capacity_mah`,
    `fg_full_charge_capacity_mah`, `fg_internal_temperature_c`,
    `fg_flags` populate independently based on whether each
    individual read returned true. This matches the design:
    `poll_bms()` issues each FG read independently, so partial
    failures only blank the failed field
  - FG attached, **all** FG reads fail: `src == BatteryCharger`,
    percentage from BMS fallback; all FG-side fields stay at
    sentinels
- _(Orchestrator cadence test removed — cadence is now a simple
  constant change from 60 s to 30 s, no variant gating to test.)_

### Mocks / stubs

- The concrete `BQ27427` class is not mocked directly. Host tests
  that exercise FG-aware code paths in `PowerService` substitute a
  `FuelGaugeDevice` implementation — same pattern as `MockBmsDevice`
  for the BMS HAL. Two valid shapes:
  - Trompeloeil `MockFuelGaugeDevice` for assertion-heavy tests
    (read call expectations, sequence guards on `poll_bms`)
  - Hand-written stub subclass that returns scripted values per test
    case for behavior-driven tests (SOC source switching, FG-attach
    null guard)
  Both live next to the existing `MockBmsDevice` in the test support
  fixtures. The concrete `BQ27427` is exercised only at hardware-in-
  the-loop time.

### ESP-IDF build

- `idf.py -C products/go build` must succeed
- No new Kconfig symbols
- No new managed components

### Hardware-in-the-loop (user-driven)

- **Prototype board:** boot log shows no `BQ27427` lines;
  `poll_bms` log shows `src=BMS`; orchestrator cadence is 30 s
  (changed from 60 s). No other behavioral difference from today
- **v1 board (when silicon is available):** boot log shows
  `BQ27427 boot: soc=X% v=YmV i=ZmA t=T°C`; subsequent boots show
  `BQ27427 cell config already correct — preserved` (idempotent
  path); `poll_bms` log shows `src=FG` with FG fields populated;
  cadence verified at 30 s
- **Corruption recovery on v1:** intentionally write a bogus Design
  Capacity via bqStudio while the board is offline (e.g.
  `DC = 208 mAh`). Reboot. Boot log must show
  `BQ27427 corrupted state (dc=208 fcc=...) — resetting`, then
  `BQ27427 applying cell config`, then the boot snapshot. Subsequent
  boots show the idempotent "preserved" line again
- **FG missing on a board that should be v1 (variant fail-safe).**
  Physically remove or cover the BQ27427 on a board that was
  manufactured as v1 — the address probe at `0x55` then NACKs and
  spec 2's variant detection classifies the board as **Prototype**
  (the spec-2 fail-safe path). The v1 init-bms branch is skipped
  entirely, so there is no `BQ27427 init failed` log; instead boot
  log shows `board variant: Prototype (BQ27427 @ 0x55 NACK)`,
  `poll_bms` shows `src=BMS`, and the device runs cleanly on the
  BQ25629 estimate. This tests the spec-2 fail-safe, not spec 3's
  internal init-failure branch (the latter requires
  `Control(DEVICE_TYPE)` returning a mismatched value, which is hard
  to provoke from intact hardware — verified via a stub build at
  factory bring-up if needed). Do **not** tie SDA low to fake the
  fault: that would wedge the entire I²C bus and prevent every
  other peripheral from responding (a different and unrelated fault)

## Open Questions

- **Q3.1.** `BatteryPercentSource` enum naming —
  `BatteryCharger` is a long label for what's really "BQ25629
  voltage-curve estimate". Acceptable as-is, or should it be shorter
  (`Bms`, `Charger`, `Voltage`)? Spec defaults to `BatteryCharger`
  for clarity in log lines
- **Q3.2.** _(Resolved.)_ Sentinels on FG fields now use dedicated
  constants (`BmsInvalid::SOC_PERCENT`, `POWER_MW`, `CAPACITY_MAH`,
  `FG_TEMP_C`) in `bms_types.h` so each field's sentinel cannot
  collide with valid values in its domain
- **Q3.3.** _(Resolved by Q3.4 fix.)_ Partial reads now leave
  `evaluate_fg_state` flags at their conservative defaults — no
  destructive action when reads couldn't confirm corruption. See
  Q3.4
- **Q3.4.** _(Resolved.)_ `evaluate_fg_state` takes explicit `_ok`
  validity flags. Transient I²C errors cannot trigger
  `needs_factory_reset` or `needs_config_write`; only confidently-
  observed corruption / mismatch does
- **Q3.5.** _(Resolved.)_ Cadence value — 30 s on V1. The partner
  branch shipped 10 s for FG bring-up log visibility; production
  doesn't need that fidelity. TRM §6.3.1.3 (verbatim): "the host
  must not issue any standard command more than two times per
  second" — per-command, not aggregate. At 30 s outer cadence each
  command is polled at 0.033 Hz, well below the per-command ceiling.
  Revisit only if field data shows the cadence too slow or too fast
- **Q3.6 — Risk: BQ27427 driver has no automated protocol tests.**
  The BQ27427 driver implements a non-trivial protocol on top of
  raw I²C: Control() subcommands with 2-second turnaround,
  BlockData reads always starting at register `0x40`, UNSEAL
  (`Control(0x8000)` twice) before any DM write, CFGUPDATE
  enter/exit with `Flags()` bit-4 polling, BlockDataChecksum (`0x60`)
  calculation and write, SOFT_RESET to exit CFGUPDATE, and the
  Control(RESET = 0x0041) factory-defaults path. Spec scope per
  earlier review excludes host tests for drivers in this codebase,
  so today the driver's protocol logic is validated only at HIL.

  **Risk acknowledged.** Bugs in this layer (endian flips, missed
  UNSEAL, wrong BlockData offset, miscalculated checksum, CFGUPDATE
  state-machine missteps) can silently corrupt the chip's persistent
  memory — particularly destructive because Qmax / Ra learning
  takes weeks of real-world cycling to rebuild and the chip's RAM
  is the only copy.

  **Mitigations in lieu of host tests:**
  - The driver follows the published TI sequences verbatim from
    BQ27427 TRM `SLUUCD5` §4 / §6 (cited in the References table)
  - The `evaluate_fg_state` idempotency invariant
    (`write_cell_config` is only called when persisted values
    actually differ) limits CFGUPDATE writes to first-boot and
    cell-config changes — a regression in the write path mostly
    bites bring-up, not field units
  - The partner branch has run this protocol on hardware and
    validated the cell-config-preserved path across reboots — we
    inherit that empirical validation
  - HIL acceptance tests (boot log shows "preserved" on second
    boot; corruption recovery exercised via bqStudio-injected
    bogus DC) cover the most consequential write paths

  **Open for follow-up.** If field experience surfaces driver
  regressions, the right response is a fake-I²C unit-test layer
  exercising at minimum: Standard Command endian mapping,
  BlockData-starts-at-0x40, UNSEAL sequence, CFGUPDATE enter/exit
  sequencing, checksum calculation, and the no-write-when-matched
  idempotency. Cost ≈ 100–200 lines of test code plus a fake I²C
  bus helper. Not done now per the spec's "no driver tests" scope.

## References

The "partner" branch referenced throughout this spec is the read-only
audit fork at `tmp/airgradient-firmware/` (branch `go/test/v0.3`,
upstream
[`Gingerman1996/airgradient-firmware`](https://github.com/Gingerman1996/airgradient-firmware)).
Paths below point at the partner's implementation. Where this spec
diverges from the partner (driver/product split, naming, code
location), the divergence is intentional — partner code is reference,
not template.

| Topic | Partner file | Notes |
|---|---|---|
| `BQ27427` driver | `tmp/airgradient-firmware/components/airgradient-bms/drivers/bq27427/bq27427.{h,cpp}` | We re-implement following our `BQ25629Bms` code style. Public surface differs: we drop the standalone `set_design_capacity_mah` (subsumed by `write_cell_config`), expose `read_cell_config`, and keep `write_cell_config` non-idempotent (the product decides whether to call it) |
| Idempotent CellConfig — partner version | `bq27427.cpp::configure_cell` | Partner does the diff inside the driver. We move it out: the driver always writes; `evaluate_fg_state` (product) decides whether to call |
| Corruption recovery — partner version | `tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp::init_bms` (the `dc_bad` / `fcc_bad` block) | Same structure; we extract the decision into `evaluate_fg_state` so it's host-testable |
| Cell config values `{2000, 7400, 3000, 50}` | `tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp` (CellConfig literal in `init_bms`) | Adopt verbatim. NOTE: validated on partner hardware |
| Sanity ranges `500..8000` / `8500` | `tmp/airgradient-firmware/products/go/main/go_hardware_board.cpp` (the same block) | Adopt verbatim. NOTE: validated on partner hardware |
| FG snapshot in `poll_bms` + SOC source marker | `tmp/airgradient-firmware/products/go/main/go_power.cpp::poll_bms` | Adopt structure |
| `BMS_POLL_INTERVAL_MS` cadence concept | `tmp/airgradient-firmware/products/go/main/go_orchestrator.h` line 151 | Partner uses 10 000 (10 s) for FG bring-up log visibility; we ship 30 000 (30 s) for both boards since production doesn't need that fidelity |
| BQ27427 `Flags()` CHG bit decode (bit 8, not bit 4) | `tmp/airgradient-firmware/components/airgradient-bms/drivers/bq27427/bq27427.cpp` | Silicon behavior, not partner-specific. Documented in driver header; consumer helpers (if any) get the bit number right |

Behavioural-source-of-truth references (not partner-specific):

- BQ27427 datasheet `SLUSEB5B` (Dec 2022, rev Sep 2025) — pin map,
  Standard Command list
- BQ27427 TRM `SLUUCD5` (Jan 2023) — §4.1 / §6 Data Memory access,
  §6.3.1.3 polling ceiling, §7.1.3 UNSEAL sequence
- AirGradient read-only audit of the partner branch:
  `go_v0_3_changes_analysis.md` at repo root, "Item 2 — Battery
  Percentage (BQ27427 Fuel Gauge)" section
