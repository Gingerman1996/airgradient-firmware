# airgradient-wifi

Shared Wi-Fi foundation for AirGradient products: a `WifiManager` service
that owns the radio, plus a thin `WifiHal` so product code never depends
directly on the ESP-IDF Wi-Fi stack.

## Status

`Experimental` — the `WifiManager` service, `WifiHal` interface, and
`EspWifiHal` driver are implemented and host-testable through a mock
HAL. `products/reference` consumes it via `test_wifi.cpp` (STA scan,
connect, static IP, mode-switch sweep), `test_http_server.cpp`
(SoftAP) and `airgradient-provisioning` component. No shipping product depends on it yet;

## Scope

This component owns:

- Wi-Fi mode control (Off / STA / AP / APSTA)
- STA connection lifecycle with hybrid auto-retry and exponential backoff
- Saved-credentials connect via empty-SSID convention plus the
  `has_saved_credentials()` query
- Transient (non-persistent) connect via `WifiStaConfig::persist = false`
  for factory-default fallback flows
- Async Wi-Fi scan (only valid while STA is disconnected)
- Soft-AP control with caller-provided SSID / password
- mDNS lifecycle (auto-start on got-IP, auto-stop on disconnect / Off)
- Static IP configuration as an alternative to DHCP
- Disconnect-reason normalisation from raw ESP-IDF codes
- Credential pass-through to the ESP-IDF default Wi-Fi NVS, plus a
  `clear_saved_credentials()` escape hatch
- Power-save mode selection (`None` / `MinModem` / `MaxModem`)

This component does not own:

- Provisioning UX / transport (BLE provisioning, SmartConfig, …)
- Sockets, HTTP, MQTT, or any application-level protocol
- Channel-biased scanning, regulatory / country configuration
- Dynamic mDNS service add / remove after initial configuration
- AP SSID auto-generation from MAC

## Directory Layout

```text
components/airgradient-wifi/
  hal/
  types/
  services/
  drivers/
  tests/
  CMakeLists.txt
  Kconfig
  idf_component.yml
  README.md
```

- `hal/` — abstract `WifiHal` interface (DI seam, mocked in host tests)
- `types/` — public enums, structs, callbacks, sentinels
- `services/` — `WifiManager` (pure C++, host-testable)
- `drivers/` — `EspWifiHal` (ESP-IDF backed implementation)
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/wifi_hal.h"
#include "types/wifi_types.h"
#include "services/wifi_manager.h"
#include "drivers/esp_wifi_hal.h"
```

Guideline:

- include from `types/` for shared enums, structs, callbacks
- include from `hal/` only when implementing or mocking the HAL
- include from `services/` for the product-facing API (`WifiManager`)
- include from `drivers/` only when instantiating `EspWifiHal`

## Design

```text
caller -> WifiManager -> WifiHal& -> EspWifiHal -> esp_wifi_* / esp_netif_* / mdns_*
```

```mermaid
flowchart LR
    Product[Product code] --> Mgr[WifiManager<br/>pure C++]
    Mgr --> Hal[WifiHal<br/>abstract]
    Hal --> Esp[EspWifiHal<br/>ESP-IDF driver]
    Esp --> Stack[ESP-IDF Wi-Fi / netif / mDNS]
```

`WifiManager` owns: mode state machine, connection retry with exponential
backoff, mDNS auto-start / auto-stop, disconnect-reason normalisation,
DHCP acquisition timeout. The HAL stays thin — translate ESP-IDF events
into typed callbacks, drive timers on behalf of the manager, and forward
mode / scan / connect calls. Driver-side code never makes scheduling
decisions.

## Usage

```cpp
EspWifiHal hw;
WifiManager wifi(hw);

wifi.set_on_connected([] { /* L2 link up */ });
wifi.set_on_got_ip([](uint32_t ip) { /* full connectivity */ });
wifi.set_on_disconnected([](WifiDisconnectReason r) {
    // long-term recovery: sleep, switch to AP, reprovisioning, ...
});

