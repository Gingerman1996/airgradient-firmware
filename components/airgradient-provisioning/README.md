# airgradient-provisioning

Wi-Fi provisioning manager: owns the provisioning state machine and
two transports — a Wi-Fi captive portal and a BLE GATT interface — so
product code never reimplements the Wi-Fi onboarding flow. A transport
selector on `ProvisioningConfig` chooses BLE-only, Wi-Fi-only, or both
in parallel; in dual-transport mode the manager auto-collapses to the
first transport that gets a client (first-client-wins).

## Status

`Experimental` — state machine, both transports (Wi-Fi captive portal
and BLE GATT), captive DNS, scan filter, and JSON credential parser are
implemented and host-tested. Exercised end-to-end on hardware via the
reference product and the Python integration tests under
`tests/integration/`. UUIDs, BLE status code allocation, and the portal
HTML/JS are still subject to change before going stable.

## Scope

This component owns:

- the provisioning state machine
  (`Idle` → `WaitingForCredentials` → `Connecting` → `Connected`)
- the transport selector
  (`ProvisioningTransport::BleOnly` / `WifiOnly` / `Both`) and the
  first-client-wins teardown that drops the unused side in `Both`
  mode
- the Wi-Fi captive-portal HTTP API and embedded portal page
- the BLE GATT provisioning service (credential write, scan
  notifications, status notifications) and Device Information Service
- the captive DNS responder that redirects all DNS queries to the AP IP
- captive-portal OS probe URL handlers (iOS, Android, Windows, Firefox)
  that trigger the in-app captive browser via `302` redirect
- a shared `ScanFilter` utility (dedup + RSSI sort) used by both
  transports
- a shared `parse_provisioning_json()` credential parser used by both
  transports
- a single event callback that surfaces every lifecycle transition to
  the product
- Wi-Fi AP setup and teardown (`ApSta` on start in `WifiOnly`/`Both`,
  `Sta` on start in `BleOnly` and on stop in every mode)
- HTTP server lifecycle (start on `start()` for `WifiOnly`/`Both`,
  route cleanup + optional stop on `stop()`)
- BLE server lifecycle (init on `start()` for `BleOnly`/`Both`, deinit
  on `stop()`)
- BLE advertising management (stop on client connect, restart on
  disconnect)
- BLE client connect/disconnect tracking for inactivity timeout

This component does not own:

- `WifiManager`, `AgBleServer`, or `HttpServer` — they are borrowed
  from the product for the duration of the session
- application-level provisioning status (server reachable, monitor
  configured, etc.) — the product pushes those codes via
  `send_ble_status()`
- credential storage beyond what `esp_wifi_set_config()` does via NVS
- static IP persistence — `WifiStaticIpConfig` is delivered to the
  product via the event callback; the product must persist and re-apply
  it on subsequent boots
- `disable_cloud` persistence — same as static IP; the product reads
  it from the event callback and decides what it means
- product-specific UI/triggers for entering provisioning

## Directory Layout

```text
components/airgradient-provisioning/
  types/                     public types and enums
  services/                  public ProvisioningManager API
  internal/                  transports, scan filter, DNS codec, JSON parser, timer, QR encoder
  vendor/                    third-party sources (qrcodegen, MIT)
  assets/portal.html         embedded captive-portal page
  tests/                     host tests
  CMakeLists.txt
  Kconfig
  README.md
```

- `types/` — pure C++ types, no ESP-IDF dependencies
- `services/provisioning_manager.h` — the only public header product
  code includes
- `internal/` — transport plumbing and pure-C++ utilities; not part of
  the public API
- `assets/portal.html` — self-contained HTML/CSS/JS shipped via
  `EMBED_FILES`

## Public Includes

```cpp
#include "services/provisioning_manager.h"
#include "services/provisioning_qr.h" // QR helpers for the Provisioning screen
#include "types/provisioning_types.h"
```

## Design

