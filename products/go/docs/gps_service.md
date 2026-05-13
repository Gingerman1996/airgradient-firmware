# GPS Service

Independent RTOS task that reads GPS data for AirGradient Go. Delegates
all NMEA parsing and serial I/O to `GpsDriver` (a concrete class in
`products/go/main/gps/`). The service orchestrates the task lifecycle,
maintains a mutex-protected latest fix, syncs the ESP32 system clock on the
first valid GPS timestamp, and posts `GpsFixUpdate` events to the orchestrator
queue at the configured interval.

## Files

| File | Purpose |
|---|---|
| `products/go/main/gps/gps_types.h` | GPS data types, sentinels, validation helpers |
| `products/go/main/gps/gps_driver.h` | `GpsDriver` class declaration |
| `products/go/main/gps/gps_driver.cpp` | NMEA parsing, serial I/O |
| `products/go/main/gps/gps_service.h` | `GpsService` class declaration, `Config` struct, `gps_read_once()` free function |
| `products/go/main/gps/gps_service.cpp` | Task loop, event posting, clock sync, one-shot read implementation |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `GpsDriver` | `products/go/main/gps/gps_driver.h` | Concrete GPS driver; injected at construction. Wraps `AirgradientSerial`, parses GGA/RMC/GSA sentences via `libnmea-esp32` |
| `GpsData`, `GpsTimestamp`, `GpsFix` | `products/go/main/gps/gps_types.h` | Canonical GPS data types; used in events and `get_latest_fix()` |
| `AirgradientSerial` | `airgradient-serial` | Serial abstraction for GPS module I/O |
| `libnmea-esp32` | `components/libnmea-esp32` | Third-party NMEA sentence parser |
| `RTOS` | `airgradient-common` | `delay_ms()`, `get_time_ms()` — platform-independent timing |
| `go_events.h` | product | `Event`, `EventType::GpsFixUpdate` |
| RTOS task / semaphore / queue | `airgradient-common` | Task creation, mutex for `_latest_fix`, done semaphore for clean shutdown, queue send |

## Configuration

`GpsService::Config` fields — hardware-specific values come from `board_config.h`;
interval is derived from `GoSettings::gps_interval_seconds * 1000`.

| Field | Default | Notes |
|---|---|---|
| `baud_rate` | `115200` | GPS module baud rate; hardware-specific, not a user setting. `GpsDriver::begin()` handles the TAU1113 baud-rate negotiation (starts at 9600, sends binary switch command, re-opens at 115200). |
| `posting_interval_ms` | `5000` | How often to post `GpsFixUpdate` to the event queue; set from `GoSettings::gps_interval_seconds` |
| `task_stack_size` | `4096` | RTOS task stack in bytes; tune at integration time |
| `task_priority` | `3` | Below display worker (4); above idle |

## Usage

```cpp
#include "gps/gps_service.h"
#include "gps/gps_driver.h"
#include "board_config.h"

// Serial transport and driver must outlive the service.
UartSerial gps_uart(BOARD_GPS_UART_PORT, BOARD_GPS_TX_PIN, BOARD_GPS_RX_PIN);
GpsDriver  gps_driver(gps_uart);

// Build config from settings.
GpsService::Config cfg{};
cfg.posting_interval_ms = settings.gps_interval_seconds * 1000;

GpsService gps_svc(gps_driver, event_queue, cfg);

// Optional: inject A-GNSS aiding data (e.g. from BLE phone position).
// Thread-safe: may be called before start() or while the task is running.
// Reduces cold-start TTFF from ~30-60s to ~10-25s (with multi-constellation).
GpsAidingData aiding;
aiding.latitude = phone_lat;
aiding.longitude = phone_lon;
aiding.altitude_m = phone_alt;
aiding.pos_acc_m = phone_acc;
aiding.epoch_s = current_epoch;
aiding.time_acc_ms = 2000;
gps_svc.set_aiding_data(aiding);

gps_svc.start();

// Aiding data can also be updated at runtime (e.g. new BLE data arrives).
// The task picks it up on the next loop iteration.
gps_svc.set_aiding_data(new_aiding);

// Orchestrator can read the latest fix at any time.
GpsData fix = gps_svc.get_latest_fix();
if (is_fix_valid(fix.fix)) {
    // use fix.position, fix.altitude_m, fix.fix, fix.timestamp
}

// Update interval when settings change.
gps_svc.set_posting_interval_ms(new_interval_ms);

// Clean shutdown before deep sleep.
gps_svc.stop();
```

