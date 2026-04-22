# AirGradient Go (AGo) — Architecture

This document describes the product architecture for the AirGradient Go portable
air quality monitor. It serves as the high-level reference; per-service
implementation details are in the `docs/` folder (linked from §10).

## 1. Product Overview

AirGradient Go is a portable, battery-powered air quality monitor with GPS
tracking, e-paper display, touch/button navigation, and multiple connectivity
modes. The device measures environmental data (PM, CO2, TVOC, NOx, temperature,
humidity), logs routes with GPS coordinates, and streams data over BLE or serves
it over WiFi.

## 2. Modes and Behaviors

AGo has two orthogonal state dimensions.

### 2.1 Operating Modes (set rarely)

| Mode | Radio | Power Source |
|---|---|---|
| Portable | BLE streams data to phone | Battery |
| Stationary | WiFi with local HTTP server | Battery or USB |
| Offline | No radio | Battery |

### 2.2 Behaviors (change frequently)

| Behavior | Description |
|---|---|
| Tracking | GPS + sensor logging, persist route to NAND flash, temporary cache for chart |
| Idle | Sensor readings on display, temporary cache for chart only |
| Shutdown | BMS QoN — full power off |

The default operating mode on fresh boot is **Portable**.

All three modes can enter any behavior. Tracking in Offline mode means GPS +
sensor logging with no radio transmission.

## 3. High-Level Architecture

The system uses a **single centralized event queue** with an orchestrator loop.
Producers post typed events into a queue (via the RTOS abstraction layer). The
orchestrator consumes events, updates state machines, and calls consumers
directly.

```
+--------------------------------------------------------------+
|                    Orchestrator (main loop)                   |
|                                                              |
|  +------------+  +------------+  +---------------------+     |
|  | App State  |  | UI State   |  | Power Policy        |     |
|  | Mode+Behav |  | Lock/Pages |  | sleep eligibility   |     |
|  +------------+  +------------+  +---------------------+     |
|                                                              |
|  dispatch(event) -> switch on event type                     |
|    -> update state machines                                  |
|    -> call consumers directly                                |
+----------------------------+---------------------------------+
                             |
                     Event Queue (RTOS queue)
                             |
     +-----------+-----------+-----------+-----------+
     |           |           |           |           |
+---------+ +--------+ +--------+ +--------+ +--------+
| Sensor  | |  GPS   | | Input  | |  BMS   | | Timer  |
| Task    | |  Task  | |  Task  | | polled | | system |
+---------+ +--------+ +--------+ +--------+ +--------+
  Producers -- each posts typed events to the queue

             Orchestrator calls directly:
     +-----------+-----------+-----------+-----------+
     |           |           |           |           |
+---------+ +--------+ +--------+ +--------+ +--------+
| Display | |Storage | |  BLE   | | Power  | |  UI   |
| Service | |Service | |Service | |  Mgmt  | |Manager|
+---------+ +--------+ +--------+ +--------+ +--------+
  Consumers -- called by orchestrator, not event-driven
```

### 3.1 Event Flow Direction

- **Events** flow from producers into the orchestrator via the event queue.
- **Commands** flow from the orchestrator to consumers via direct method calls.
- The orchestrator never blocks on a consumer. Slow operations (e-paper refresh)
  are handled asynchronously by the consumer's internal task.

## 4. State Machines

### 4.1 App State Machine

Controls what the device is doing. Two orthogonal dimensions:

```
Operating Mode:     Portable | Stationary | Offline
Behavior:           Tracking | Idle | Shutdown
```

Mode affects which radios/features are enabled. Behavior affects the
measurement + logging pipeline. These are independent — the device can be in
any (mode, behavior) combination.

Transitions:
- Mode changes: user navigates UI menu
- Tracking start/stop: user navigates UI menu, or BLE `start_tracking` / `stop_tracking` command
- Shutdown: physical button long press (Button 1) -> BMS QoN

### 4.2 UI State Machine

