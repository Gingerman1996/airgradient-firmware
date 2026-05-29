# Power Management Service

Product-specific power management for AirGradient Go. Handles BMS status
polling, battery monitoring, sleep cycle management, RTC state persistence,
and shutdown. Called synchronously by the orchestrator — no independent task.

For AGo, the power service also keeps the BQ25629 PMID rail in the correct
mode for the SPS30 supply:

- external input present → PMID pass-through
- no external input / OTG active → PMID 5V boost

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_power.h` | `PowerSnapshot` struct, `PowerService` class declaration |
| `products/go/main/go_power.cpp` | BMS polling, sleep entry/exit, RTC state, fast-path boot logic |

## Dependencies

| Dependency | Component | Usage |
|---|---|---|
| `BmsDevice` | `airgradient-bms` (HAL) | BMS telemetry, status, battery %, watchdog reset |
| `gpio::Hal` | `airgradient-gpio` | Configure GPIO wake sources for deep sleep |
| `go_types.h` | product | `RtcAppState`, `WakeCause`, `LockState` |
| `go_settings.h` | product | `GoSettings` for interval-based sleep decisions |
| `esp_sleep.h` | ESP-IDF | `esp_sleep_*` functions (deep sleep) |

## PowerSnapshot Fields

`PowerSnapshot` aggregates BMS data for the orchestrator and display.  All
fields default to invalid sentinels (`BmsInvalid::VOLT` / `-1.0f` / `false`).

| Field | Type | Sentinel | Description |
|---|---|---|---|
| `battery_voltage` | `float` | `-1.0f` | Battery voltage (V) from ADC |
| `charging_voltage` | `float` | `-1.0f` | Charging/bus voltage (V) from ADC |
| `battery_percentage` | `float` | `-1.0f` | Estimated SOC (0--100%) from `get_battery_percentage()` |
| `charging_status` | `BmsChargingState` | `Unknown` | Enumerated charging state |
| `critical` | `bool` | `false` | Set when `battery_percentage` is valid and below `BATTERY_CRITICAL_PERCENT` (5 %) |
| `charger_status` | `BmsStatus` | all defaults | Full charger status: power source, regulation flags, fault flags |
| `telemetry` | `BmsTelemetry` | all invalid | Full ADC telemetry: currents, system/PMID voltages, thermistor, die temperature |

## Critical Battery Threshold

```cpp
static constexpr float PowerService::BATTERY_CRITICAL_PERCENT = 5.0f;
```

This is a **fixed constant**, not a user-configurable setting.  When the
orchestrator receives a `PowerSnapshot` with `critical == true` it should:

1. Show a low-battery warning on the display.
2. Initiate an automatic shutdown to protect the battery cell.

## PowerService Config

Passed to the constructor; provides pin assignments and the deep-sleep
threshold:

| Field | Type | Default | Description |
|---|---|---|---|
| `pin_wake_button_power` | `int` | — | GPIO number for Button Power deep-sleep wake |
| `pin_wake_button_boot` | `int` | — | GPIO number for Button Boot deep-sleep wake (`-1` on ESP32-C5 — GPIO28 is not RTC-capable) |
| `pin_ext_wdt` | `int` | `-1` | External watchdog GPIO (`-1` = disabled); pulsed HIGH 20 ms on reset |
| `deep_sleep_threshold_ms` | `int` | `5000` | Minimum sleep duration (ms) to bother entering deep sleep; shorter intervals stay awake. AGo sets this to `5000` |
| `pin_pm_power` | `int` | `-1` | PM sensor power-enable GPIO (`-1` = no GPIO hold during sleep) |
| `sensor_hold_max_sleep_ms` | `uint32_t` | `20000` | Maximum sleep duration (ms) for which the PM sensor power GPIO is latched at the on-level during deep sleep (LOW for v0.3 active-low EN_PM). Above this threshold the sensor powers off normally |
| `pm_sleep_threshold_ms` | `uint32_t` | `20000` | Minimum measurement interval (ms) to power-cycle the PM sensor between measurements in non-Offline modes. Accounts for ~10 s warmup plus minimum off-time |

## Sleep Type Selection

`decide_sleep(settings, lock_state, mode, awake_ms)` is pure logic (no
platform calls, testable on host). Returns `SleepDecision {type, duration_ms}`:

```text
Not Offline mode → {None, 0}   (only Offline mode sleeps)
Unlocked         → {None, 0}   (never sleep while user is active)

sleep_ms = (measure_interval_seconds * 1000) - awake_ms   (clamped to 0)

