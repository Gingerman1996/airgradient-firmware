/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_WIFI_PORTAL_TRANSPORT_H
#define AG_PROVISIONING_WIFI_PORTAL_TRANSPORT_H

#include <cstdint>

#include "../types/provisioning_types.h"
#include "types/wifi_types.h"

class HttpServer;
class HttpRequest;
struct HttpResponse;

// Internal HTTP/captive-portal transport for ProvisioningManager.
//
// Owns: route registration on the borrowed HttpServer, scan-result
// caching, JSON parse/build for the portal API. Does NOT own the
// HttpServer, the captive DNS responder, or the provisioning state
// machine.
//
// The manager wires it up:
//   * provides callbacks for credential submission and scan triggering
//   * pushes scan results into update_scan_results() when WifiManager
//     reports them
//   * sets the visible status via set_state() so GET /api/status reflects
//     the live state machine
//
// All routes are exact-match (no wildcards). Captive-portal redirect
// targets (e.g. /generate_204, /hotspot-detect.html) are handled by the
// DNS responder pointing every name at the AP IP plus a catch-all root
// route below.
class WifiPortalTransport {
public:
  enum class PortalState : uint8_t {
    Waiting,
    Connecting,
    Connected,
    Failed,
  };

  using CredentialsCallback = std::function<bool(const ProvisioningData &)>;
  using ScanRequestCallback = std::function<bool()>;

  WifiPortalTransport() = default;
  ~WifiPortalTransport() = default;

  WifiPortalTransport(const WifiPortalTransport &) = delete;
  WifiPortalTransport &operator=(const WifiPortalTransport &) = delete;

  // Wire callbacks invoked by the HTTP handlers. on_credentials must
  // return true if the credentials were accepted (state machine
  // transitioned). on_scan_request must return true if a scan was
  // started.
  void set_on_credentials(CredentialsCallback cb) { _on_credentials = std::move(cb); }
  void set_on_scan_request(ScanRequestCallback cb) { _on_scan_request = std::move(cb); }

  // Register all portal HTTP routes on `http`. Must be called before
  // the server is started. Returns false if any route registration
  // fails. The flash-embedded portal.html bytes are supplied by the
  // caller (ProvisioningManager owns the linker symbols).
  bool register_routes(HttpServer &http, const uint8_t *html_start, const uint8_t *html_end);

  // Update the cached scan results so GET /api/scan returns them.
  // Result buffer ownership stays with the caller — entries are copied.
  // Pass count=0 to mark the scan as done with no networks found.
  void update_scan_results(const WifiScanEntry *entries, uint16_t count);

  // Update the visible portal state for GET /api/status.
  void set_state(PortalState state);

  // True between a POST /api/scan and the matching scan-complete update.
  bool scan_in_progress() const { return _scan_in_progress; }

  // --- Visible for tests --------------------------------------------------
  //
  // The portal handlers are exposed so host tests can invoke them with
  // a TestHttpRequest directly, without spinning up an HttpServer.

  void handle_scan_post(const HttpRequest &req, HttpResponse &resp);
  void handle_scan_get(const HttpRequest &req, HttpResponse &resp);
  void handle_provision_post(const HttpRequest &req, HttpResponse &resp);
  void handle_status_get(const HttpRequest &req, HttpResponse &resp);

  // Captive-portal OS probe handler. Returns 302 Found with
  // `Location: /` so the probing OS (iOS, Android, Windows, Firefox)
  // opens its in-app captive-portal browser pointed at the portal
  // page instead of treating the network as broken.
  static void handle_captive_probe(const HttpRequest &req, HttpResponse &resp);

private:
  enum class ScanState : uint8_t { Idle, Scanning, Done };

  CredentialsCallback _on_credentials;
  ScanRequestCallback _on_scan_request;

  // Scan cache. Sized for the filter cap (30 networks).
  static constexpr size_t MAX_CACHED_SCAN = 30;
  WifiScanEntry _scan_cache[MAX_CACHED_SCAN] = {};
  size_t _scan_cache_size = 0;
  ScanState _scan_state = ScanState::Idle;
  bool _scan_in_progress = false;

  PortalState _state = PortalState::Waiting;
};

#endif // AG_PROVISIONING_WIFI_PORTAL_TRANSPORT_H