```text
product -> ProvisioningManager -> WifiPortalTransport -> HttpServer&
                               \-> BleTransport --------> AgBleServer&
                               \-> CaptiveDnsResponder -> lwIP UDP
                               \-> WifiManager& (borrowed)
```

Product code constructs `WifiManager`, `AgBleServer`, and `HttpServer`,
then hands them to `ProvisioningManager::start()`. The manager wires
its internal transports onto those objects, drives the state machine,
and emits lifecycle events through a single callback. After `stop()`
returns the product resumes full ownership of the borrowed objects.

### Transport Selection

`ProvisioningConfig::transport` controls which side(s) the manager
brings up. The default is **`BleOnly`** because it is the safest
single-transport flow on ESP32-C5 with AUTO band mode (no SoftAP
client, so the PMF SA-Query disassoc documented for the captive flow
cannot bite).

| Mode | Wi-Fi mode after `start()` | BLE init | Portal routes | SoftAP | Captive DNS | First-client teardown |
|---|---|---|---|---|---|---|
| `BleOnly` (default) | `Sta` | yes | no | no | no | n/a |
| `WifiOnly` | `ApSta` | no | yes | yes | yes | n/a |
| `Both` | `ApSta` | yes | yes | yes | yes | yes — whichever side wins |

In `Both` mode the manager subscribes to AP-client-joined and
BLE-client-connected events. The first side to receive a real client
"commits" the session; the other side is torn down immediately:

- **AP-first commit:** BLE is `deinit()`'d, advertising stopped,
  callbacks cleared. Frees ~28–30 KB of heap in production.
- **BLE-first commit:** captive portal routes are unregistered, DNS
  responder stopped, Wi-Fi reverts to `Sta` (drops the AP). The HTTP
  server is left running so the product can register its own routes
  immediately.

After a side is torn down, repeat commits (e.g. a second BLE central
reconnecting) are short-circuited by per-side `_*_active` guards in
the manager — the teardown helpers are idempotent in observable state
but also avoid re-entering ESP-IDF for no benefit.

Argument validation is also gated by transport: `ap.ssid` is enforced
only when Wi-Fi will run, `ble.device_name` only when BLE will run.
The `start()` argument list does not change — callers may pass an
unused `AgBleServer&` to a `WifiOnly` start (or an unused
`HttpServer&` to a `BleOnly` start) without the manager touching it.

### Runtime Transport Switching

Products may switch transports mid-session as a back-to-back
`stop()` then `start(new_cfg)` against the same `ProvisioningManager`
instance. Contract:

- After `stop()` returns and `Stopped` has fired, the manager is in
  `Idle` and a fresh `start()` with a different transport is legal
  without any other intervening calls.
- `set_on_event()` registration is persistent across cycles. `stop()`
  does not clear it; only the destructor does (as a safety net for
  capture lifetime).
- `stop()` (default `stop_http_server=true`) satisfies the next
  `start()`'s preconditions: BLE deinit'd, routes unregistered, HTTP
  server stopped, Wi-Fi back to `Sta`.
- `stop(false)` keeps the HTTP server running. A subsequent
  `start(WifiOnly)` or `start(Both)` requires a fresh HTTP server and
  will fail its precondition. Use `stop()` (default) if you intend to
  switch into a transport that needs the portal, or stop the HTTP
  server yourself before the next `start()`.
- Repeated `BleOnly ↔ WifiOnly` round trips have been observed to
  fragment the heap by ~3 KB free / ~3.3 KB DMA per cycle. A single
  switch in a provisioning session sits well above the
  management-frame allocation threshold. Designs that expect many
  toggles per session should monitor
  `heap_caps_get_largest_free_block(MALLOC_CAP_DMA)` and bail out
  early.

### State Machine

