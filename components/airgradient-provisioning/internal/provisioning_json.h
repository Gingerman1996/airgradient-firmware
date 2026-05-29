/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_JSON_H
#define AG_PROVISIONING_JSON_H

#include <cstdint>
#include <cstdio>

#include "../types/provisioning_types.h"

struct cJSON;

// Parse a dotted-decimal IPv4 string (e.g. "192.168.1.100") into a
// uint32_t in network byte order (octet 0 in the low byte — matches
// lwIP ip4_addr_t and WifiStaticIpConfig). Returns true on success.
inline bool parse_ipv4(const char *s, uint32_t &out_be) {
  if (s == nullptr) {
    return false;
  }
  unsigned a = 0, b = 0, c = 0, d = 0;
  int matched = std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
  if (matched != 4) {
    return false;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255) {
    return false;
  }
  out_be = (static_cast<uint32_t>(a)) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
  return true;
}

// Result of parsing a provisioning credential JSON payload.
enum class ProvisioningJsonError : uint8_t {
  Ok,
  MissingSsid,
  InvalidPassword,
  InvalidStaticIp,
};

// Parse a cJSON object into ProvisioningData. Shared by both the Wi-Fi
// portal transport (HTTP POST /api/provision) and the BLE transport
// (credential characteristic write).
//
// Expected JSON fields:
//   "ssid"         — required, non-empty string
//   "password"     — optional. Empty means open network; otherwise must
//                    be 8..63 characters (WPA-PSK range).
//   "disableCloud" — optional bool, defaults to false
//   "staticIp"     — optional object with "ip", "netmask", "gateway", "dns"
//
// The caller owns the cJSON root and must cJSON_Delete() it afterward.
ProvisioningJsonError parse_provisioning_json(cJSON *root, ProvisioningData &out);

#endif // AG_PROVISIONING_JSON_H