wifi.set_mode(WifiMode::Sta);
WifiStaConfig cfg;
std::strncpy(cfg.ssid, "MyNetwork", sizeof(cfg.ssid) - 1);
std::strncpy(cfg.password, "secret", sizeof(cfg.password) - 1);
wifi.connect(cfg);
```

### Saved-Credentials And Transient Connects

`WifiStaConfig::ssid` empty means "use NVS-saved credentials":
`WifiManager::connect()` first calls `_hal.has_saved_credentials()`. If
the HAL reports none it returns `WifiStatus::NotFound` immediately
without touching driver state. Otherwise it forwards to the HAL with
the empty SSID and the HAL calls `esp_wifi_connect()` directly,
letting ESP-IDF auto-connect from NVS. Retry / backoff fields still
apply because they are manager-owned policy. `WifiManager::has_saved_credentials()`
exposes the HAL query so callers can branch between saved-creds and
fallback paths without attempting a connect.

`WifiStaConfig::persist = false` is the factory-default fallback path.
`EspWifiHal::connect_sta()` toggles `WIFI_STORAGE_RAM` immediately
before `esp_wifi_set_config` and restores `WIFI_STORAGE_FLASH`
immediately after, so the set_config call writes RAM only and never
touches NVS. The default `persist = true` keeps every existing caller
source- and behaviour-compatible. The toggle is global driver state;
keep it bounded to a single set_config call inside `connect_sta()`.

## Configuration

The component exposes one Kconfig knob under **AirGradient Wi-Fi** in
`menuconfig` (see `components/airgradient-wifi/Kconfig`):

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AG_WIFI_DHCP_TIMEOUT_MS` | `15000` | DHCP acquisition timeout after L2 association. Bypassed when a static IP is configured. |

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, `lwip` — private,
  only consumed by the ESP-IDF driver
- `espressif/mdns` — managed component declared in `idf_component.yml`;
  IDF 5.x moved mDNS out of the core tree

## Tests

Host tests live in `components/airgradient-wifi/tests/` and run through
the top-level [tests runner](../../tests/README.md). They cover:

- mode state-machine transitions and idempotency
- connect / disconnect / retry backoff
- saved-credentials path (empty-SSID convention, `NotFound` when the
  HAL has no creds, retry / backoff still applied)
- transient (`persist=false`) connect leaves NVS unchanged
- disconnect-reason mapping (raw ESP-IDF code → `WifiDisconnectReason`)
- mDNS auto-start on got-IP, auto-stop on disconnect / Off
- DHCP timeout policy (treated as `dhcp_failed`, non-retriable)
- mode enforcement on `connect` / `start_scan` / `start_ap`
- scan-only-while-disconnected enforcement
- `set_mode` leaving STA emits `requested_by_user` and swallows the
  driver echo (no phantom retry in non-STA mode)
- `status_snapshot` zeroes STA-only fields when disconnected

Hardware-only behaviour (real STA association, AP DHCP, mDNS resolution,
static IP application, power-save effects, NVS clear) is verified
manually on a real ESP32; see `products/reference/main/test_wifi.cpp`
for the runtime smoke test.

## Notes

ESP-IDF does not expose a direct DHCP-failure event — only
`IP_EVENT_STA_GOT_IP` and `IP_EVENT_STA_LOST_IP`. The HAL therefore
exposes two single-shot timers — `arm_dhcp_timeout` /
`cancel_dhcp_timeout` for the DHCP watchdog, and `arm_retry_timer` /
`cancel_retry_timer` for the connect-retry backoff — so the manager
can keep all timing decisions in pure C++. The driver backs both with
`esp_timer`.

### Active-scan dwell

`EspWifiHal::start_scan` requests a 60 ms per-channel active dwell
(`scan_time.active.max = 60`). The intent is to keep a full 42-channel
AUTO-band scan inside the ~10 s PMF SA-Query tolerance of any client
associated to a co-resident SoftAP, so the captive-portal provisioning
flow does not lose its legitimate client to a SA-Query disassoc while
the scan is in flight.

When BLE is enabled on the same chip, ESP-IDF silently overrides the
requested dwell back to BT-coex-safe defaults (~240 ms/channel) and
logs `"Should use default active scan time parameter for WiFi scan
when Bluetooth is enabled"` at warning level. The 60 ms request is
still honoured for `WifiOnly` provisioning flows and for any path
where BLE has already been deinit'd before the scan starts (e.g. the
`airgradient-provisioning` "first AP client commits" teardown). No
public-API change; `WifiScanConfig` is unchanged.

### PMF on SoftAP is not configurable

`EspWifiHal::start_ap` does not attempt to disable PMF on the
soft-AP. ESP-IDF documents `pmf_cfg.capable` as deprecated and always
forces PMF when the peer advertises support; setting `capable=false`
is silently dropped. Mitigation for the resulting SA-Query disassoc
under BT-coex lives entirely in `airgradient-provisioning` (transport
selector, first-client-wins teardown, shortened scan dwell above).

### Band mode is not pinned

ESP-IDF persists `band_mode` to NVS via `WIFI_STORAGE_FLASH`. A
one-time `esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY)` call
survives reboots, which can silently mask regressions during
diagnosis. Products that want a specific band policy must call
`esp_wifi_set_band_mode()` explicitly on every boot; otherwise a
fresh-NVS unit falls back to AUTO. This driver intentionally does
not pin the band — leaving the radio on AUTO preserves 5 GHz scan
visibility, and product-side mitigation (e.g. transport-aware
teardown in `airgradient-provisioning`) handles the SoftAP / scan
interaction without needing to disable a whole band.
