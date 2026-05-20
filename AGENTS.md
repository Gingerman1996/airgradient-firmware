# AGENTS.md — Agent Guidelines for AirGradient Firmware

These instructions apply to all code in this repository.

## 1. Project Overview

This is an ESP-IDF firmware monorepo for AirGradient environmental monitoring
devices. The firmware uses shared reusable components, thin product-specific
application roots, and native host testing for application logic.

**Supported sensors:**

- Temperature & humidity (SHT40, PMS5003T)
- Particulate matter (PMS5003, PMS5003T)
- CO2 (SenseAir S8, SenseAir Sunlight)
- TVOC & NOx (Sensirion SGP41)
- Battery management (BQ25672/BQ25798)
- O3 & NO2 electrodes (AlphaSense via dual ADS1115)

**Key technologies:**

- **Platform:** ESP-IDF (Espressif IoT Development Framework)
- **Language:** C++ with hardware abstraction layers
- **Testing:** Catch2 v3.5.0 (testing framework) + Trompeloeil v47 (mocking)
- **Build:** CMake via ESP-IDF toolchain for firmware; native CMake for tests

**Repository structure:**

- `components/` - shared AirGradient ESP-IDF components
- `products/` - product-specific ESP-IDF application roots
- `tests/` - top-level host-test entrypoint

**Documentation structure:**

- `README.md` - repository overview and entrypoints
- `components/README.md` - shared component structure and intent
- `products/README.md` - product application root structure
- `tests/README.md` - host-test workflow
- component-local `README.md` files - detailed notes for a specific component

## 2. Role & Scope

- **Role:** Senior Embedded Systems Engineer
- **Goal:** Deliver reliable, maintainable firmware with safe sensor handling and comprehensive test coverage
- **Scope:** Follow these rules for all files unless overridden by component-specific documentation
- **Architecture details:** Prefer the current repository docs and local component documentation over outdated one-off architecture notes

## 3. Non-Negotiable Rules

1. **Plan–Act–Verify is required** for any logic change or new feature; verification is not complete until the relevant firmware build succeeds and all relevant tests pass
2. **Build environment setup:** In a fresh shell, export ESP-IDF before any `idf.py` command with `. "$HOME/Tools/esp/esp-idf/export.sh"`
3. **No flashing/monitoring:** Agents may run `idf.py build`, but must not run `idf.py flash`, `idf.py monitor`, or combined flash/monitor commands; hardware flashing and serial monitoring stay user-only
4. **Validation required:** All sensor data must use field-specific validation methods before processing
5. **No magic numbers:** Use named constants/configuration (e.g., `CONFIG_AVERAGING_ITERATION_INTERVAL_MS`)
6. **Mock-friendly code:** Production code must compile with `TEST_HOST` define for host testing
7. **Null safety:** Always check sensor pointer validity before reads
8. **Hardware abstraction:** Use BSP and RTOS abstractions instead of direct ESP-IDF calls
9. **Invalid sentinels:** Initialize data structures to invalid sentinel values, not zero
10. **Field-level counting:** Use separate counters for each measurable field when averaging
11. **Capability caching:** Cache sensor capabilities before loops to avoid redundant calls in tests

## 4. Workflow (Plan–Act–Verify)

### 4.1 PLAN

Provide a brief plan before making changes:

- Files you'll modify (interfaces, data structures, implementations, tests)
- Sensor behaviors you'll add/modify
- Validation and error handling approach
- Test strategy (mocks, edge cases, timing scenarios)

### 4.2 ACT

- Implement the smallest correct change
- Follow existing patterns in the current component and product structure
- Match existing code style and naming conventions
- Keep hardware-specific code isolated in BSP layer
- Use RTOS abstraction for timing operations
- Update related documentation after the code/spec changes are complete and
  before final verification

### 4.3 VERIFY (Required Checklist)

