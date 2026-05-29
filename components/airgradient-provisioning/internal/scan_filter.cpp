/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "scan_filter.h"

#include <algorithm>
#include <cstring>

#include "ag_log.h"

namespace ScanFilter {

namespace {

constexpr const char *TAG = "ScanFilter";

bool ssid_empty(const WifiScanEntry &e) { return e.ssid[0] == '\0'; }

// Stable in-place insertion of `candidate`, capped at `capacity`.
// Duplicates by SSID keep the strongest RSSI. At capacity the candidate
// is dropped — safe given the RSSI-descending precondition on `in`
// (see scan_filter.h).
size_t insert_or_merge(WifiScanEntry *out, size_t current_size, size_t capacity,
                       const WifiScanEntry &candidate) {
  for (size_t i = 0; i < current_size; ++i) {
    if (std::strncmp(out[i].ssid, candidate.ssid, sizeof(out[i].ssid)) == 0) {
      if (candidate.rssi > out[i].rssi) {
        out[i] = candidate;
      }
      return current_size;
    }
  }
  if (current_size >= capacity) {
    return current_size;
  }
  out[current_size] = candidate;
  return current_size + 1;
}

} // namespace

size_t apply(const WifiScanEntry *in, uint16_t in_count, WifiScanEntry *out, size_t out_capacity) {
  if (out == nullptr || out_capacity == 0) {
    return 0;
  }
  const size_t hard_cap = std::min(out_capacity, SCAN_MAX_NETWORKS);
  size_t size = 0;

  if (in == nullptr || in_count == 0) {
    return 0;
  }

  for (uint16_t i = 0; i < in_count; ++i) {
    const WifiScanEntry &entry = in[i];
    if (ssid_empty(entry)) {
      continue;
    }
    if (entry.rssi < SCAN_MIN_RSSI_DBM) {
      continue;
    }
    size = insert_or_merge(out, size, hard_cap, entry);
  }

  // Sort by RSSI descending. Stable sort keeps insertion order for ties.
  std::stable_sort(out, out + size,
                   [](const WifiScanEntry &a, const WifiScanEntry &b) { return a.rssi > b.rssi; });

  AG_LOGI(TAG, "scan filter: in=%u, kept=%u (dedup+rssi_floor)", static_cast<unsigned>(in_count),
          static_cast<unsigned>(size));
  for (size_t i = 0; i < size; ++i) {
    AG_LOGI(TAG, "  [%2u] rssi=%4d ch=%2u auth=%s ssid='%s'", static_cast<unsigned>(i),
            static_cast<int>(out[i].rssi), static_cast<unsigned>(out[i].channel),
            wifi_auth_mode_to_string(out[i].auth_mode), out[i].ssid);
  }

  return size;
}

} // namespace ScanFilter
