# Sensor Producer

Independent RTOS task that drives sensor measurements for AirGradient Go.
Wraps the shared `SensorManager` component: the orchestrator signals it with
an iteration count via RTOS task notification; the task blocks inside
`SensorManager::start_measures()` for the full averaging window, then posts a
`SensorDataReady` event to the orchestrator queue.

`SensorProducer` holds only a `SensorManager` reference and has no knowledge
of which sensors are wired — that is the product wiring layer's responsibility
(`go_hardware_board.cpp`).

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_sensor_producer.h` | `SensorProducer` class declaration, `Config` struct |
| `products/go/main/go_sensor_producer.cpp` | Task loop, notification handling, event posting |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `SensorManager` | `airgradient-sensors` (`services/sensor_manager.h`) | Blocking multi-iteration sensor averaging |
| `Sensors` struct | `airgradient-sensors` (`services/sensor_manager.h`) | HAL pointer table injected into `SensorManager` by wiring layer |
| `MeasuresAGo` | `airgradient-common` (`measures_types.h`) | Averaging result; carried in `SensorDataReady` event payload |
| `RTOS` | `airgradient-common` (`rtos.h`) | `task_create()`, `task_delete()`, `queue_send()`, `task_notify_send()`, `task_notify_wait()` |
| `go_events.h` | product | `Event`, `EventType::SensorDataReady` |

## AGo Sensor Wiring

The wiring layer (`go_hardware_board.cpp`) populates a `Sensors` struct and passes it to
`SensorManager`. `SensorProducer` never touches this struct directly.

| `Sensors` field | Driver | Notes |
|---|---|---|
| `temp_hum` | `SHT40` | Dedicated temperature + humidity sensor |
| `co2` | `S8` or `Sunlight` | CO2 sensor (model confirmed at board bring-up) |
| `pms_a` | `PMS5003` | Single particulate matter sensor |
| `pms_b` | `nullptr` | No second PM sensor on AGo |
| `tvoc_nox` | `SGP41` | TVOC + NOx sensor |
| `pressure` | `DPS368` | Barometric pressure + altitude sensor |
| `o3_no2` | `nullptr` | No AlphaSense electrodes on AGo |

Battery management is handled separately by `PowerService` via the
`airgradient-bms` component and is not part of the `Sensors` struct.
Battery data comes from `PowerService::poll_bms()`, not from `SensorManager`.

## Configuration

`SensorProducer::Config` fields:

| Field | Default | Notes |
|---|---|---|
| `task_stack_size` | `4096` | RTOS task stack in bytes; tune at integration time |
| `task_priority` | `5` | Below input task; above idle |

## Usage

```cpp
#include "go_sensor_producer.h"
#include "services/sensor_manager.h"

// Sensor drivers and Sensors struct owned by the wiring layer.
Sensors sensors{};
sensors.temp_hum = &sht40_driver;
sensors.co2      = &co2_driver;
sensors.pms_a    = &pms5003_driver;
sensors.tvoc_nox = &sgp41_driver;
// pms_b, o3_no2 remain nullptr

SensorManager sensor_manager(sensors);

SensorProducer::Config cfg{};  // defaults: stack=4096, priority=5
SensorProducer sensor_producer(sensor_manager, event_queue, cfg);
sensor_producer.start();

// Orchestrator triggers a measurement cycle when a sensor timer fires:
sensor_producer.request_measurement(1, SensorGroup::All);  // single iteration, all sensors

// Orchestrator triggers PM warmup after powering on the sensor via GPIO:
sensor_producer.request_prepare();  // blocks task for ~10 s warmup

// Clean shutdown before deep sleep.
sensor_producer.stop();
```

## Event Output

`SensorProducer` posts `EventType::SensorDataReady` to the orchestrator queue
once per measurement cycle, after `start_measures()` returns.

```cpp
// Event union member (go_events.h):
MeasuresAGo sensor_data;   // all averaged fields; null-sensor fields carry sentinel values
```

The orchestrator receives this event and routes the `MeasuresAGo` payload to
storage, display, and BLE services as appropriate.

## Iteration Count and Sensor Groups

On AGo, the orchestrator always requests 1 iteration. AGo sensors (SPS30,
STCC4, SGP41, DPS368) perform internal averaging, so multi-iteration
firmware averaging adds no meaningful data quality. The per-iteration 2 s
delay is skipped when `iterations == 1`.

The `SensorGroup` parameter controls which sensor categories are polled:

| Group | Sensors |
|---|---|
| `PM` | `pms_a`, `pms_b` |
| `Other` | `temp_hum`, `co2`, `o3_no2`, `pressure` |
| `TvocNox` | `tvoc_nox` (SGP41 read + algorithm step when configured) |
| `All` | All of the above (default) |

The task encodes both values into the `uint32_t` notification: iterations
in bits 0-7, group mask in bits 8-15. On decode, zero iterations defaults
to 1, and `SensorGroup::None` defaults to `All`.

Two sentinel values use the remaining notification space:

| Sentinel | Value | Purpose |
|---|---|---|
| `NOTIFY_CALIBRATION` | `UINT32_MAX` | CO2 background calibration |
| `NOTIFY_PREPARE` | `UINT32_MAX - 1` | PM sensor warmup after power cycle |

## Internal Architecture

### Task Loop

After warmup, the producer enables a gas-index sampler when an SGP41
sensor is wired and `configure_tvoc_nox_index()` succeeds. The sampler
converts the indefinite `task_notify_wait` into a timeout-driven loop so
the Sensirion algorithm is fed at a fixed cadence (Kconfig-configurable,
default 10 s) independent of the measurement interval.

```text
SensorProducer::run():
  warmup()
  sampler_enabled = has_tvoc_nox_sensor() && configure_tvoc_nox_index(TICK_MS)

  while _running:
    timeout = time_until_next_tick if sampler_enabled else UINT32_MAX
    notified = task_notify_wait(&notify_value, timeout)

    if !_running: break

    if notified:
      dispatch to handle_calibration / handle_prepare / handle_measurement

    if sampler_enabled && tick_due:
      handle_sampler_tick()
