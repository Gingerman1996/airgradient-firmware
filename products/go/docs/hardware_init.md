# Hardware Initialization

Boot path selection, hardware initialization, and fast-path boot for the
AirGradient Go product. All initialization logic lives in `main.cpp` with pin
assignments in `board_config.h`.

## Files

| File | Purpose |
|---|---|
| `main/main.cpp` | `app_main()`, boot path selection, `BootContext`, shared init helpers, fast-path boot, `run_interactive()` |
| `main/board_config.h` | Pin assignments and peripheral constants (TBD placeholders) |
| `main/go_types.h` | `BootHandoff` struct, `RtcAppState`, `WakeCause`, `LockState` |
| `main/go_orchestrator.h` | Orchestrator class declaration |
| `main/go_orchestrator.cpp` | Full orchestrator implementation: dispatch, timers, state transitions, sleep |

## Boot Path Selection

`app_main()` determines the boot path before any peripheral initialization
using two static helpers from `PowerService`:

```
app_main():
    cause = PowerService::get_wake_cause()

    if cause == Timer:
        state = load_rtc_app_state()
        if PowerService::is_fast_path_wake(cause, state):
            run_fast_path(state)           // never returns — sleeps or promotes

    if cause == Button:
        state = load_rtc_app_state()
        if state.mode == Offline:
            run_button_wake_path(state)    // never returns

    run_interactive(cause, ctx, {})        // never returns
```

| Wake Cause | Condition | Path |
|---|---|---|
| `PowerOn` | -- | `run_interactive()` with empty BootContext |
| `Timer` + `Locked` | `is_fast_path_wake()` | `run_fast_path()` — measure, display, sleep or promote |
| `Timer` + `Unlocked` | Not fast-path eligible | `run_interactive()` with empty BootContext |
| `Button` + `Offline` | -- | `run_button_wake_path()` — four-phase early paint |
| `Button` + non-Offline | -- | `run_interactive()` with empty BootContext |

### load_rtc_app_state()

Free function declared in `go_power.h`, implemented in `go_power.cpp`. Reads
the same `RTC_DATA_ATTR` static variables that `PowerService::save_state()` /
`load_state()` use. Safe to call before `PowerService` is constructed because
it has no dependencies on I2C, BMS, or any other peripheral.

## BootContext

File-local struct in `main.cpp` that accumulates initialized hardware handles
and driver pointers as boot progresses. Each shared init helper writes its
results into a `BootContext &`. On fast-path promotion, the partially-filled
context is passed to `run_interactive()` so already-initialized resources are
reused, not double-initialized.

```cpp
struct BootContext {
    i2c_master_bus_handle_t i2c_bus;
    bool spi_ready;
    NvsConfigStore *config_store;
    GoSettings settings;
    BQ25629Bms *bms;
    SensorManager *sensor_manager;
    PayloadCache *cache;
    StorageService *storage;
    DisplayService *display;
    PowerService *power_service;
};
```

All pointers are heap-allocated and never freed (the Orchestrator's `run()`
never returns; on fast-path sleep, `enter_sleep()` reboots the CPU).

## BootHandoff

Defined in `go_types.h`. Describes what boot has already done so the
orchestrator can skip redundant work. Default-initialized values represent
a fresh power-on boot.

| Field | Type | Default | Description |
|---|---|---|---|
| `display_painted` | `bool` | `false` | Display already shows valid content |
| `measurement_completed` | `bool` | `false` | A measurement was completed during boot |
| `suppress_wake_press` | `bool` | `false` | Suppress first ButtonPower short-press |
| `initial_lock_state` | `LockState` | `Locked` | Initial lock state for orchestrator |
| `display_snapshot` | `const RtcDisplaySnapshot *` | `nullptr` | Stale display values for seeding |
| `fast_path_measures` | `const MeasuresAGo *` | `nullptr` | Fresh measurement from fast path |

## Shared Init Helpers

These replace the ~120 lines of duplicated init code that previously existed
across the three boot paths. Each helper writes results into `BootContext &ctx`:

| Helper | Dependencies | What it does |
|---|---|---|
| `init_settings(ctx)` | -- | NVS flash, ConfigStore, load GoSettings |
| `init_buses(ctx)` | -- | GPIO power enables, I2C bus, settling delays |
| `init_spi(ctx)` | -- | SPI bus initialization |
| `init_bms_driver(ctx)` | I2C | BQ25629 BMS construction + init |
| `init_sensors(ctx)` | I2C | All sensor drivers + SensorManager (heap-allocated Sensors) |
| `init_storage(ctx)` | SPI | RTC payload cache, NAND flash, StorageService |
| `init_power(ctx)` | BMS | PowerService + external watchdog |
| `init_display(ctx)` | SPI | DisplayService construction (no paint) |
| `init_core(ctx)` | -- | Convenience: settings + buses + SPI + BMS |
| `init_core_no_spi(ctx)` | SPI already done | Convenience: settings + buses + BMS (skips SPI) |

**Note:** `init_sensors()` heap-allocates the `Sensors` struct because
`SensorManager` stores it by reference. The struct must outlive the helper
function.