## Event Output

The service posts `EventType::GpsFixUpdate` to the orchestrator queue at the
configured interval, but only when `GpsDriver::has_valid_fix()` is true. If
no valid fix is available at posting time, the event is skipped; `last_post_ms`
is still updated so the next attempt happens one full interval later.

```cpp
// Event union member (go_events.h):
GpsData gps_data;  // position, altitude_m, fix (type/DOP/sats), timestamp
```

The orchestrator can also call `GpsService::get_latest_fix()` directly at any
time (e.g. when logging a route point after a sensor measurement), independent
of the posting interval.

## Internal Architecture

### Task Loop

```text
GpsService::run():
  Create _done_sem (binary semaphore for stop() join)
  GpsDriver::begin(baud_rate)       // baud negotiation → MON-VER → CFG-EPHSAVE → CFG-NAVSAT
  GpsDriver::gnss_start()
  last_post_ms = 0

  while _running:
    if GpsDriver::read():                         // drains serial buffer
      data = GpsDriver::get_data()
      update _latest_fix under mutex
      if !_clock_synced and is_gps_timestamp_valid(data.timestamp):
        sync_system_clock(data.timestamp)         // settimeofday(), #ifndef TEST_HOST
        _clock_synced = true

    now_ms = RTOS::get_time_ms()
    if now_ms - last_post_ms >= posting_interval_ms:
      if GpsDriver::has_valid_fix():
        post_fix_event()                          // RTOS queue send, non-blocking
      last_post_ms = now_ms

    RTOS::delay_ms(10)                            // yield

  GpsDriver::end()
  _done_sem.give()                                // signal stop()
```

### System Clock Sync

On the first call to `GpsDriver::read()` that returns a sentence with a valid
`GpsTimestamp`, the service converts the UTC date/time fields to a POSIX epoch
via `mktime()` and calls `settimeofday()`. A `_clock_synced` flag prevents
repeated syncing.

On ESP-IDF, the default timezone is UTC, so `mktime()` yields the correct
POSIX epoch without any timezone adjustment.

This path is entirely wrapped in `#ifndef TEST_HOST`.

### One-Shot Read (`gps_read_once`)

For the fast-path timer-wake boot path, the orchestrator does not start the
full GPS task. Instead it calls:

```cpp
GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms,
                      const volatile bool &abort = gps_no_abort);
```

This function calls `GpsDriver::begin()`, polls `GpsDriver::read()` every
10 ms until `has_valid_fix()` returns true, `timeout_ms` elapses, or `abort`
becomes true. Captures `get_data()`, calls `GpsDriver::end()`, and returns
the result. Callers should check `is_fix_valid(data.fix)` on the return value.

The `abort` parameter enables ISR-driven early exit during the fast path:
the power button ISR sets a `volatile bool` flag, and the polling loop checks
it alongside the timeout deadline. The default (`gps_no_abort`, a file-scope
`inline const volatile bool = false`) preserves non-abortable behavior for
callers that don't need it.

## Thread Safety

`_latest_fix` is written by the GPS task and read by the orchestrator thread
via `get_latest_fix()`. Access is protected by an RTOS mutex (`_mutex`):

- **GPS task**: `update_latest_fix()` takes the mutex, copies `GpsData` in,
  releases immediately.
- **Orchestrator**: `get_latest_fix()` takes the mutex, copies `GpsData` out,
  releases immediately.

Both holders keep the mutex for a single struct copy — negligible contention.

## Deep Sleep and GNSS Power Mode Sync

The service provides two shutdown modes to match the GPS power policy:

### Active GPS → deep sleep (`stop()`)