sleep_ms >= deep_sleep_threshold_ms → {Deep, sleep_ms}
sleep_ms <  deep_sleep_threshold_ms → {None, 0}   (stay awake)
```

The single `measure_interval_seconds` (always ≥ 1) determines the sleep
duration directly. `awake_ms` is subtracted so the total cycle (awake +
sleep) matches the configured interval.

## Sleep Entry

`enter_sleep(sleep_duration_ms)` — only called when `decide_sleep()` returns `Deep`:

1. If `should_hold_pm_sensor(sleep_duration_ms)` is true (sleep < `sensor_hold_max_sleep_ms`
   and `pin_pm_power >= 0`):
   - `gpio_hold_en(pin_pm_power)` — latch the current output level.  On
     ESP32-C5 (`SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP=1`) per-pin hold
     automatically persists during deep sleep; no global
     `gpio_deep_sleep_hold_en()` call is needed
   - The PM sensor fan keeps spinning; the sensor stays warm
2. Calls `configure_wake_sources(sleep_duration_ms)`:
   - Timer: `esp_sleep_enable_timer_wakeup()` (µs)
   - Buttons: `esp_sleep_enable_ext1_wakeup()` with a combined bitmask for
     both button pins (ESP32-C5 target uses EXT1; no EXT0 support on this chip)
3. Calls `esp_deep_sleep_start()` — does **not** return.

The caller must set `RtcAppState::sensors_warm` via
`should_hold_pm_sensor()` and call `save_state()` **before** `enter_sleep()`.

## PM Sensor Warm-Hold

For short deep sleeps (< `sensor_hold_max_sleep_ms`, default 20 s) the SPS30
power-enable GPIO (active-low on v0.3) is latched at LOW during sleep via
`gpio_hold_en()`. On ESP32-C5,
per-pin hold persists through deep sleep automatically, so no global
`gpio_deep_sleep_hold_en()` call is needed. The fan keeps spinning and the
sensor stays in measurement mode.

On the next timer wake the fast path reads `RtcAppState::sensors_warm`:

| `sensors_warm` | Behavior |
|---|---|
| `true`  | `release_sleep_gpio_holds()` calls `gpio_hold_dis()` on the pin, `SPS30::init(skip_reset=true)` re-attaches without resetting, **warmup loop skipped entirely** — boot drops from ~14–17 s to ~4–7 s |
| `false` | Normal cold boot: full `SPS30::init()` with `CMD_RESET`, 10 s interruptible warmup |

For sleeps ≥ 20 s the sensor powers off normally and the full warmup runs on
wake — the power saved by sleeping far outweighs the warmup cost.

`should_hold_pm_sensor(duration_ms)` is pure logic (testable on host):

```cpp
return pin_pm_power >= 0 && duration_ms < sensor_hold_max_sleep_ms;
```

`release_sleep_gpio_holds(pin_pm_power)` is a static method called in the
boot path **after** `init_gpio()` has reconfigured the pin as output HIGH.
While the hold is active the pad stays latched HIGH; the GPIO driver writes
the new output configuration to registers underneath. Releasing the hold
then lets the fresh output driver take over with zero power glitch.

## PM Sensor Sleep (Active Mode Power-Cycling)

In non-Offline modes (Portable, Stationary) the device stays awake but
the SPS30 PM sensor may idle for long periods between measurements,
drawing 45–65 mA of continuous fan current.  When the measurement
interval is at or above `pm_sleep_threshold_ms` (default 20 s) the
orchestrator power-cycles the SPS30 via `PIN_PM_POWER` between
measurements.

### Cycle

```text
Measurement completes → on_sensor_data() → set_pm_power(false)
    ↓
Idle (PM off, fan stopped, ~0 mA)
    ↓
Pre-wake timer fires (interval − warmup before next measurement)
    → set_pm_power(true) → request_prepare()
    ↓
SensorProducer runs warmup() (~10 s of discard reads)
    — first read() triggers SPS30 recovery: stop → start → settle
    ↓
Measurement timer fires → request_measurement(1, All)
    — PM data is stable, fan has spun up during warmup
