/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_portal_transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <cJSON.h>

#include "ag_log.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "hal/http_server.h"
#include "provisioning_json.h"
#include "scan_filter.h"
#include "types/http_types.h"

namespace {

constexpr const char *TAG = "WifiPortal";

// Well-known OS captive-portal probe URLs. Each gets a GET handler that
// returns 302 Found → "/" so the probing OS opens its in-app captive
// browser at the actual portal page instead of treating the AP as a
// broken network. Covers iOS, macOS, Android (incl. OEM variants),
// Windows, Firefox, and a few NetworkManager variants.
constexpr const char *CAPTIVE_PROBE_PATHS[] = {
    "/hotspot-detect.html",       // iOS / macOS
    "/library/test/success.html", // iOS legacy
    "/generate_204",              // Android / Chrome
    "/gen_204",                   // Android variant
    "/connecttest.txt",           // Windows 10/11
    "/ncsi.txt",                  // Windows legacy
    "/canonical.html",            // Firefox
    "/redirect",                  // Some Android OEMs
    "/success.txt",               // Linux NetworkManager
};

const char *portal_state_string(WifiPortalTransport::PortalState s) {
  switch (s) {
  case WifiPortalTransport::PortalState::Waiting:
    return "waiting";
  case WifiPortalTransport::PortalState::Connecting:
    return "connecting";
  case WifiPortalTransport::PortalState::Connected:
    return "connected";
  case WifiPortalTransport::PortalState::Failed:
    return "failed";
  }
  return "waiting";
}

void send_json(HttpResponse &resp, HttpStatus status, cJSON *root) {
  char *encoded = cJSON_PrintUnformatted(root);
  if (encoded == nullptr) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"encode"})");
    return;
  }
  resp.json(status, encoded);
  cJSON_free(encoded);
}

} // namespace

void WifiPortalTransport::handle_captive_probe(const HttpRequest &req, HttpResponse &resp) {
  AG_LOGD(TAG, "captive probe hit: %s", req.uri());

  // 302 Found + absolute Location URL pointing directly at the AP.
  // iOS CNA (and similar OS captive-portal detectors) treat a probe
  // response as a captive signal only when the Location is a fully
  // qualified URL to a different host. A relative redirect like
  // `Location: /` is followed silently and the popup never appears.
  //
  // The AP IP is the lwIP soft-AP default (192.168.4.1); keep this in
  // sync with DEFAULT_AP_IP_BE in provisioning_manager.cpp if it ever
  // changes.
  resp.set_header("Location", "http://192.168.4.1/");
  // Body is irrelevant for a redirect, but we set content-type to avoid
  // the response defaulting to 500's content type via HttpResponse.
  resp.body(HttpStatus::Found, "", 0, "text/plain");
}

bool WifiPortalTransport::register_routes(HttpServer &http, const uint8_t *html_start,
                                          const uint8_t *html_end) {
  bool ok = true;
  if (html_start != nullptr && html_end != nullptr && html_end > html_start) {
    ok &= http.register_static("/", html_start, html_end, "text/html");
    ok &= http.register_static("/index.html", html_start, html_end, "text/html");
  }
  ok &= http.register_route(
      HttpMethod::Post, "/api/scan",
      [this](const HttpRequest &q, HttpResponse &r) { handle_scan_post(q, r); });
  ok &=
      http.register_route(HttpMethod::Get, "/api/scan",
                          [this](const HttpRequest &q, HttpResponse &r) { handle_scan_get(q, r); });
  ok &= http.register_route(
      HttpMethod::Post, "/api/provision",
      [this](const HttpRequest &q, HttpResponse &r) { handle_provision_post(q, r); });
  ok &= http.register_route(
      HttpMethod::Get, "/api/status",
      [this](const HttpRequest &q, HttpResponse &r) { handle_status_get(q, r); });

  // OS captive-portal probe URLs — all redirect to the portal page.
  constexpr size_t probe_count = sizeof(CAPTIVE_PROBE_PATHS) / sizeof(CAPTIVE_PROBE_PATHS[0]);
  for (const char *path : CAPTIVE_PROBE_PATHS) {
    ok &= http.register_route(HttpMethod::Get, path, &handle_captive_probe);
  }
  // Silence the browser favicon probe (one less 404 in the logs).
  ok &= http.register_route(HttpMethod::Get, "/favicon.ico",
                            [](const HttpRequest &, HttpResponse &r) { r.no_content(); });

  if (ok) {
    AG_LOGI(TAG, "portal routes registered (api + %u captive probes)",
            static_cast<unsigned>(probe_count));
  } else {
    AG_LOGE(TAG, "portal route registration failed (some routes rejected)");
  }
  return ok;
}

void WifiPortalTransport::update_scan_results(const WifiScanEntry *entries, uint16_t count) {
  _scan_cache_size = ScanFilter::apply(entries, count, _scan_cache, MAX_CACHED_SCAN);
  _scan_state = ScanState::Done;
  _scan_in_progress = false;
  AG_LOGD(TAG, "portal scan cache updated: %u networks", static_cast<unsigned>(_scan_cache_size));
}