```text
[*] --> Idle
Idle --> WaitingForCredentials : start()
WaitingForCredentials --> Connecting : credentials submitted
Connecting --> WaitingForCredentials : connect failed (single attempt)
Connecting --> Connected : got IP
Connected --> Idle : stop()
WaitingForCredentials --> Idle : stop() or timeout
Connecting --> Idle : stop()
```

Connection attempts use `max_retry_count = 0` — a single STA attempt
per credential submission. On failure, `ConnectFailed` fires within
~6 seconds and the manager returns to `WaitingForCredentials` so the
user can retry from either transport.

### Lifecycle Events

| Event | Meaning | `data` populated? | `ip` populated? |
|---|---|---|---|
| `Started` | transports active, waiting for credentials | no | no |
| `Connecting` | credentials received, STA connect in progress | yes | no |
| `ConnectFailed` | connect attempt failed, still listening | no | no |
| `Connected` | STA has an IP address | yes | yes |
| `Stopped` | provisioning torn down | no | no |

`Stopped` includes a `stop_reason` field: `ProductRequested` (explicit
`stop()` call) or `TimedOut` (inactivity timeout expired).

### Inactivity Timeout

- Controlled by `ProvisioningConfig::overall_timeout_ms` (0 = disabled).
- Timer starts when entering `WaitingForCredentials`.
- Timer pauses when any client connects (AP STA join or BLE central
  connect).
- Timer resumes when all clients disconnect (both AP and BLE counts
  reach zero).
- On expiry: manager performs full teardown (same as `stop()`) and
  fires `Stopped` with `TimedOut` reason.

### BLE Transport

The BLE transport creates two GATT services on the borrowed
`AgBleServer`:

**AirGradient Provisioning Service** (`acbcfea8-e541-4c40-9bfd-17820f16c95c`):

| Characteristic | UUID | Properties |
|---|---|---|
| Credentials/Status | `703fa252-...` | READ, READ_ENC, WRITE, WRITE_ENC, NOTIFY |
| Wi-Fi Scan | `467a080f-...` | WRITE, WRITE_ENC, NOTIFY |

**Device Information Service** (`180A`):

| Characteristic | UUID | Properties |
|---|---|---|
| Model Number | `2A24` | READ, READ_ENC |
| Serial Number | `2A25` | READ, READ_ENC |
| Firmware Revision | `2A26` | READ, READ_ENC |
| Manufacturer Name | `2A29` | READ, READ_ENC |

BLE security is configurable through `ProvisioningBleConfig`:

- `io_capability` (default `AgBleIoCapability::NO_INPUT_NO_OUTPUT`)
- `auth_flags` (default `AgBleAuth::BOND | AgBleAuth::SC`)

The defaults preserve the reference product's Just Works + bonded
Secure Connections behaviour. Products that need a different security
profile override `auth_flags` and `io_capability` on the config struct
before calling `ProvisioningManager::start()`. The AGo product uses
`AgBleAuth::SC` only (no BOND, no MITM) so one-shot Wi-Fi onboarding
does not leave long-lived phone-side bonds.

Advertising is single-connection: stopped on client connect, restarted
on disconnect.

Scan results are sent as paginated JSON notifications (3 networks per
page, ~100 ms between pages via `ProvisioningTimer`).

### BLE Status Codes

The provisioning manager automatically sends these on Wi-Fi state
changes:

| Code | Name | Meaning |
|---|---|---|
| `0` | `WIFI_CONNECTED` | Wi-Fi connected |
| `10` | `WIFI_CONNECT_FAILED` | Failed to connect |

Products send application-level codes via `send_ble_status()`:

| Code | Name | Meaning |
|---|---|---|
| `1` | `CONNECTING_TO_SERVER` | Connecting to AirGradient server |
| `2` | `SERVER_REACHABLE` | AirGradient server is reachable |
| `3` | `MONITOR_CONFIGURED` | Monitor is configured on dashboard |
| `11` | `SERVER_UNREACHABLE` | AirGradient server is unreachable |
| `12` | `GET_CONFIG_FAILED` | Failed to get monitor config |
| `13` | `NOT_REGISTERED` | Monitor not registered on dashboard |