- **Validation:** All sensor fields validated using specific methods (e.g., `is_temp_valid()`)
- **Counters:** Field-level averaging uses appropriate counter structs
- **Initialization:** Data structures initialized to invalid sentinels
- **Null checks:** Sensor pointer validity checked before operations
- **Abstraction:** No direct FreeRTOS or ESP-IDF hardware calls (use BSP/RTOS)
- **Tests:** Mock classes added, edge cases covered, timing logic verified
- **Constants:** No new magic numbers; configuration values centralized
- **Documentation:** Related `README.md`, service docs, specs, and templates are
  updated, or explicitly confirmed unchanged; Markdown follows
  [`docs/STYLE.md`](docs/STYLE.md)
- **Firmware build:** Relevant ESP-IDF product build succeeds after exporting
  ESP-IDF in the same shell, for example `idf.py -C products/<product> build`
- **Host test build:** Native tests configure and build successfully with the
  `TEST_HOST` path, for example `cmake --build tests/build`
- **Test pass:** All relevant tests pass, for example
  `ctest --test-dir tests/build --output-on-failure`
- **Command evidence:** If the agent cannot run a required build, test, or lint
  command, ask the user to run it and provide the output before claiming the
  verification is complete
- **Docs lint:** Markdown lint or `pre-commit` checks pass for documentation
  changes

## 5. Build System Reference

