# ESP-IDF Power Management — Feature Spec

## Goal

Reduce power consumption in Portable and Offline modes by enabling ESP-IDF's
power management framework. Currently, the ESP32-C5 runs at 240 MHz constantly
with no frequency scaling, no BLE modem sleep, and no automatic light sleep.
The only power-saving mechanism is deep sleep in Offline mode when the
measurement interval is long enough.

## Three Layers of Power Savings

### Layer 1 — Dynamic Frequency Scaling (DFS)

**What:** CPU frequency automatically drops from 240 MHz to XTAL (~40 MHz)
whenever no peripheral driver needs full speed. I2C, SPI, and BLE drivers
already acquire/release PM locks during transactions — the framework handles
this automatically.

**When active:** Always, in all modes, all states. No conditions.

**Application code impact:** None. `CONFIG_PM_DFS_INIT_AUTO` auto-configures
DFS at boot with max = `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` (240 MHz) and
min = XTAL frequency. No `esp_pm_configure()` call needed.

### Layer 2 — BLE Modem Sleep

**What:** BLE radio sleeps between advertising intervals (when no client
connected) and between connection events (when connected). Combined with DFS,
the CPU also drops to min frequency during these radio-off windows.

**When active:** Portable mode (only mode where BLE is enabled). Automatically
managed by the BLE controller.

**Constraint:** On ESP32-C5, the BLE controller holds `ESP_PM_NO_LIGHT_SLEEP`,
which blocks automatic light sleep while BLE is active. This means Portable
mode gets DFS + BLE modem sleep, but **not** automatic light sleep.

**Application code impact:** None. Build configuration change only.

### Layer 3 — Automatic Light Sleep (Offline, Short Intervals)

**What:** The CPU enters light sleep during FreeRTOS idle periods (between
queue timeouts). Unlike deep sleep, light sleep preserves full CPU state —
wake is instant, no reboot, no RTC state restore, no sensor re-init. This
fills the gap where deep sleep is too short but the device currently wastes
full CPU power.

**When active (all must be true):**

| Condition | Rationale |
|---|---|
| Operating mode is **Offline** | No BLE — `ESP_PM_NO_LIGHT_SLEEP` not held by BLE controller |
| Device is **locked** | Never light-sleep during user interaction |
| Measurement interval **too short for deep sleep** | Deep sleep path already handles long intervals |

**Note — GPS interaction:** When GPS is active in Offline mode, the GPS task
polls UART every 10 ms. Automatic light sleep still works in the ~10 ms idle
windows between polls. At 115200 baud, ~115 bytes accumulate during a 10 ms
light sleep period, which fits within the ESP32-C5 UART hardware FIFO
(128 bytes). If hardware testing reveals FIFO overflows, UART wake-on-data
(`uart_set_wakeup_threshold()`) can be configured as a mitigation. This is
outside the scope of this spec.

**Application code impact:** The orchestrator manages an
`ESP_PM_NO_LIGHT_SLEEP` lock. The lock is held by default. It is released
only when all three conditions above are met. When any condition stops being
true (unlock, mode change), the lock is re-acquired.

FreeRTOS tickless idle is configured with `configEXPECTED_IDLE_TIME_BEFORE_SLEEP = 3`
(30 ms at 100 Hz tick rate). The CPU will not enter light sleep for idle periods
shorter than 30 ms.

## Conditions Summary

| Mode | Lock | BLE | Deep Sleep | DFS | BLE Modem Sleep | Auto Light Sleep |
|---|---|---|---|---|---|---|
| Portable | Locked | Adv/Connected | — | Yes | Yes | No (BLE blocks) |
| Portable | Unlocked | Any | — | Yes | Yes | No (BLE blocks) |
| Offline | Locked | — | Long interval | Yes | — | Short interval: Yes |
| Offline | Unlocked | — | — | Yes | — | No (unlocked) |
| Stationary | Any | — | — | Yes | — | No (future WiFi) |

## Interaction with Existing Deep Sleep

No changes to the existing deep sleep path. The layers are complementary:

1. `decide_sleep()` runs first. If it returns `Deep`, deep sleep is entered
   as before.
2. If it returns `None`, the CPU falls into the event loop where DFS and
   auto light sleep take effect automatically via the ESP-IDF PM framework.

## Build Configuration

All build configuration is applied via `sdkconfig` (menuconfig). Layer 1
and Layer 2 are fully operational with no application code changes.

| Option | Value | Purpose |
|---|---|---|
| `CONFIG_PM_ENABLE` | `y` | Enable PM framework |
| `CONFIG_PM_DFS_INIT_AUTO` | `y` | Auto-configure DFS at boot (max = 240 MHz, min = XTAL) |
| `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` | `y` | Power down CPU during light sleep |
| `CONFIG_BT_LE_SLEEP_ENABLE` | `y` | BLE modem sleep |
| `CONFIG_FREERTOS_USE_TICKLESS_IDLE` | `y` | Required for auto light sleep |

CPU max stays at 240 MHz. DFS min frequency is set to XTAL (~40 MHz)
automatically by `CONFIG_PM_DFS_INIT_AUTO`.

## Implementation Status

| Layer | Config | Code | Status |
|---|---|---|---|
| Layer 1 — DFS | Done | Not needed | **Complete** |
| Layer 2 — BLE modem sleep | Done | Not needed | **Complete** |
| Layer 3 — Auto light sleep | Done | `ESP_PM_NO_LIGHT_SLEEP` lock management | **Complete** |

## Open Questions

1. **GPIO reset workaround:** `CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=n` is
   disabled due to a known ESP32-C5 stall at startup. Need to verify this does
   not affect light sleep during runtime (after scheduler is running) — the
   boot-time issue may not apply to runtime light sleep.

2. **Peripheral resume after light sleep:** I2C (sensors, BMS), SPI (display,
   NAND), and UART (GPS) need to resume correctly after automatic light sleep.
   ESP-IDF handles clock gating, but hardware testing is needed.

3. **External watchdog timing:** The 60 s watchdog pulse should not be
   affected by short light sleep periods (CPU wakes on timer). But if light
   sleep causes drift, pulse timing may need adjustment.

4. **Power measurement:** Actual current savings for each layer should be
   measured on hardware to validate the approach.