```

#### `handle_measurement(notify_value)`

When the sampler is active, the `TvocNox` bit is stripped from the
requested group mask so measurement notifications never read SGP41 at an
irregular cadence. After `start_measures()` returns, the cached TVOC/NOx
from the most recent sampler tick is spliced into the result before posting
`SensorDataReady`. When the sampler is inactive, the original mask is
preserved so `All`/`TvocNox` still reads raw SGP41 values.

The latest valid `temp_hum_a` from each measurement is cached for
compensation push.

#### `handle_sampler_tick()`

Pushes cached temp/hum compensation to the SGP41 driver (if available),
then calls `start_measures(1, TvocNox)` and caches the result in
`_last_tvoc_nox`. Missed ticks are skipped, not replayed.

#### Offline (fast path)

In Offline mode, no `SensorProducer` is created. The fast path calls
`start_measures(1, All)` directly on `SensorManager`. The algorithm is
never configured (`_index_configured == false`), so index fields stay at
invalid sentinels. A retained UX placeholder in `execute_fast_path()`
(`go_app.cpp`) overwrites the index fields with raw ticks so the display
still shows a value.

### Orchestrator Signalling

```cpp
// Orchestrator calls this when a sensor timer fires:
sensor_producer.request_measurement(1, groups);

// Orchestrator calls this to warm up PM sensor after power-on:
sensor_producer.request_prepare();

// Internally both use task_notify_send with different values:
uint32_t value = (static_cast<uint32_t>(groups) << 8) | iterations;
RTOS::task_notify_send(_task_handle, value);
// overwrites any unconsumed notification
```

Overwrite semantics are used so that if the orchestrator fires two timers
in quick succession (should not occur in normal operation), the task always
sees the most recent iteration count.

### Task Duration

The sensor task is blocked for the full measurement window:
`iterations × CONFIG_AVERAGING_ITERATION_INTERVAL_MS`.  During this time the
orchestrator continues processing other events (GPS fixes, button presses,
timers) uninterrupted.  This is the primary reason for the independent task —
`SensorManager::start_measures()` is inherently blocking.

## stop() Behaviour

`stop()` sets `_running = false`, sends a zero-value task notification to
unblock `task_notify_wait()`, waits briefly (10 ms), then forcefully deletes
the task via `RTOS::task_delete(_task_handle)`. The forced delete is safe
because `SensorManager` holds no mutexes, so there is no risk of deadlock or
resource leak.

The task entry function blocks indefinitely after `run()` returns (rather than
self-deleting) to avoid a double-delete race with `stop()`.

After `stop()` returns, `_task_handle` is `nullptr`.  A subsequent call to
`start()` creates a fresh task.

## Initialization Order

The wiring layer must follow this sequence:

1. Initialise I2C/UART buses.
2. Construct concrete driver instances (`SHT40`, `S8`/`Sunlight`, `PMS5003`, `SGP41`).
3. Call `driver.init()` on each.
4. Populate a `Sensors` struct (unused pointers set to `nullptr`).
5. Construct `SensorManager(sensors)`.
6. Construct `SensorProducer(sensor_manager, event_queue, config)`.
7. Call `sensor_producer.start()`.

## Thread Safety

`SensorProducer` has no shared mutable state accessed from multiple threads
other than:

- `_running` (`volatile bool`): written by the orchestrator via `stop()` and
  read by the sensor task.  No mutex needed — the flag is used only to signal
   exit; the task is deleted immediately after by `RTOS::task_delete()`.
- `_task_handle`: written by `start()` / `stop()` (orchestrator thread) and
  read by `request_measurement()` (also orchestrator thread).  Both callers
  are in the same thread, so no protection is required.

## Testability

RTOS task notification and queue calls are no-ops under `TEST_HOST`, allowing
the file to compile in native host test builds.

For host testing:

- Inject a mock `SensorManager` (or construct one with mock sensor HALs via
  the `Sensors` struct pattern).
- Replace the RTOS queue with a test double or inspect the `Event` struct
  directly.
- `start()` is a no-op in `TEST_HOST` mode (`RTOS::task_create` returns
  `false`); call `run()` directly in tests to exercise the task loop.
- Verify `request_measurement(0)` causes the task to use 1 iteration.
- Verify `SensorDataReady` is posted with the correct `MeasuresAGo` payload.
