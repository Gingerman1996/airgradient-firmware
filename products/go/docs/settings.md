# Settings Service

Persistent device configuration for AirGradient Go, stored in NVS via
`ConfigStore`. Follows the same pattern as the reference product
(`ReferenceSettings`): a plain struct with default member initializers and free
functions for load/save.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_settings.h` | `GoSettings` struct and load/save declarations |
| `products/go/main/go_settings.cpp` | NVS key constants, validation functions, load/save implementations |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `ConfigStore` | `airgradient-config` (`hal/config_store.h`) | Typed key-value persistence interface |
| `NvsConfigStore` | `airgradient-config` (`backends/nvs_config_store.h`) | ESP-IDF NVS-backed implementation injected at construction |
| `OperatingMode`, `GpsMode` | product (`go_types.h`) | Enums serialized as int and reconstructed on load |
| `WifiStaticIpConfig` | `airgradient-wifi` (`types/wifi_types.h`) | Five-uint32 static-IP record persisted alongside the other settings |

## Public API

| Function | Returns | Purpose |
|---|---|---|
| `load_go_settings(store)` | `GoSettings` | Read all keys from NVS, fall back to defaults for missing or invalid values. Never fails. |
| `save_go_settings(store, settings)` | `bool` | Validate every field, then write all keys and commit. Returns `false` if validation fails or any write/commit fails — atomicity by best effort. |

See [`go_settings.h`](../main/go_settings.h) for full signatures.

## GoSettings Fields

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `measure_interval_seconds` | `"mi"` | `int` | `10` | 1 .. 3600 | All sensors measured together at this cadence; no per-group on/off |
| `use_fahrenheit` | `"uf"` | `bool` | `false` | — | Temperature display unit (false=C, true=F) |
| `pm_use_usaqi` | `"pmu"` | `bool` | `false` | — | PM display format (false=µg/m³, true=USAQI) |
| `gps_interval_seconds` | `"gis"` | `int` | `5` | 1 .. 60 | How often the GPS task posts fixes to the event queue |
| `gps_mode` | `"gpm"` | `int` (stored) / `GpsMode` (in struct) | `OnWhenTracking` (1) | 0 .. 2 | GPS operating mode: AlwaysOff / OnWhenTracking / AlwaysOn |
| `operating_mode` | `"opm"` | `int` (stored) / `OperatingMode` (in struct) | `Portable` (0) | 0 .. 2 | Serialized as int; cast to `OperatingMode` on load |
| `inactivity_timeout_seconds` | `"ito"` | `int` | `30` | 5 .. 600 | Persisted and exposed over BLE; not currently used by the runtime auto-lock path |
| `auto_lock_seconds` | `"als"` | `int` | `0` | 0, 10, 30, 60 | Runtime auto-lock timeout; `0` = disabled |
| `device_name` | `"dn"` | `std::string` | `"airgradient-go"` | 1 .. 64 chars | Advertised name for BLE/WiFi |
| `disable_cloud` | `"dc"` | `bool` | `false` | — | Stationary connectivity preference latched from the provisioning payload. Honoured by `CloudService` — when true, both POST and FETCH are suppressed. |
| `static_ip.ip` | `"sip"` | `uint32_t` (stored as `int`) | `0` (DHCP) | — | Static-IP address (network byte order). Zero means DHCP and skips the other static-IP fields on load. |
| `static_ip.netmask` | `"snm"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |
| `static_ip.gateway` | `"sgw"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |
| `static_ip.dns_primary` | `"sd1"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |
| `static_ip.dns_secondary` | `"sd2"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |

Wi-Fi SSID and password are owned by ESP-IDF Wi-Fi NVS via
`esp_wifi_set_config()`. Only metadata that ESP-IDF Wi-Fi does not own
(`disable_cloud`, `static_ip`) lives in `GoSettings`.

## Load Behavior

`load_go_settings(ConfigStore &store)` reads each NVS key in sequence. For
every field:

1. Call the appropriate `store.get_*()` for the key.
2. If the read succeeds **and** the value passes field-level validation: use the
   stored value.
3. If the read fails (key absent) or validation rejects the value: keep the
   struct default.

The function never fails. It always returns a fully populated `GoSettings`
struct with either the stored or the default value for each field.

## Save Behavior

`save_go_settings(ConfigStore &store, const GoSettings &settings)` validates
all fields up front before writing anything. If **any** field is out of range,
the function returns `false` immediately without touching NVS.

On success, all fields are written with `store.set_*()`, and `store.commit()`
is called. The function returns `true` only when all writes and the commit
succeed.

## Validation Rules

All validation is implemented in an anonymous namespace in `go_settings.cpp`
(file-local; not visible to callers).

| Field | Rule |
|---|---|
| `measure_interval_seconds` | `>= 1 && <= 3600` |
| `inactivity_timeout_seconds` | `>= 5 && <= 600` |
| `use_fahrenheit` | No range check (bool) |
| `pm_use_usaqi` | No range check (bool) |
| `gps_interval_seconds` | `>= 1 && <= 60` |
| `gps_mode` | Underlying int in `0 .. 2` (matches `GpsMode` enum values) |
| `operating_mode` | Underlying int in `0 .. 2` (matches `OperatingMode` enum values) |
| `auto_lock_seconds` | `0`, `10`, `30`, or `60` |
| `device_name` | Non-empty and `<= 64` characters |
| `disable_cloud` | No range check (bool) |
| `static_ip.*` | No range check; the loader treats `static_ip.ip == 0` as DHCP and short-circuits the other four fields |

## Stationary Networking Fields

`disable_cloud` and `static_ip` are written by the orchestrator on
every successful provisioning session
(`Orchestrator::on_provisioning_state_changed()` on the `Connected`
event). The provisioning payload carries both values inline, and the
orchestrator persists them via `save_go_settings()` before tearing the
provisioning transport down. `static_ip` is zeroed when the user
selected DHCP, so re-provisioning back to DHCP cleanly clears any
previously-stored static-IP fields.

Factory reset writes a default-constructed `GoSettings` to NVS (zeroing
both fields) and additionally calls `WifiService::clear_credentials()`
to erase the ESP-IDF Wi-Fi NVS entries.

## Relationship to RtcAppState

Some settings fields have a corresponding field in `RtcAppState` (the
RTC-memory cache used to survive deep sleep):

| Setting | `RtcAppState` field | Purpose |
|---|---|---|
| `operating_mode` | `mode` | Settings = durable (NVS, survives power-off). RtcAppState = fast cache (RTC memory, survives deep sleep only). |
| `gps_mode` | `gps_enabled` | `GpsMode` in settings maps to a boolean in `RtcAppState` (enabled = not AlwaysOff). |

**Startup logic:**

- **Fresh power-on:** Load from `GoSettings` (NVS), then initialize
  `RtcAppState` from those values.
- **Deep sleep timer wake:** Read `RtcAppState` directly; skip NVS to keep
  the fast path minimal.
- **Mode or GPS toggle:** Write both `GoSettings` (NVS) and `RtcAppState`.

## Usage Example

```cpp
#include "go_settings.h"

// Provided by the NVS backend (platform-specific):
NvsConfigStore store("go_cfg");

// Load — always succeeds, falls back to defaults for missing/invalid keys.
GoSettings settings = load_go_settings(store);

// Mutate a field.
settings.measure_interval_seconds = 30;

// Save — returns false if any field is invalid.
if (!save_go_settings(store, settings)) {
    ESP_LOGE(TAG, "save_go_settings: invalid settings, not written");
}
```

## Testability

`load_go_settings` and `save_go_settings` depend only on the `ConfigStore`
abstract interface. In host tests, inject a mock `ConfigStore` (e.g., via
Trompeloeil) that returns controlled values. No ESP-IDF dependency exists in
the settings logic itself.

Recommended test cases:

- Load with empty store returns all defaults.
- Load with valid stored values returns the stored values.
- Load with out-of-range stored values falls back to defaults.
- Save with valid settings returns `true` and calls `commit()`.
- Save with any single invalid field returns `false` without writing.
- Round-trip: save then load returns identical values.

## Build-Time Options

BLE link security is not part of `GoSettings`. It is controlled separately by
the product Kconfig option `CONFIG_AGO_BLE_SECURITY_ENABLED` in
`products/go/main/Kconfig.projbuild`.
