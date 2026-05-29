/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_SCAN_FILTER_H
#define AG_PROVISIONING_SCAN_FILTER_H

#include <cstddef>
#include <cstdint>

#include "types/wifi_types.h"

// Shared pure-C++ utility used by both provisioning transports.
//
// Reduces a raw scan result array (from WifiManager::on_scan_complete) to
// a filtered, deduplicated view capped at SCAN_MAX_NETWORKS:
//   1. drop empty SSIDs and entries weaker than SCAN_MIN_RSSI_DBM
//   2. dedupe by SSID, keeping the strongest RSSI
//   3. cap at SCAN_MAX_NETWORKS (drops surplus in input order)
//   4. stable-sort by RSSI descending
//
// Precondition: `in` is sorted by RSSI descending — guaranteed by
// `esp_wifi_scan_get_ap_records()` and forwarded 1:1 by the Wi-Fi HAL.
// Without it, step 3 may drop a stronger AP that arrived after the cap.
//
// Operates in place on the caller's output buffer to avoid heap traffic.
namespace ScanFilter {

inline constexpr int8_t SCAN_MIN_RSSI_DBM = -75;
inline constexpr size_t SCAN_MAX_NETWORKS = 30;

// Apply the filter pipeline. `in` may alias `out`. Reads up to
// `in_count` entries from `in`, writes filtered entries into `out`, and
// returns the number written (0..SCAN_MAX_NETWORKS).
//
// Precondition: `in` is sorted by RSSI descending (see above).
size_t apply(const WifiScanEntry *in, uint16_t in_count, WifiScanEntry *out, size_t out_capacity);

} // namespace ScanFilter

#endif // AG_PROVISIONING_SCAN_FILTER_H
