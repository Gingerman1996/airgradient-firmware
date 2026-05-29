# AirGradient Go

Firmware for the [AirGradient Go](https://www.airgradient.com/portable/)
portable air quality monitor with GPS, e-paper display, BLE, and battery.

## Sensors

- **PM** — Sensirion SPS30 (PM1.0, PM2.5, PM10 mass + particle counts)
- **CO2** — SenseAir S12 / Sensirion SCD4x / Sensirion STCC4 (probed in
  order at boot; first detected wins)
- **TVOC / NOx** — Sensirion SGP41
- **Temp / Humidity** — SHT40 on V1, then fallback from CO2 and pressure
  sensors
- **Pressure** — Infineon DPS368
- **Battery** — TI BQ25629 charger IC; TI BQ27427 Impedance Track fuel gauge
  (V1 board only)

## Hardware Notes

A single firmware binary supports both the **Prototype** and **V1** boards.
Board variant is detected at runtime by probing the BQ27427 fuel gauge at
I2C address `0x55` during `init_buses()`. All variant-conditional behavior is
gated on `board.variant()`.

| Variant | PM Enable Polarity | Fuel Gauge | SOC Source |
|---|---|---|---|
| Prototype | Active-high (level 1) | None | BQ25629 voltage-curve estimate |
| V1 | Active-low (level 0) | BQ27427 | FG-derived (BQ25629 fallback) |

### Temperature and Humidity Source

V1 boards probe SHT40 during `sensors()`. `SensorManager` resolves
`temp_hum_a` in this priority order: dedicated SHT40, CO2-integrated T/RH
(SCD4x or STCC4), then DPS368 temperature-only fallback. Prototype boards do
not probe SHT40 and use the same fallback chain without the dedicated source.

### PMID Power Rail

The SPS30 PM sensor is powered by the PMID +5 V rail from the BQ25629.
PMID enable (`EN_OTG`) is demand-coupled to PM-sensor power:

- `set_pm_power(true)` arms the boost converter then drives the PM enable
  GPIO to the variant-appropriate level
- `set_pm_power(false)` drops the PM GPIO then disarms the boost, saving
  ~220 uA quiescent from VBAT when PM is off

When USB is present, PMID comes from the buck (pass-through) regardless of
`EN_OTG`. The behavioral change is on battery with PM off: PMID collapses
and the quiescent draw goes away.

All three boot paths call `power().set_pm_power(true)` before `sensors()`.

### Cell Safety

- **EDV (over-discharge):** ship mode fires when cell voltage stays below
  2.9 V for 3 consecutive polls while on battery
- **OT (over-temperature):** charge cutoff at 50 C (resume at 47 C); ship
  mode at 60 C

### Fuel Gauge (V1 Only)

On V1, `init_bms()` initialises the BQ27427 with a two-pass corruption
recovery sequence and idempotent cell-config write. At runtime,
`poll_bms()` prefers FG-derived SOC and surfaces FG telemetry in
`PowerSnapshot`. The log line includes a `src=FG|BMS` marker.

## Build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/go build
```

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot paths, event model, module
  structure
- [`docs/`](docs) — per-service implementation notes (BLE, display, GPS,
  input, orchestrator, power, sensor producer, settings, storage, UI,
  Wi-Fi)
- [`specs/`](specs) — design specs and refactor plans (temporary; deleted
  once shipped, per [`docs/STYLE.md`](../../docs/STYLE.md))
- [`tests/`](tests) — host tests (`go_*.tests.cpp`) plus the BLE
  integration suite under `tests/ble-integration/`
- `main/board_config.h` — pin assignments and I2C addresses