Used when `is_gps_active()` returns true at sleep time. Stops the RTOS task
and closes the ESP UART, but does **not** send GNSS stop to the TAU1113.
The module keeps tracking during ESP32 deep sleep so timer-wake can acquire
a fix quickly (hot-start behavior).

### Inactive GPS → deep sleep (`stop_and_idle_gnss()`)

Used when `is_gps_active()` returns false at sleep time, or when the GPS
mode transitions from active to inactive at runtime. Stops the RTOS task,
sends GNSS stop while the serial link is still open, then closes the UART.
This puts the TAU1113 receiver into an idle state to save power.

**Important:** `GpsDriver::gnss_stop()` waits for the TAU1113 binary ACK
response before returning. Without this wait, the UART link may close
before the module has finished processing the stop command, leaving GNSS
tracking active and drawing 16–21 mA during ESP32 deep sleep instead of
the ~1–2 mA idle state.

### Boot with GPS inactive (`idle_gnss()`)

On boot, if the configured mode says GPS is inactive, `idle_gnss()` opens
the UART briefly, sends GNSS stop (to halt the TAU1113's default tracking),
and closes. No RTOS task is created.

### Task lifecycle

All stop variants follow the same pattern:

1. Set `_running = false`.
2. Block on `_done_sem` — the GPS task signals this just before calling
   `RTOS::task_delete(nullptr)`.
3. Delete `_done_sem`, clear `_task_handle`.
4. Caller controls the shutdown sequence: optionally sends `gnss_stop()`
   while the serial link is still open, then calls `_driver.end()`.

`start()` is idempotent: calling it when the task is already running returns
`true` without creating a second task. On start, the task sends `gnss_start()`
after `begin()` to ensure the receiver is tracking.

## Testability

`GpsDriver` is a concrete class with `AirgradientSerial&` injection. Tests
inject a `StubSerial` (byte-queue `AirgradientSerial` subclass) at
construction. Push raw NMEA bytes via `queue_rx()`, call `read()`, assert
on `get_data()` / `has_valid_fix()`.

For future command testing: assert bytes written to `StubSerial` via a
`get_tx_bytes()` method on the stub.

The orchestrator tests use link-time stub replacement for the entire
`GpsService` class (via `go_orchestrator_stubs.cpp`). The stubs take
`GpsDriver&` in the constructor signature. No mock GPS sensor class is needed.

## A-GNSS Aiding

The service supports optional Assisted GNSS (A-GNSS) to reduce cold-start TTFF.
The caller provides approximate position and/or time via `set_aiding_data()`,
which is thread-safe and may be called before `start()` or while the task is
running (e.g. when new data arrives via BLE). The task checks for pending aiding
data on each loop iteration and forwards it to `GpsDriver::inject_aiding()`,
which sends CASIC AID-POS and/or AID-TIME binary messages to the TAU1113 module.

`_aiding_data` and `_aiding_pending` are protected by the existing `_mutex`.
The mutex is held only for a struct copy; serial I/O (`inject_aiding()`) happens
outside the critical section.

Additionally, `GpsDriver::begin()` polls the module version and sends two
configuration commands after baud-rate negotiation:

1. **MON-VER** — polls the module's software and hardware version strings
   and logs them for diagnostics.
2. **CFG-EPHSAVE** — enables ephemeris persistence in the module's flash,
   improving warm-start performance after brief power interruptions.
3. **CFG-NAVSAT** — sets the constellation enable mask to
   `0x00004037` (GPS L1 | GLONASS G1 | BeiDou B1 | Galileo E1 | QZSS L1 |
   BeiDou B1C). The mask requests all L1-band signals the TAU1113 family
   may support; the module firmware silently ignores unsupported signals.
   After the module ACKs the set command, the driver polls CFG-NAVSAT back
   and logs the active mask for diagnostics.

Both commands use `send_cfg_with_ack()` (drain → send → wait for ACK with
one retry). A failed ACK is logged as a warning but does not prevent startup.

If no aiding data is set, `inject_aiding()` is a no-op and the module
cold-starts normally.

See `products/go/specs/a_gnss_aiding.md` for full protocol and design details.