All codes are defined as named constants in `ProvisioningBleStatus`.

### Captive Portal Detection

The portal registers OS-specific probe URLs (`/hotspot-detect.html`,
`/generate_204`, `/connecttest.txt`, `/canonical.html`, etc.) that
return `302 Found` with `Location: http://192.168.4.1/`. This triggers
the in-app captive browser on iOS, Android, Windows, and Firefox.

The `Location` header **must** be an absolute URL — iOS CNA silently
follows relative redirects without popping the captive browser.

## Contracts

### Preconditions for `start()`

- The `HttpServer` instance must **not** be started and must **not**
  have any routes registered — but only when the selected transport
  will run the Wi-Fi side (`WifiOnly` or `Both`). For `BleOnly` the
  HTTP server is borrowed-but-untouched.
- The `AgBleServer` instance must **not** be initialized — but only
  when the selected transport will run the BLE side (`BleOnly` or
  `Both`). For `WifiOnly` the BLE server is borrowed-but-untouched.
- `ProvisioningConfig::ap.ssid` must not be empty when Wi-Fi will run.
- `ProvisioningConfig::ble.device_name` must not be `nullptr` when BLE
  will run.
- The event callback should be set before `start()`.

### What `start()` Does

The bring-up sequence is gated by `ProvisioningConfig::transport`.
Common to all modes: WifiManager STA callbacks are wired, the DHCP
timeout is set, and `Started` is emitted at the end.

| Step | `BleOnly` | `WifiOnly` | `Both` |
|---|---|---|---|
| Validate `ap.ssid` | skip | enforce | enforce |
| Validate `ble.device_name` | enforce | skip | enforce |
| Register portal HTTP routes | no | yes | yes |
| Init BLE, create GATT services, set security, start advertising | yes | no | yes |
| Wire AP-client join/leave callbacks | no | yes | yes |
| Set Wi-Fi mode | `Sta` | `ApSta` | `ApSta` |
| Start SoftAP | no | yes | yes |
| Start captive DNS responder | no | yes | yes |
| Start HTTP server on `http_port` | no | yes | yes |
| Arm inactivity timeout (if `overall_timeout_ms > 0`) | yes | yes | yes |
| Emit `Started` | yes | yes | yes |

### What `stop()` Does

1. If called from `Connected`, blocks for ~1.5 s so the captive-portal
   browser can poll `/api/status` once and observe the success state
   before the AP is dropped.
2. Cancels the inactivity timeout.
3. Stops the captive DNS responder (no-op if it was never started, as
   in `BleOnly`).
4. Clears BLE transport callbacks, then calls `teardown()` (no-op if
   BLE was never set up, as in `WifiOnly`; otherwise calls
   `ble.deinit()`).
5. Calls `http.unregister_all()` — wipes all registered routes.
6. Optionally calls `http.stop()` (default `true`; pass `false` to
   keep the server running for product routes).
7. Detaches all WifiManager callbacks.
8. Sets Wi-Fi mode to `Sta` — drops the AP (no-op for `BleOnly`),
   preserves any active STA association.
9. Emits `Stopped`.

The ~1.5 s hold only applies when stopping from `Connected`. From
other states (`WaitingForCredentials`, `Connecting`) the teardown is
immediate.

### What `stop(false)` Does Differently

Same as `stop()` except the HTTP server stays running (not stopped).
Routes are still wiped. The product can immediately register its own
routes on the same server without a bind/unbind cycle.

### Destruction

The destructor calls `stop()` as a safety net but **clears the event
callback first** to prevent the `Stopped` event from firing into
potentially-destroyed captures. If the product needs the `Stopped`
event, it must call `stop()` explicitly before the manager goes out
of scope.

### Callback Threading