void WifiPortalTransport::set_state(PortalState state) { _state = state; }

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void WifiPortalTransport::handle_scan_post(const HttpRequest &req, HttpResponse &resp) {
  (void)req;
  AG_LOGI(TAG, "HTTP /api/scan POST — scan requested");
  bool started = false;
  if (_on_scan_request) {
    started = _on_scan_request();
  }
  if (!started) {
    AG_LOGE(TAG, "HTTP /api/scan POST — scan not started");
    resp.json(HttpStatus::InternalServerError, R"({"status":"error"})");
    return;
  }
  _scan_state = ScanState::Scanning;
  _scan_in_progress = true;
  // Invalidate previous results so a stale list isn't returned mid-scan.
  _scan_cache_size = 0;
  resp.json(HttpStatus::Ok, R"({"status":"scanning"})");
}

void WifiPortalTransport::handle_scan_get(const HttpRequest &req, HttpResponse &resp) {
  (void)req;
  AG_LOGD(TAG, "HTTP /api/scan GET state=%u networks=%u", static_cast<unsigned>(_scan_state),
          static_cast<unsigned>(_scan_cache_size));
  if (_scan_state == ScanState::Idle) {
    resp.json(HttpStatus::Ok, R"({"status":"idle"})");
    return;
  }
  if (_scan_state == ScanState::Scanning) {
    resp.json(HttpStatus::Ok, R"({"status":"scanning"})");
    return;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *networks = cJSON_CreateArray();
  if (root == nullptr || networks == nullptr) {
    if (root != nullptr)
      cJSON_Delete(root);
    if (networks != nullptr)
      cJSON_Delete(networks);
    resp.json(HttpStatus::InternalServerError, R"({"error":"encode"})");
    return;
  }
  cJSON_AddStringToObject(root, "status", "done");

  for (size_t i = 0; i < _scan_cache_size; ++i) {
    const WifiScanEntry &e = _scan_cache[i];
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ssid", e.ssid);
    cJSON_AddNumberToObject(entry, "rssi", static_cast<double>(e.rssi));
    cJSON_AddStringToObject(entry, "auth", wifi_auth_mode_to_string(e.auth_mode));
    cJSON_AddNumberToObject(entry, "channel", static_cast<double>(e.channel));
    cJSON_AddItemToArray(networks, entry);
  }
  cJSON_AddItemToObject(root, "networks", networks);

  send_json(resp, HttpStatus::Ok, root);
  cJSON_Delete(root);
}

void WifiPortalTransport::handle_provision_post(const HttpRequest &req, HttpResponse &resp) {
  const char *body = req.body();
  const size_t body_len = req.body_length();
  AG_LOGI(TAG, "HTTP /api/provision POST (%u bytes)", static_cast<unsigned>(body_len));
  if (body == nullptr || body_len == 0) {
    AG_LOGW(TAG, "provision rejected: empty body");
    resp.json(HttpStatus::BadRequest, R"({"error":"empty body"})");
    return;
  }

  cJSON *root = cJSON_ParseWithLength(body, body_len);
  if (root == nullptr) {
    AG_LOGW(TAG, "provision rejected: invalid json");
    resp.json(HttpStatus::BadRequest, R"({"error":"invalid json"})");
    return;
  }

  ProvisioningData data;
  ProvisioningJsonError err = parse_provisioning_json(root, data);
  cJSON_Delete(root);

  if (err == ProvisioningJsonError::MissingSsid) {
    AG_LOGW(TAG, "provision rejected: missing ssid");
    resp.json(HttpStatus::BadRequest, R"({"error":"missing ssid"})");
    return;
  }
  if (err == ProvisioningJsonError::InvalidPassword) {
    AG_LOGW(TAG, "provision rejected: invalid password");
    resp.json(HttpStatus::BadRequest, R"({"error":"password must be 8..63 characters"})");
    return;
  }
  if (err == ProvisioningJsonError::InvalidStaticIp) {
    AG_LOGW(TAG, "provision rejected: invalid staticIp");
    resp.json(HttpStatus::BadRequest, R"({"error":"invalid staticIp"})");
    return;
  }

  bool accepted = false;
  if (_on_credentials) {
    accepted = _on_credentials(data);
  }
  if (!accepted) {
    // Either state machine is busy or no callback wired. Tell the
    // client and let it poll status.
    AG_LOGW(TAG, "provision rejected: state busy");
    resp.json(HttpStatus::Ok, R"({"status":"busy"})");
    return;
  }
  AG_LOGI(TAG, "provision accepted: ssid='%s'", data.ssid);
  resp.json(HttpStatus::Ok, R"({"status":"connecting"})");
}

void WifiPortalTransport::handle_status_get(const HttpRequest &req, HttpResponse &resp) {
  (void)req;
  AG_LOGD(TAG, "HTTP /api/status state=%s", portal_state_string(_state));
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), R"({"state":"%s"})", portal_state_string(_state));
  if (n <= 0) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"snprintf"})");
    return;
  }
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}