Controls what is on the display. Independent from the app state machine, though
UI actions can trigger app state transitions (e.g., user selects "Start
Tracking" in a menu).

```
User-navigable:   Home | MainMenu | Settings | SettingsChoice | About | TagList | Confirm
Orchestrator-set: Shutdown | PairingPasskey
```

The UI state machine consumes input events (touch/button) and decides what to
render. The orchestrator forwards input events to the UI manager when the device
is unlocked. Shutdown and PairingPasskey screens are set directly by the
orchestrator (via `set_screen()` / `show_pairing_passkey()`) and do not accept
user input.

### 4.3 Lock / Unlock

A lock/unlock gate separates interactive and passive display modes.

| State | Touch Pads | Display | Sleep | Button 1 Short |
|---|---|---|---|---|
| Locked | Ignored | Static dashboard (periodic refresh) | Eligible | Unlock |
| Unlocked | Active (Up/Down/Enter) | Interactive menus | Never | Lock |

Both states: Button 1 long press -> Shutdown.

Inactivity timeout while unlocked -> auto-lock. The timeout value is
configurable via settings (minimum 5 seconds).

While locked, the display periodically updates sensor values and status on the
dashboard. The refresh interval is configurable and can be disabled entirely.

## 5. Event Types

### 5.1 Producer Events (into queue)

| Event | Source | Payload |
|---|---|---|
| `SensorDataReady` | Sensor Task | `MeasuresAGo` struct |
| `GpsFixUpdate` | GPS Task | `GpsData` from `airgradient-gps` — position, altitude, fix type, DOP, satellite count, timestamp |
| `InputPress` | Input Task | source (touch_up/down/enter, btn_power, btn_boot), type (short/long) |
| `BleConnected` | BLE Service | client connected |
| `BleDisconnected` | BLE Service | client disconnected |
| `BleConfigWrite` | BLE Service | pending Config characteristic write available |
| `BleHistoryWrite` | BLE Service | pending History characteristic write available |
| `BlePairingRequest` | BLE Service | 6-digit passkey for authenticated pairing |
| `Co2CalibrationDone` | Sensor Producer | calibration result code |

### 5.2 System Events (into queue)

| Event | Source | Meaning |
|---|---|---|
| `InactivityTimeout` | Timer | No input for configured duration -> auto-lock |
| `MeasurementTimer` | Timer | Time to start next measurement cycle |
| `WakeFromSleep` | Boot path | Wake cause: timer or button |

BMS is polled directly by the orchestrator on a timer. There is no dedicated
`BatteryStatus` queue event in the current implementation.

### 5.3 UI Action Events (UI -> orchestrator, into queue)

| Event | Meaning |
|---|---|
| `UserStartTracking` | User selected start tracking in menu |
| `UserStopTracking` | User selected stop tracking in menu |
| `UserChangeMode` | User selected Portable/Stationary/Offline |
| `SettingsChanged` | User changed a setting via UI (includes GPS mode changes) |
| `ClearData` | User confirmed data clear via UI |
| `SaveTag` | User selected a tag (with tag index) |

## 6. Tasks

### 6.1 Task Map

| Task | Role | Blocks? | Posts to Queue | Notes |
|---|---|---|---|---|
| Orchestrator | Main event loop | No | -- | Runs in `app_main` context or dedicated task |
| Sensor Producer | Wraps SensorManager | Yes | `SensorDataReady` | Waits for RTOS task notification from orchestrator |
| GPS Producer | UART NMEA read loop | Yes | `GpsFixUpdate` | Product-specific, uses `airgradient-gps` (`GpsSensor` / `NmeaGps`) |
| Input Producer | Classifies raw ISR events | Yes | `InputPress` | Debounce + long-press detection |
| Display Worker | Drives e-paper refresh | Yes | -- | Receives render commands from orchestrator |

BMS is polled directly by the orchestrator on a timer (I2C read, fast and
non-blocking). No dedicated task.

### 6.2 Sensor Producer

Wraps the shared `SensorManager` from `components/airgradient-sensors/`:

```
Sensor Task:
  loop:
    Wait for task notification from orchestrator
    Decode iterations + SensorGroup from notification value
    Call SensorManager::start_measures(iterations, groups)   // blocking
    Post Event::SensorDataReady to event queue
```

The orchestrator controls when to measure by sending a task notification that
encodes both the iteration count (always 1) and which sensor groups to poll
(`SensorGroup::PM`, `SensorGroup::Other`, or `SensorGroup::All`). The two
sensor groups have independent timers.

### 6.3 GPS Producer

Product-specific `GpsService` task in `products/go/main/`. Uses the shared
`airgradient-gps` component for all NMEA parsing and serial I/O. The service
holds a `GpsSensor &` reference (concrete type: `NmeaGps`) and calls its
non-blocking `read()` method in a loop. NMEA sentence accumulation, checksum
validation, and field extraction are fully handled by the component.

```
GPS Task:
  loop:
    Call GpsSensor::read()            // drains serial buffer, parses sentences
    If read() returned true:
      Update _latest_fix from GpsSensor::get_data()
      If timestamp valid and clock not yet synced:
        Set ESP32 system clock via settimeofday()
    If posting interval elapsed and GpsSensor::has_valid_fix():
      Post Event::GpsFixUpdate to event queue
    Delay 10 ms                       // yield, avoid busy-wait
```

`GpsData` (from `airgradient-gps/types/gps_types.h`) carries position
(`GpsPosition`), altitude, fix metadata (`GpsFix` — type, satellite count,
DOP values), and a UTC timestamp (`GpsTimestamp`). The orchestrator can also
call `GpsService::get_latest_fix()` directly at any time (mutex-protected).

GPS hardware is always powered on (no software on/off control at hardware
level). The software enable/disable setting controls whether the orchestrator
uses GPS data, not whether the task runs. When GPS is disabled in settings, the
orchestrator ignores `GpsFixUpdate` events.

GPS data gaps during deep sleep are acceptable. The GPS module maintains its fix
independently; the ESP reads the current position immediately on wake.

### 6.4 Input Producer

Handles all 5 inputs: 3 capacitive touch pads (via `airgradient-touch`) and 2
physical buttons (via `airgradient-gpio`).

```
ISR (GPIO/Touch interrupt):
  Post raw event (gpio_id + timestamp) to raw_input_queue via xQueueSendFromISR

Input Task:
  loop:
    Receive raw event from raw_input_queue
    Debounce
    Classify: short press vs long press (physical buttons only)
    Post Event::InputPress to orchestrator event queue
```

Touch pads support short press only. Physical buttons support short and long
press.

Input mapping:

| Input | Type | Role |
|---|---|---|
| Touch Pad 1 | Capacitive | Navigate Up |
| Touch Pad 2 | Capacitive | Navigate Down |
| Touch Pad 3 | Capacitive | Enter / Select |
| Button 1 | Physical | Short: lock/unlock. Long: shutdown |
| Button Boot | Physical | Short: unused. Long: factory reset |

### 6.5 Display Worker

E-paper display with an independent task for the slow SPI refresh.

```
Orchestrator:
  display_service.update(current_screen_data)   // prepares framebuffer
  // returns immediately

Display Task:
  loop:
    Wait for "frame ready" signal
    Drive e-paper refresh (slow, blocking SPI)
```

The orchestrator never blocks on e-paper refresh. The display service API is
synchronous (prepare what to render), but the hardware refresh is async.

## 7. Power Management

### 7.1 ESP-IDF Power Management Framework

The firmware enables the ESP-IDF PM framework for three layers of power
savings that apply across all operating modes:

| Layer | Mechanism | When Active |
|---|---|---|
| DFS | CPU frequency drops from 240 MHz to XTAL (~40 MHz) when idle | Always, all modes |
| BLE modem sleep | BLE radio sleeps between advertising/connection events | Portable mode (BLE active) |
| Auto light sleep | CPU enters light sleep during FreeRTOS idle periods | Offline, locked, short intervals |

DFS and BLE modem sleep are managed entirely by ESP-IDF drivers and build
configuration (`CONFIG_PM_DFS_INIT_AUTO`, `CONFIG_BT_LE_SLEEP_ENABLE`).

Auto light sleep requires application-level lock management: `PowerService`
holds an `ESP_PM_NO_LIGHT_SLEEP` lock that is acquired at boot and released
only when the device is Offline and locked.  On ESP32-C5, the BLE controller
holds its own `ESP_PM_NO_LIGHT_SLEEP` lock while active, so auto light sleep
is inherently blocked in Portable mode regardless of the application lock.

See [specs/esp_pm.md](../specs/esp_pm.md) for the full feature spec.

### 7.2 Deep Sleep Eligibility

Deep sleep is only eligible when:
- The operating mode is **Offline** (Portable and Stationary never deep sleep), and
- The device is **locked** (never sleep while user is interacting with menus).

### 7.3 Sleep Type Selection

`PowerService::decide_sleep()` computes sleep type and duration in one call:

```
sleep_ms = (measure_interval_seconds * 1000) - awake_ms   (clamped to 0)
```

| sleep_ms | Sleep Type | Rationale |
|---|---|---|
| >= `deep_sleep_threshold_ms` (5 s) | Deep sleep | Benefit exceeds reboot cost |
| < `deep_sleep_threshold_ms` | None (stay awake) | Reboot overhead ≥ sleep duration; loop instead |

The threshold is a tunable constant (`Config::deep_sleep_threshold_ms`), set to
5 s for AGo. Combined with the PM sensor warm-hold (§7.4), the fast-path
boot takes only ~4–7 s for warm wakes (skipping the 10 s warmup), making a
5 s threshold viable. The awake time is subtracted so the total cycle
(awake + sleep) matches the configured interval.

### 7.4 Sleep Entry

```
1. Final display update (wait=true — ensures the e-paper refresh completes)
2. Save RTC display snapshot (sensor values, battery, status flags,
   rendering settings) so the next button wake can render immediately
3. Stop all task-based services (BLE, sensor, GPS, input, display worker)
4. Put SSD1680 into deep sleep mode 1 (~100 µA → <1 µA during ESP deep sleep)
5. Backup chart cache to RTC memory
6. Persist app state to RTC memory:
   - Current mode, behavior, lock status
   - Tracking-in-progress flag, session ID
   - GPS enabled/disabled
   - sensors_warm flag (set when sleep < sensor_hold_max_sleep_ms)
7. Pulse external watchdog (gives it a full timeout window during sleep)
8. If sleep < sensor_hold_max_sleep_ms (20 s):
   - gpio_hold_en(PIN_PM_POWER) — hold PM sensor power HIGH
     (ESP32-C5 per-pin hold persists through deep sleep automatically)
9. Configure wake sources:
   - Timer: next wake time
   - GPIO: Button Power (unlock) — Button Boot is not RTC-capable on ESP32-C5
10. Enter deep sleep
```

For sleeps ≥ 20 s the PM sensor powers off normally (GPIO floats during sleep)
and the full 10 s warmup runs on wake.

### 7.5 Wake and Boot Path

Deep sleep reboots the CPU. All tasks restart from `app_main`. Two
abbreviated paths exist to avoid the full event-loop overhead when it is
not needed.

```
app_main:
  1. Check wake cause (timer vs button vs fresh power-on)

  If timer wake AND locked (fast path):
    run_fast_path(state)
      — Never returns: sleeps or promotes to run_interactive()

  If button wake AND Offline mode:
    run_button_wake_path(state)
      — Never returns

  Otherwise (fresh power-on, button wake in non-Offline mode):
    run_interactive(cause, ctx, {})
      — Never returns
```

All three boot paths use shared init helpers that populate a `BootContext`
struct — a file-local accumulator for initialized hardware handles and
driver pointers. This eliminates the ~120 lines of duplicated init code
that previously existed across the three paths.

When transitioning from the fast path to the interactive event loop (either
because sleep is too short or the user pressed a button), the fast path
calls `run_interactive()` directly with its partially-filled `BootContext`.
Already-initialized resources are reused, not double-initialized. A
`BootHandoff` struct describes what the fast path has already done
(display painted, measurement completed, lock state) so the orchestrator
can skip redundant work.

**Fast path** avoids GPS task, input task, and the full orchestrator for a
"measure and sleep" cycle. An ISR on the power button detects button
presses during blocking warmup, measurement, and GPS operations. If the
user presses the button, the fast path aborts early and promotes to
interactive mode with the device unlocked.

**Button-wake path** eliminates the double display flash (empty frame →
unlock frame). The display worker holds the SPI bus during the ~3 s refresh,
which naturally prevents NAND access until the bus is free — no explicit
synchronization needed.

### 7.6 Shutdown

Button 1 long press triggers BMS QoN (ship mode) via
`BmsDevice::enter_ship_mode()` on the BQ25629. The device fully powers off.
GPS module loses power. Next power-on is a fresh boot.

## 8. Services (products/go/main/)

These are the product-specific service modules that live in the AGo product
root. Each service is a focused unit that the orchestrator wires together.

### 8.1 GPS Service

- Product-specific service (`go_gps.h` / `go_gps.cpp`) — not a shared component
- Delegates all NMEA parsing and serial I/O to the shared `airgradient-gps`
  component; holds a `GpsSensor &` reference (concrete type: `NmeaGps`)
- `GpsData` type comes from `airgradient-gps/types/gps_types.h`; no separate
  product-local GPS data struct
- Independent task (GPS Producer); clean shutdown via `stop()` which blocks
  until the task self-signals before deleting itself
- API: `start()`, `stop()`, `get_latest_fix()` (mutex-protected), `set_posting_interval_ms()`
- Posts `GpsFixUpdate` events to the orchestrator queue at the configured interval
- Syncs ESP32 system clock (`settimeofday`) on the first valid GPS timestamp
- `gps_read_once()` free function provides a synchronous one-shot read for the
  fast-path timer-wake boot path, without starting the full task infrastructure.
  Accepts an optional `const volatile bool &abort` parameter for ISR-driven
  early exit during fast-path button detection

### 8.2 Input Service (Button Service)

- Uses `airgradient-touch` (CAP1203) for 3 capacitive touch pads
- Uses `airgradient-gpio` for 2 physical buttons with interrupt
- Independent task (Input Producer)
- Handles ISR -> raw queue -> debounce -> classify -> typed event
- Posts `InputPress` events to the orchestrator queue
- Future-proof: additional input sources (IMU) can post the same event types

### 8.3 Storage Service

Two tiers of storage:

**Temporary (chart data):**
- Uses `airgradient-payload-cache` with `RtcPayloadCacheStorage` backend
- Keeps last N measurements for display chart rendering
- Survives deep sleep (RTC memory), lost on power-off
- Ring buffer semantics (overwrites oldest when full)
- Supports `clear_cache()` for user-triggered data clearing / factory reset

**Persistent (route data):**
- Uses `airgradient-nand-storage` (SPI NAND with FATFS)
- Stores GPS + sensor data for tracking routes
- Survives power-off
- POSIX file I/O on mounted filesystem
- One file per tracking session: `route_XXXXX.bin` with a random 5-digit session ID
- Supports `clear_routes()` for user-triggered data clearing / factory reset
- Exposes total and used filesystem capacity for BLE status reporting

### 8.4 Power Management

- BMS interaction via `BmsDevice` HAL from `airgradient-bms`
- Polled by orchestrator on a timer (no dedicated task)
- Sleep cycle management (deep sleep entry, wake source config)
- RTC memory state persistence before sleep
- Fast-path boot logic for timer wakes
- External watchdog (GPIO2): initialized at boot, pulsed every 60 s and before sleep
- `PowerSnapshot` aggregates battery voltage, percentage, charging state, critical flag

### 8.5 Settings

- Uses `airgradient-config` (`ConfigStore` with NVS backend)
- Product-specific `GoSettings` struct with field validation and NVS load/save
- BLE link security is not a runtime setting. It is controlled by the build-time
  Kconfig option `CONFIG_AGO_BLE_SECURITY_ENABLED`

Settings fields:
- PM interval, other sensor interval (independent timers; 0 = off)
- Display refresh interval (0 = display off while locked; unlocked always shows dashboard)
- Temperature units (C/F), PM display (µg/m³ / USAQI)
- GPS mode (AlwaysOff / OnWhenTracking / AlwaysOn)
- Operating mode (Portable / Stationary / Offline; default: Portable)
- Auto-lock timeout (0 = disabled, 10s / 30s / 60s)
- Inactivity timeout, GPS interval, device name

### 8.6 Display Service (E-Paper)

- SSD1680 128×250 e-paper driven over SPI
- Independent worker task for async hardware refresh (full + partial updates)
- u8g2 software renderer for framebuffer composition
- `init(values, defer_refresh=false)` — synchronous by default; pass
  `defer_refresh=true` in the button-wake path to return in ~10 ms and let
  the worker handle the initial full refresh in the background
- `deep_sleep()` — puts the SSD1680 into deep sleep mode 1 after the worker
  task is stopped; reduces quiescent current from ~100 µA to <1 µA
- `RtcDisplaySnapshot` struct + `save_rtc_display_snapshot()` /
  `load_rtc_display_snapshot()` free functions in `go_display.cpp`; saves
  the last displayed state before sleep so the button-wake paint can use
  cached values without reading NVS or sensors
- Status bar, hero blocks (PM2.5 / CO2), grid cells, chart, menu overlays, snackbar

### 8.7 UI Manager

- Pure state machine — zero hardware or RTOS dependencies
- Manages screen navigation (Home → MainMenu → Settings / About / TagList / Confirm)
- Metric cycling, settings choice selection, snackbar lifecycle
- `handle_input()` returns `UIAction` for orchestrator-level state changes
- `build_values()` produces a `DisplayValues` snapshot from `BuildContext`
- `sync_settings()` synchronizes internal option indices from `GoSettings`

### 8.8 BLE Streams

- BLE peripheral mode using `airgradient-ble` (NimBLE)
- Streams sensor + GPS data to connected phone
- Exposes live measures, device status, config read/write, command execution
  (including start/stop tracking), and route-history export over one custom
  GATT service
- Only active in Portable mode
- Supports authenticated pairing/bonding when `CONFIG_AGO_BLE_SECURITY_ENABLED=y`
- Development builds can disable authenticated access at build time by setting
  `CONFIG_AGO_BLE_SECURITY_ENABLED=n`

### 8.9 Factory Reset

- Triggered by Button Boot long press or BLE `factory_rst` command
- Clears temporary chart cache and all persisted route files
- Restores `GoSettings` to defaults
- Deletes all stored BLE bonds
- Reboots the ESP on success

## 9. Data Flow Examples

### 9.1 Locked + Tracking + Measurement Timer Fires

```
1. Timer posts MeasurementTimer to queue
2. Orchestrator receives event
3. Orchestrator sends task notification to Sensor Producer (1 iteration, group)
4. Sensor Producer calls SensorManager::start_measures(1, group)
5. Sensor Producer posts SensorDataReady(measures) to queue
6. Orchestrator receives SensorDataReady
7. Orchestrator reads latest GPS fix (from last GpsFixUpdate)
8. Orchestrator calls Storage::append_route_point(point)
9. Orchestrator calls Storage::cache_measurement(measures)
10. Orchestrator calls Display::update(values)
11. If Portable mode and connected: Orchestrator calls BLE::notify_measures(cached_measures, gps)
12. Orchestrator evaluates sleep eligibility -> locked -> sleep
```

### 9.2 User Unlocks Device

```
1. Button 1 GPIO interrupt fires
2. ISR posts raw event to Input Task raw queue
3. Input Task classifies: Button 1, short press
4. Input Task posts InputPress(btn_power, short) to orchestrator queue
5. Orchestrator receives InputPress
6. Orchestrator checks: currently locked -> transition to unlocked
7. Orchestrator disables sleep eligibility
8. Orchestrator resets inactivity timer
9. Orchestrator calls Display::show_dashboard_interactive()
10. Display shows unlock indicator
```

### 9.3 Deep Sleep Fast-Path (Timer Wake)

```
1. ESP32 wakes from deep sleep (timer)
2. app_main reads wake cause: timer, loads RTC state
3. run_fast_path(state): never returns to app_main
4. Install GPIO ISR on power button (falling edge → sets volatile flag)
5. Core init (NVS, GPIO, I2C, SPI, BMS)
   - init_gpio() reconfigures PIN_PM_POWER as output HIGH while hold is active
6. Release GPIO holds (release_sleep_gpio_holds) — pad transitions glitch-free
7. Sensor init via shared helpers
   - If state.sensors_warm: SPS30::init(skip_reset=true) — re-attach only
8. If state.sensors_warm: skip warmup entirely (sensors already warm)
   Else: interruptible warmup loop:
   - Call warmup_step() each iteration (TVOC conditioning + PM discard read)
   - Check button flag between iterations
   - If button pressed: abort warmup, promote to interactive
9. One-shot measurement (skip if promoting)
10. If tracking + GPS active: gps_read_once() with abort flag (skip if promoting)
11. Storage: cache measurement + route point (skip if promoting)
12. Display: build locked dashboard, init display (skip if promoting)
13. Sleep decision:
    - Set sensors_warm = should_hold_pm_sensor(sleep_duration)
    - Save RTC state (with sensors_warm flag)
    - Deep sleep (>= 5s): remove ISR, enter deep sleep (never returns)
    - Too short (< 5s): promote to interactive, stay locked
14. If button pressed during any step:
    - Remove ISR, load RTC display snapshot
    - Build BootHandoff: unlocked, suppress wake press, snapshot for display
    - Call run_interactive(): display wake values, enter event loop
```

The fast path never returns to `app_main()`. It either sleeps (CPU reboots)
or promotes to `run_interactive()` with its partially-filled `BootContext`.
This eliminates the latent crash from double hardware initialization.

The fast-path bypasses the UIManager entirely. `DisplayValues` is built
directly from sensor data and RTC state — always `Screen::Home`,
`locked=true`, no chart data, no snackbar, no menu state. The display call
uses `init()` which sets up the u8g2 renderer, renders, and drives SPI
inline without starting the async worker task.

### 9.4 Button-Wake Path (Button Wake, Offline Mode)

```
1. ESP32 wakes from deep sleep (button press)
2. app_main reads wake cause: button, mode: Offline
3. run_button_wake_path(state):
   Phase 1 (~10 ms):
     4. init_spi(ctx), init_display(ctx) via shared helpers
     5. Load RtcDisplaySnapshot from RTC memory
     6. Build DisplayValues: Home, locked=false, "Unlocked" snackbar,
        sensor values from snapshot (or dashes if invalid)
     7. DisplayService::init(values, defer_refresh=true)
        → renders frame, starts worker, worker begins SPI refresh
        → returns immediately

   Phase 2 (~300 ms, parallel with display refresh):
     8. init_core_no_spi(ctx), init_sensors(ctx) via shared helpers
     9. GPS serial, touch sensor, event queue
     10. Construct SensorProducer, GpsService, InputService
     11. init_power(ctx) via shared helper
     12. Start producer tasks → sensors and touch input operational

   Phase 3 (~3 s, blocks on SPI):
     13. init_storage(ctx) → SpiNandStorage spi_device_transmit() blocks
         until display worker releases bus (natural serialization)
     14. BLE service (requires StorageService from Phase 3)

   Phase 4 (~10 ms):
     15. Build BootHandoff: display_painted=true, suppress_wake_press=true,
         initial_lock_state=Unlocked, display_snapshot=&snapshot
     16. Orchestrator::init(Button, handoff)
         → sets lock=Unlocked, pre-arms snackbar + schedules refresh timer,
           seeds _cached_measures from snapshot, requests fresh measurement
         → skips update_display() (screen already correct)
     17. Orchestrator::run()
```

First meaningful paint: ~3 s. Single display flash (no empty-frame flash).
Touch input and sensors ready at ~310 ms.

## 10. Service Documentation

Detailed implementation documentation for each service:

- [Settings Service](docs/settings.md)
- [Sensor Producer](docs/sensor_producer.md)
- [GPS Service](docs/gps_service.md)
- [Input Service](docs/input_service.md)
- [Storage Service](docs/storage_service.md)
- [Display Service](docs/display_service.md)
- [UI Manager](docs/ui_manager.md)
- [Power Management](docs/power_management.md)