Callbacks fire from various task contexts (WiFi event loop, HTTP
server task, NimBLE task, esp_timer task). Callers must not block
inside callbacks or call back into `ProvisioningManager`. Signal your
own task and do heavy work there.

### What the Product Must Persist

The provisioning manager does not persist anything beyond what
`esp_wifi_set_config()` stores automatically (SSID + password). The
following fields are delivered via the `Connected` event and the
product is responsible for saving and re-applying them:

| Field | Where | Product responsibility |
|---|---|---|
| `data.static_ip` | `ProvisioningEventInfo` | Save to NVS; call `wifi.set_static_ip()` on every boot before `connect()` |
| `data.disable_cloud` | `ProvisioningEventInfo` | Save to NVS; honor in application logic |

### DHCP Timeout

`ProvisioningConfig::connect_timeout_ms` (default 15000 ms) is
forwarded to `WifiManager::set_dhcp_timeout_ms()`. This bounds how
long the manager waits for DHCP after L2 association — the only
failure mode ESP-IDF does not surface natively.

## Usage

```cpp
EspWifiHal wifi_hal;
WifiManager wifi(wifi_hal);
wifi_hal.init();
IdfHttpServer http;
NimbleBleServer ble;

ProvisioningManager prov;
prov.set_on_event([&](const ProvisioningEventInfo &info) {
    // fire-and-forget signal to product task — do not block here
    switch (info.event) {
    case ProvisioningEvent::Connected:
        // info.data.ssid, info.data.disable_cloud, info.data.static_ip
        // info.ip — STA IP address
        break;
    case ProvisioningEvent::Stopped:
        // info.stop_reason — ProductRequested or TimedOut
        break;
    default:
        break;
    }
});

ProvisioningConfig cfg = {};
// Default is BleOnly. Set Both to bring up BLE + captive portal in
// parallel (the manager will collapse to whichever side gets a
// client first); set WifiOnly for the captive-portal-only flow.
cfg.transport = ProvisioningTransport::Both;
std::strncpy(cfg.ap.ssid, "airgradient-ABCD", sizeof(cfg.ap.ssid) - 1);
std::strncpy(cfg.ap.password, "cleanair", sizeof(cfg.ap.password) - 1);
cfg.ble.device_name = "AirGradient";
cfg.ble.model_name  = "I-9PSL";
cfg.overall_timeout_ms = 180000;

prov.start(wifi, ble, http, cfg);
// ... wait for Connected or Stopped via event callback ...
prov.send_ble_status(ProvisioningBleStatus::CONNECTING_TO_SERVER);
prov.stop();
// Wi-Fi is STA-only; HTTP stopped; BLE deinit'd; product resumes control.
```

See
[`products/reference/main/test_provisioning.cpp`](../../products/reference/main/test_provisioning.cpp)
for the full hardware smoke test.

## Provisioning QR Helper

`services/provisioning_qr.h` exposes a small QR-code encoder shared across
products' Provisioning screens:

- `encode_go_to_app_qr(QrCode*)` — companion-app deep-link URL.
- `encode_wifi_qr(ssid, password, auth, QrCode*)` — `WIFI:` join
  descriptor for the captive-portal SoftAP; reserved chars in SSID and
  password are escaped per the Wi-Fi Alliance spec.

`QrCode` is a caller-owned POD (~212 B at `QR_MAX_VERSION = 6`).
Products encode once on screen entry / transport switch and render many
frames from the cached buffer.  Module pixel size, quiet zone, and
origin live in the product's display layer.

Encoder parameters: ECC `LOW` with `boostEcl`, automatic mask, max
version 6 (41×41 modules).  Bump `QR_MAX_VERSION` if a future payload
needs more bits — stack use scales with the buffer length.

### Third-party attribution

