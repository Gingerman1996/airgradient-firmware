/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "provisioning_json.h"

#include <cstring>

#include <cJSON.h>

ProvisioningJsonError parse_provisioning_json(cJSON *root, ProvisioningData &out) {
  if (root == nullptr) {
    return ProvisioningJsonError::MissingSsid;
  }

  cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
  if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr || ssid->valuestring[0] == '\0') {
    return ProvisioningJsonError::MissingSsid;
  }

  out = ProvisioningData{};
  std::strncpy(out.ssid, ssid->valuestring, sizeof(out.ssid) - 1);

  cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
  if (cJSON_IsString(password) && password->valuestring != nullptr) {
    // Validate WPA-PSK length range but empty is fine (open network).
    const size_t pw_len = std::strlen(password->valuestring);
    if (pw_len > 0 && (pw_len < 8 || pw_len >= sizeof(out.password))) {
      return ProvisioningJsonError::InvalidPassword;
    }
    std::strncpy(out.password, password->valuestring, sizeof(out.password) - 1);
  }

  cJSON *disable_cloud = cJSON_GetObjectItemCaseSensitive(root, "disableCloud");
  if (cJSON_IsBool(disable_cloud)) {
    out.disable_cloud = cJSON_IsTrue(disable_cloud);
  }

  cJSON *static_ip = cJSON_GetObjectItemCaseSensitive(root, "staticIp");
  if (cJSON_IsObject(static_ip)) {
    cJSON *ip_node = cJSON_GetObjectItemCaseSensitive(static_ip, "ip");
    cJSON *netmask_node = cJSON_GetObjectItemCaseSensitive(static_ip, "netmask");
    cJSON *gateway_node = cJSON_GetObjectItemCaseSensitive(static_ip, "gateway");
    cJSON *dns_node = cJSON_GetObjectItemCaseSensitive(static_ip, "dns");

    bool any_invalid = false;
    if (cJSON_IsString(ip_node) && ip_node->valuestring != nullptr) {
      if (!parse_ipv4(ip_node->valuestring, out.static_ip.ip)) {
        any_invalid = true;
      }
    }
    if (cJSON_IsString(netmask_node) && netmask_node->valuestring != nullptr) {
      if (!parse_ipv4(netmask_node->valuestring, out.static_ip.netmask)) {
        any_invalid = true;
      }
    }
    if (cJSON_IsString(gateway_node) && gateway_node->valuestring != nullptr) {
      if (!parse_ipv4(gateway_node->valuestring, out.static_ip.gateway)) {
        any_invalid = true;
      }
    }
    if (cJSON_IsString(dns_node) && dns_node->valuestring != nullptr) {
      if (!parse_ipv4(dns_node->valuestring, out.static_ip.dns_primary)) {
        any_invalid = true;
      }
    }
    if (any_invalid || out.static_ip.ip == 0) {
      return ProvisioningJsonError::InvalidStaticIp;
    }
  }

  return ProvisioningJsonError::Ok;
}
