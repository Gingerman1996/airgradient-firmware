/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_QR_H
#define AG_PROVISIONING_QR_H

#include <cstddef>
#include <cstdint>

// Shared QR-code data for the Provisioning screen.  Encoding is shared
// across products; rendering (pixel size, quiet zone, origin) is each
// product's responsibility — it depends on panel geometry and graphics
// stack.  Two payloads are supported: a compile-time companion-app URL
// and a runtime "WIFI:" join descriptor that embeds the per-device
// SoftAP SSID.

namespace AirgradientProvisioning {

// Max QR version we encode.  v6 = 41x41 modules.  Buffer ~212 B.
inline constexpr int QR_MAX_VERSION = 6;

// Mirrors qrcodegen_BUFFER_LEN_FOR_VERSION(n) so callers can size
// QrCode without including the vendor header.
inline constexpr std::size_t QR_BUFFER_LEN =
    ((QR_MAX_VERSION * 4 + 17) * (QR_MAX_VERSION * 4 + 17) + 7) / 8 + 1;

enum class WifiAuth : uint8_t {
  None = 0, // open network ("nopass")
  Wep,
  Wpa, // WPA/WPA2/WPA3
};

// Caller-owned QR state.  Zero-initialize; size() returns 0 until a
// successful encode populates the buffer.
struct QrCode {
  uint8_t buffer[QR_BUFFER_LEN] = {};

  // Module count per side; 0 means no valid matrix (never encoded or
  // last encode failed).
  int size() const;

  // True when module (x, y) is dark.  Origin top-left; out-of-range
  // returns false.
  bool module_on(int x, int y) const;
};

// Encode the companion-app deep-link URL.  Returns true with the
// current compile-time URL.
bool encode_go_to_app_qr(QrCode *out);

// Encode "WIFI:T:<auth>;S:<ssid>;P:<password>;;".  Escapes `\ ; , : "`
// in ssid/password per the Wi-Fi Alliance spec.  Returns false (and
// zeros out) on null/empty ssid, payload overflow, or QR-version
// overflow.
bool encode_wifi_qr(const char *ssid, const char *password, WifiAuth auth, QrCode *out);

} // namespace AirgradientProvisioning

#endif // AG_PROVISIONING_QR_H