`vendor/qrcodegen.{c,h}` is Project Nayuki's QR Code generator library
(MIT).  See the header preamble for the full notice.

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `components/airgradient-ble/` — `AgBleServer` interface (borrowed)
- `components/airgradient-wifi/` — `WifiManager` (borrowed)
- `components/airgradient-http-server/` — `HttpServer` interface
  (borrowed)
- `json` (ESP-IDF managed) — cJSON for portal and BLE JSON
  encoding/decoding
- `esp_timer`, `lwip` (ESP-IDF) — one-shot inactivity timer, scan
  pagination timer, and captive DNS UDP socket

## Tests

Host tests live under
[`components/airgradient-provisioning/tests/`](tests/) and run through
the [top-level tests runner](../../tests/README.md). They cover:

- `ScanFilter` — dedup, sort, cap, RSSI floor
- DNS query parse / A-response build
- `provisioning_json` — shared credential JSON parsing (ssid, password,
  disableCloud, staticIp), strict static IP validation, error enum
- `BleTransport` — GATT setup/teardown, credential write callback,
  scan pagination via timer, status notifications, advertising
  stop/start on connect/disconnect, teardown during active connection
- `ProvisioningManager` state-machine transitions (start, stop,
  connect, fail, retry, timeout, teardown verification), BLE
  integration (dual-transport credentials, scan result fanout,
  `send_ble_status()`, BLE client timeout pause/resume, mixed AP + BLE
  client timeout suppression)
- portal JSON handlers via `TestHttpRequest` (scan, provision, status,
  captive probe redirect)
- `stop()` teardown contract (routes wiped, HTTP server stopped, BLE
  deinit'd, Wi-Fi reverted to STA)
- `provisioning_qr` — go-to-app and Wi-Fi QR encoding, finder-pattern
  smoke, oversized-payload rejection, empty-SSID rejection

The captive DNS responder and `esp_timer`-backed timers are exercised
on hardware only.

## Observability

Lifecycle log lines carry a `transport=<ble-only|wifi-only|both>`
token so multi-mode field logs are unambiguous:

```text
Provisioning: start(): transport=ble-only AP ssid='...' ch=1 port=80 ...
Provisioning: provisioning started transport=ble-only AP='...' ch=1 port=80
Provisioning: stop() requested (state=2 stop_http_server=1 transport=ble-only)
Provisioning: provisioning stopped (reason=0 transport=ble-only)
```

In `Both` mode, the first-client teardown emits an `INFO` summary
plus a pair of `DEBUG`-level heap probes around the work (compiled
out at INFO+ log levels and elided entirely under `TEST_HOST`):

```text
Provisioning: BLE teardown begin free=115200 largest=98304 dma=98304
Provisioning: first AP client committed — tearing down BLE transport
BleProv: BLE teardown
Provisioning: BLE teardown done  free=145148 largest=98304 dma=98304
```

The `begin/done` probes are research telemetry and the format may
change; do not parse them in field tools.

## Notes

- The captive-portal probe redirect uses a hardcoded
  `http://192.168.4.1/` in the `Location` header. This is the lwIP
  soft-AP default gateway IP. If the AP subnet is ever reconfigured,
  this must be updated in
  `internal/wifi_portal_transport.cpp :: handle_captive_probe()`.
- BLE scan pagination uses `ProvisioningTimer` (wraps `esp_timer` on
  firmware, no-op with `fire_for_test()` under `TEST_HOST`). Pages are
  sent at ~100 ms intervals to avoid overwhelming the BLE stack.
- The provisioning manager clears BLE transport callbacks before
  calling `teardown()` to prevent the disconnect triggered by
  `AgBleServer::deinit()` from calling back into the manager while the
  mutex is held.
- The captive portal page (`assets/portal.html`) does not auto-trigger
  a scan on load — the network list stays hidden until the user
  presses the "Scan for Wi-Fi networks" button (which then becomes
  "Rescan" after the first scan completes). This avoids paying the
  multi-second scan dwell on every page reload during the user's
  onboarding flow.