```

### Eligibility

The orchestrator checks eligibility inline at each decision point —
no persistent mode flag is tracked:

```cpp
_mode != OperatingMode::Offline && should_sleep_pm_sensor(interval_ms)
```

### Edge Cases

| Scenario | Handling |
|---|---|
| **Unlock** | Display shows cached data; PM powers on at the next pre-wake timer |
| **Interval shortened below threshold** | `reschedule_sensor_timer()` calls `set_pm_power(true)` |
| **Interval lengthened above threshold** | `reschedule_sensor_timer()` calls `set_pm_power(false)` |
| **Mode change** | `change_mode()` calls `set_pm_power(true)` unconditionally |

### Methods

`should_sleep_pm_sensor(measure_interval_ms)` is pure logic (testable on
host):

```cpp
return pin_pm_power >= 0 && measure_interval_ms >= pm_sleep_threshold_ms;
```

`set_pm_power(on)` controls the PM power GPIO directly.  No-op when
`pin_pm_power < 0`:

```cpp
_gpio.set_level(pin_pm_power, on ? 1 : 0);
```

## Wake Cause Mapping

`get_wake_cause()` maps `esp_sleep_get_wakeup_cause()` to `WakeCause`:

| ESP-IDF cause | `WakeCause` |
|---|---|
| `ESP_SLEEP_WAKEUP_TIMER` | `Timer` |
| `ESP_SLEEP_WAKEUP_EXT0/EXT1/GPIO` | `Button` |
| `ESP_SLEEP_WAKEUP_UNDEFINED` (first power-on) | `PowerOn` |

## Boot Path Routing

`GoApp::run()` selects the boot path via the pure function
`select_boot_path(cause, state)`:

### Fast-path (timer wake, locked)

`is_fast_path_wake(cause, state)` returns `true` when:

```cpp
cause == WakeCause::Timer && state.lock_state == LockState::Locked
```

When true, `GoApp::run_fast_path(state)` is called — **never returns**.
The fast path either enters deep sleep (CPU reboots on wake) or promotes to
`run_interactive()`. The core logic lives in `execute_fast_path()` which
returns a `FastPathResult` (testable on host). A `BootHandoff` struct
describes what has been done when promoting.

Promotion happens when:

- **Sleep too short** (`< deep_sleep_threshold_ms`): stays locked, display
  shows correct locked frame, measurement completed.
- **Button press during fast path**: ISR detects the press during warmup,
  measurement, or GPS read. Unlocks, suppresses wake press, loads RTC
  snapshot for initial display.

### Button-wake path (button wake, Offline mode)

```cpp
cause == WakeCause::Button && state.mode == OperatingMode::Offline
```

Checked after the fast-path condition. `load_rtc_app_state()` returns a
default `RtcAppState` (mode = `Portable`) when no valid state exists, so the
condition is naturally false on the first power-on and falls through to
`run_interactive()`.

When true, `GoApp::run_button_wake_path(state)` renders the wake frame
immediately from the RTC display snapshot and initializes peripherals in
parallel while the display refreshes. See
[ARCHITECTURE.md → Wake and Boot Path](../ARCHITECTURE.md#wake-and-boot-path)
for the four-phase sequence.

### All other cases

`GoApp::run_interactive(cause, {})` is called with a default `BootHandoff`.
All init runs via idempotent GoBoard methods.

## RTC State Persistence

Two separate RTC-memory regions survive deep sleep:

### App state (`go_power.cpp`)

```cpp
RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;
```

- `save_state(state)` — copies `state` into `s_rtc_state` and sets the valid flag.
- `load_state()` — returns a copy of `s_rtc_state` if valid; otherwise returns
  a default-constructed `RtcAppState` (safe starting point for fresh power-on).

Under `TEST_HOST`, `RTC_DATA_ATTR` is defined away so the variables become
ordinary statics — `save_state()` / `load_state()` work identically.

### Display snapshot (`go_display.cpp`)

```cpp
RTC_DATA_ATTR static RtcDisplaySnapshot s_rtc_display_snapshot;
RTC_DATA_ATTR static bool s_rtc_display_snapshot_valid = false;
```

Saved by `save_rtc_display_snapshot(values)` in `prepare_for_sleep()` after
the final display update. Loaded by `load_rtc_display_snapshot(out)` in
`run_button_wake_path()` before the early paint.

Contains the sensor values, GPS clock, battery state, status flags, and
rendering settings from the last displayed frame. Allows the button-wake path
to render a meaningful Home screen without reading NVS or sensors.

### RTC memory budget

| Region | Size | Location |
|---|---|---|
| `RtcAppState` + valid flag | ~14 B | `go_power.cpp` |
| `PayloadCacheStorageData` | ~1.5 KB | `rtc_payload_cache_storage.cpp` |
| `RtcDisplaySnapshot` + valid flag | ~43 B | `go_display.cpp` |
| **Total** | **~1.6 KB** | ESP32-C5: 8 KB available |

## LP Core Watchdog Feed (Deep Sleep)

During deep sleep no code runs on the main CPU.  The pre-sleep
`reset_ext_watchdog()` pulse buys one timeout window (~6 min).  For sleep
intervals longer than that, the LP Core takes over.

### Files

| File | Purpose |
|---|---|
| `products/go/main/ulp/wdt_feed.c` | LP Core program: pulse LP_IO_2 HIGH 20 ms, return |
| `products/go/main/go_ulp.h` | Declare `ulp_wdt_start()`, `ulp_wdt_stop()` |
| `products/go/main/go_ulp.cpp` | Load binary, start/stop with `stall_rdy` polling |

### Lifecycle

| Phase | Call | Where |
|---|---|---|
| Fast-path boot | `ulp_wdt_stop()` | Before `init_core()` |
| Button-wake boot | `ulp_wdt_stop()` | After Phase 1 (SPI/display), before Phase 2 (I2C) |
| Fast-path sleep | `ulp_wdt_start()` | After ISR removal, before `enter_sleep()` |
| Orchestrator sleep | `ulp_wdt_start()` | End of `prepare_for_sleep()`, after `reset_ext_watchdog()` |
| Fresh boot | (none) | LP Core not running on first power-on |

### LP timer interval

60 s — matches the orchestrator's awake-mode watchdog interval.  With a
6-minute watchdog timeout this gives a 6x safety margin.

### GPIO2 pin mux

GPIO2 is driven by the regular GPIO matrix (main CPU) while awake and
by the RTCIO subsystem (LP Core) during sleep.  `ulp_wdt_start()` calls
`rtc_gpio_init(PIN_EXT_WDT)` to switch to RTCIO mode; `init_ext_watchdog()`
inside `init_power()` naturally switches back on the next wake.

### Stop safety

`ulp_wdt_stop()` waits 20 ms (the pulse duration) then calls
`ulp_lp_core_stop()`.  After deep sleep wake the LP Core is not
executing — the delay is a no-op safety margin.  `ulp_lp_core_stop()`
is a harmless no-op when the LP Core is not running (fresh boot,
non-Offline modes).

## BMS Watchdog

The BMS device has a hardware watchdog that must be reset at least every **10 seconds**.
The orchestrator calls `reset_watchdog()` on each measurement timer tick.

If the minimum sensor interval exceeds 10 s, the orchestrator must schedule a
separate periodic call to `reset_watchdog()` to avoid watchdog expiry.

During deep sleep the watchdog is **not** reset.  On expiry the BMS typically
resets charge parameters to defaults; actual behavior should be verified during
hardware bring-up.

## Shutdown (QoN / Ship Mode)

`shutdown()` triggers BMS QoN (ship mode) via `_bms.enter_ship_mode()`,
which writes the BQ25629 registers to cut power to the entire system.
The call should not return since the system loses power. If it does
(error or unsupported hardware), the method spins with `vTaskDelay` to
preserve the "does not return" contract.

Shutdown sequence called by the orchestrator on a Button Power long-press:

1. `storage.end_route()` — close any open route file
2. `storage.backup_cache()` — save chart data to RTC memory
3. `display.show_shutdown()` — show shutdown indicator
4. `power_service.shutdown()` — BMS QoN (does not return)

## External Watchdog

An external hardware watchdog is connected to `PIN_EXT_WDT` (GPIO2). Two
methods on `PowerService` wrap the free functions from
`components/airgradient-common/common.h`:

| Method | Calls | Effect |
|---|---|---|
| `init_ext_watchdog()` | `ext_watchdog_init(gpio, pin)` | Configures pin as output, drives LOW |
| `reset_ext_watchdog()` | `ext_watchdog_reset(gpio, pin)` | Pulses pin HIGH for 20 ms, then LOW |

Both are no-ops when `Config::pin_ext_wdt < 0`.

Pulse points:

| When | Where | Purpose |
|---|---|---|
| Boot (fast + full) | `GoHardwareBoard::power()` on first access | First pulse after wake/power-on |
| Every 60 s | Orchestrator `check_timers()` | Periodic keep-alive |
| Before sleep | Orchestrator `prepare_for_sleep()` | Maximize timeout window during sleep |

## Platform Abstraction Summary

| Method | Testable on Host? | Notes |
|---|---|---|
| `decide_sleep()` | Yes | Pure logic |
| `should_hold_pm_sensor()` | Yes | Pure logic |
| `should_sleep_pm_sensor()` | Yes | Pure logic |
| `set_pm_power()` | Yes (mock gpio::Hal) | GPIO level via HAL |
| `is_fast_path_wake()` | Yes | Pure logic |
| `poll_bms()` | Yes (mock BmsDevice) | I2C reads via driver |
| `poll_status()` | Yes (mock BmsDevice) | Fast status poll + PMID mode sync |
| `reset_watchdog()` | Yes (mock BmsDevice) | |
| `save_state()` / `load_state()` | Yes | `RTC_DATA_ATTR` defined away |
| `enter_sleep()` | No | Calls `esp_sleep_*` + `gpio_hold_en()` |
| `configure_wake_sources()` | No | Calls `esp_sleep_*` |
| `release_sleep_gpio_holds()` | No | Calls `gpio_hold_dis()` |
| `get_wake_cause()` | No | Calls `esp_sleep_get_wakeup_cause()` |
| `shutdown()` | No | BMS hardware command |
| `init_ext_watchdog()` | Yes (mock gpio::Hal) | GPIO config via HAL |
| `reset_ext_watchdog()` | Yes (mock gpio::Hal) | GPIO pulse via HAL |