## Interactive Boot (run_interactive)

Handles both fresh boot (empty BootContext) and fast-path promotion
(partially filled BootContext). Conditionally completes any missing init:

| # | What | Condition |
|---|---|---|
| 1 | `init_core(ctx)` | if `!ctx.config_store` |
| 2 | `init_sensors(ctx)` | if `!ctx.sensor_manager` |
| 3 | GPS driver, touch sensor | always (never done in fast path) |
| 4 | `init_storage(ctx)` | if `!ctx.storage` |
| 5 | Event queue, BLE service | always |
| 6 | SensorProducer, GpsService, InputService | always |
| 7 | `init_display(ctx)` | if `!ctx.display` |
| 8 | `init_power(ctx)` | if `!ctx.power_service` |
| 9 | UIManager | always |
| 10 | Display init (wake values or empty) | if `!handoff.display_painted` |
| 11 | Start producer tasks | always |
| 12 | Orchestrator init + run | always; never returns |

When `run_interactive()` paints the display, it sets `handoff.display_painted =
true` before passing the handoff to the orchestrator, preventing a redundant
`update_display()` from the orchestrator's `unlock()`.

## Fast-Path Boot

Timer wake while locked. Minimal initialization — no event loop, no producer
tasks, no input handling. Goal: measure, display, sleep. If sleep is too short
or the user presses a button, promotes to `run_interactive()`.

| # | What |
|---|---|
| 0 | Install GPIO ISR on power button (falling edge, sets volatile flag) |
| 1 | `init_core(ctx)` + `init_sensors(ctx)` via shared helpers |
| 2 | Interruptible warmup loop: `warmup_step()` with button checks between iterations |
| 3 | One-shot measurement (skip if button pressed) |
| 4 | One-shot GPS if tracking + GPS active, with abort flag (skip if button pressed) |
| 5 | Storage: cache measurement + route point (skip if button pressed) |
| 6 | Power + display + sleep decision (skip if button pressed) |
| 7 | If deep sleep: remove ISR, enter sleep (never returns) |
| 8 | If sleep too short or button pressed: remove ISR, build BootHandoff, call `run_interactive()` |

The fast path **never returns to `app_main()`**. This eliminates a latent crash
where the old code would fall through to `run_full_boot()` and double-initialize
NVS, I2C, and SPI.

### Button detection during fast path

An ISR on `PIN_BUTTON_POWER` (falling edge) sets a `static volatile bool` flag.
The ISR does no allocation, logging, or blocking — only sets the flag. The flag
is checked between warmup iterations, after measurement, after GPS read, and
after the sleep decision. The ISR is removed before `InputService` construction
to avoid double-handler conflicts.

### Promotion handoff

On button press: unlocked, suppress wake press, display painted with RTC
snapshot (stale values + "Unlocked" snackbar), fresh measures if available.

On sleep too short: locked, display painted with locked dashboard, measurement
completed.

## Button-Wake Path

Button wake in Offline mode. Four-phase boot with early paint — unchanged
structure, but uses shared init helpers instead of inline init code. See
[ARCHITECTURE.md §9.4](../ARCHITECTURE.md) for the four-phase sequence.

## Error Handling

- **Bus init failures** (I2C, SPI, NVS): `ESP_ERROR_CHECK` — fatal, device
  cannot function without buses.
- **Sensor init failures**: Log and set `Sensors` pointer to `nullptr`.
  `SensorManager` handles `nullptr` sensors gracefully. A failed sensor does
  not block the rest of the system.
- **NAND mount failure**: `StorageService::init()` returns false. Temporary
  cache still works (RTC-backed). Route persistence is unavailable.
- **BMS init failure**: Logged. PowerSnapshot returns invalid sentinels.

## Board Configuration

All pin assignments in `board_config.h` are placeholder values (`-1`) pending
hardware schematic finalization. Known I2C addresses use datasheet defaults:

| Device | Address |
|---|---|
| SHT40 | 0x44 |
| SGP41 | 0x59 |
| BQ25XX | 0x6B |
| CAP1203 | 0x28 |

Peripheral bus assignments:

| Peripheral | Bus |
|---|---|
| Display (SSD1680) | SPI2_HOST |
| NAND flash | SPI3_HOST |
| GPS (NmeaGps) | UART_NUM_1 |
| CO2 (S8/Sunlight) | UART_NUM_0 |
| PM (PMS5003) | UART_NUM_2 |

## Serial Number and BLE Name

`app_main()` builds a 12-character device serial number via
`build_serial_number()` from `airgradient-common` before full boot. In
Portable mode, the BLE service advertises as `AGo-<serial>`.

## Design Decisions

See [specs/hardware_init.md](../specs/hardware_init.md) for detailed rationale
on: no BSP layer, inline initialization in main.cpp, stack-allocated objects,
settling delays, fast-path NVS load, and sensor serial interface selection.

See the BootContext/BootHandoff design notes in this file and
[ARCHITECTURE.md §7.5](../ARCHITECTURE.md) for the boot path overview.