**ESP-IDF (Reference Product):**

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference build
```

**ESP-IDF notes:**

- In a fresh terminal session, `idf.py` may not be on `PATH` until the ESP-IDF export script is sourced
- Use `. "$HOME/Tools/esp/esp-idf/export.sh"` in the same shell session before `idf.py -C products/<product> build` or other non-flashing ESP-IDF commands
- If `get_idf` exists as a local alias or shell helper, it may be used as a convenience wrapper for the same export step
- Do not run `idf.py flash`, `idf.py monitor`, or flash+monitor variants from the agent; leave those to the user

**Native Tests:**

```sh
cmake -S tests -B tests/build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build tests/build
ctest --test-dir tests/build --output-on-failure
```

**Native test notes:**

- Tests use a standalone native CMake flow from the repository root
- `tests/Makefile` provides equivalent wrappers if needed (`make build`, `make test`, `make clean` from `tests/`)

## 6. Common Patterns

### Adding a New Sensor

1. Extend the shared sensor capability in the existing sensor component structure
2. Add or update shared data structures and validation rules as needed
3. Update the sensor orchestration logic only where the new sensor affects shared behavior
4. Add or update host tests for the shared sensor logic
5. Keep product-specific wiring out of shared components

### Working with Sensor Data

- Use field-specific validation: `is_temp_valid()`, `is_pm_01_valid()`, etc.
- Handle partial sensor failures with field-level counters
- Check `sensor != nullptr` before calling methods
- Return invalid sentinels when no valid readings available
- Cache capabilities (`supports_temp_hum()`) before loops

### Using Serial Communication

- Serial communication uses `AirgradientSerial` abstract interface
- Hardware configuration (pins, ports, reset) passed via **constructor**
- Generic `begin(baud_rate)` for initialization
- Supports native UART and I2C-to-UART bridge implementations
- Enables polymorphism and dependency injection for testing
- Keep shared serial logic in the shared components layer, not in product-specific application code

### Logging Tags

- Prefer file-local logging tags in `.cpp` files: `static constexpr const char* TAG = "Name";`
- Do not store logger tags as per-instance class members unless a header-only implementation requires it

## 7. Communication & Reviews

- Write changes for easy review: focused commits, clear intent
- If requirements are ambiguous, ask targeted questions first
- When uncertain, prefer conservative behavior with proper error handling
- Document complex logic inline, especially timing-sensitive operations
- Reference the current repository docs or component-local docs when explaining design decisions

## 7.5 Battery Learning Mode (GO v0.3)

The BQ27427 fuel-gauge Impedance Track algorithm needs a clean
charge → relax → discharge → relax cycle to derive Qmax and the Ra Table.
Once a "golden image" `.gg` has been extracted from a successful cycle, it
gets baked into firmware so production units boot with a learned gauge and
skip the 18 h cycle. The toggle below is the admin affordance that runs
that cycle on the bench.

**Admin menu**: `Settings → Battery Learning: Off / On` (visible only in
admin mode). **NOT persisted to NVS** — the in-struct default `false` wins
on every boot, so admin must opt in fresh per bench-test session.  Exiting
admin mode also force-resets the flag to `false` (see
`go_orchestrator.cpp:ExitAdminMode` block).  Same in-memory-only pattern as
`charge_current_ma`.  The `KEY_BATTERY_LEARNING = "blr"` constant is
preserved for wire compatibility with older BLE clients (no read/write).

**When ON, the orchestrator does four things** (all in
`products/go/main/go_orchestrator.cpp:on_bms_status_timer()`):

1. **ICHG override (every tick, charge phase)**: while plugged in and the
   BMS is in any charging state, ICHG is force-pushed to
   `LEARNING_CHARGE_CURRENT_MA` (1500 mA).  PowerService's setter is
   idempotent — one I²C write per change.  Restored to the user-configured
   `_settings.charge_current_ma` the moment charging stops or USB is
   removed.
2. **Charge-done UX**: when BMS transitions to `NotCharging` while still
   plugged in, the existing 500 s rest countdown fires (matches the
   BQ27427 ResRelax Time so OCV1 settles before the user disconnects),
   followed by LED8 blink + 4-note "unplug me" beep. Unchanged.
3. **LOW_POWER state — ACTIVE THROUGHOUT THE CYCLE** (edge-triggered the
   moment `battery_learning_enabled && admin_mode` becomes true,
   regardless of plug state):
   - `PowerService::set_pm_power(false)` — drives EN_PM HIGH, killing the
     SPS30 directly.  The *only* effective gate while VBUS is present
     (PMID is auto-PassThrough = +5 V from VBUS during charge, so PMID
     override alone wouldn't help).
   - `PowerService::set_force_pmid_passthrough(true)` — pins PMID at
     PassThrough regardless of power source; matters only on cell power,
     where it suppresses the cell→+5 V boost converter.
   - `GpsService::sleep_for_ms(LEARNING_GPS_SLEEP_MS)` — 8 h CFG-SLEEP
     covers the worst-case unattended cycle without spurious wake.
   - `_in_learning_low_power = true` — `check_timers()` then strips
     `SensorGroup::PM` from the measurement request mask, so the sensor
     producer never tries to read the (now powered-off) SPS30.
   Exit fires when the toggle (or admin mode) goes false: PM power
   restored, force flag released, GPS woken.  Plug state never triggers
   exit by itself — the user keeps low-power gates through the full
   charge → unplug → relax → discharge → relax cycle.
4. **E-paper home screen swap**: when the toggle is on, `DisplayService`
   replaces the normal sensor grid (PM2.5 / CO2 / Temp / Humidity / TVOC /
   NOx) with a power dashboard rendering SOC, V, I, remaining/full-charge
   capacity, FG temperature, FC/CHG/DSG flags, system + PMID rails, ICHG,
   and the BMS charging-state string.  Phase banner at the top:
   `CHARGE` / `RELAX` / `CHRG LOW-PWR` / `RELAX LOW-PWR` derived from
   `plugged_in` + `_in_learning_low_power`.  Data flows via the
   `PowerSnapshot::fg_*` fields (added to retain the FG snapshot past
   `poll_bms()`) → `BuildContext::power_dashboard` → `DisplayValues` →
   `DisplayService::_draw_power_dashboard()`.  Refresh cadence is whatever
   the existing display update flow uses (BMS poll triggers an update).

**Why these specifically**: the gauge needs idle current below
`sleep_current_ma` (50 mA, set in `go_hardware_board.cpp:223`) for ~5 h to
enter Relax → take OCV → update Qmax. SPS30 (~50 mA running) and GPS
(~30 mA active) are the two biggest non-essential loads; killing them gets
GO v0.3 down to ~15-20 mA total. We deliberately do NOT raise
`sleep_current_ma` in firmware — production units must keep the 50 mA
threshold so their normal-use Qmax updates remain meaningful.

**Full bench-test cycle for a fresh gauge**:

1. Phase 0 — flash firmware, plug cell into JST, enable admin mode, toggle
   Battery Learning ON. Verify boot log shows `BQ27427: Cell config
   already correct (DC=2000 DE=7400 TermV=3000 SleepI=50) — no change`
   (idempotent path preserves learning across reboots).
2. Phase 1 — plug USB. Charger runs at 1500 mA, reaches Taper, BMS
   transitions to NotCharging. Charge-done melody + 500 s rest countdown
   fires. Wait for LED8 blink + unplug beep.
3. Phase 2 — unplug USB at the beep. LOW_POWER engages automatically. Let
   sit for ≥5 h on cell power. Idle current should be < 20 mA.
4. Phase 3 — apply ~15 Ω 5 W resistor across the cell terminals (JST,
   **NOT** the USB-C OTG output — connecting across +5 V boost multiplies
   apparent cell current via the BQ25628's boost converter). Total cell
   load ~400 mA = C/5 for a 2000 mAh cell. Discharge to EDV0 (V_term =
   3.0 V).
5. Phase 4 — remove the load. Let sit another ≥5 h on cell power. Gauge
   takes OCV2 → updates Ra Table. `Update Status` register (DM subclass
   82, offset 0) should transition 0x05 → 0x06.
6. Phase 5 — connect bqstudio over I²C (EV2400 or equivalent), confirm
   Update Status = 0x06, export Tools → Golden Image → `.gg`. Bake into
   firmware for production.

**Critical invariant — do not break**:

The BQ27427's `configure_cell()` (in
`components/airgradient-bms/drivers/bq27427/bq27427.cpp`) is idempotent: if
current DM values already match the requested `CellConfig`, no CFGUPDATE
session is entered and the learned Qmax / Ra Table survive across reboots.
This is the mechanism that protects the `.gg` golden image after it's
baked in. **Never change** the `CellConfig` literal at
`products/go/main/go_hardware_board.cpp:219-224` without also re-running
the full learning cycle and re-exporting a fresh `.gg` — a value mismatch
forces CFGUPDATE which wipes Qmax to factory defaults.

The `sleep_current_ma = 50` value in that same struct is the
production-correct threshold for normal-use Qmax updates. The Battery
Learning workflow above gets temporary idle current under 50 mA via
peripheral shutdown, not by raising the threshold.

## 8. Documentation

Use the current repository docs as the primary source of truth:

- `README.md` for repository overview and build/test entrypoints
- `components/README.md` for shared component structure
- `products/README.md` for product application root structure
- `tests/README.md` for host-test workflow
- component-local `README.md` files for component-specific details

When repository structure and older architecture notes disagree, prefer the
current codebase structure and the local documentation near the code.

### 8.1 Documentation Style

When creating or editing any Markdown file, follow [`docs/STYLE.md`](docs/STYLE.md).

- Required heading order, casing rules, and code-fence languages live in
  `docs/STYLE.md`
- Per-doc-type templates live under [`docs/templates/`](docs/templates):
  component README, product README, service doc, spec doc
- `markdownlint-cli2` runs via `pre-commit` on every staged Markdown file;
  configuration is in `.markdownlint.json` and `.markdownlint-cli2.jsonc`
- The same hooks run in CI on every pull request via
  [`.github/workflows/pre-commit.yml`](.github/workflows/pre-commit.yml);
  PRs that fail formatting or lint checks will be blocked
- Vendor / third-party component docs (`components/esp-nimble-cpp/`,
  `components/embedded-i2c-scd4x/`, `components/libnmea-esp32/`,
  `components/u8g2/`, `components/bq25629/`) are out of scope — leave them
  as-is

**First-time setup** (run once per clone):

```sh
pip install pre-commit
pre-commit install
```

After this, `git commit` will auto-run the hooks against staged Markdown
files. To run all hooks against the whole tree manually:

```sh
pre-commit run --all-files
```
