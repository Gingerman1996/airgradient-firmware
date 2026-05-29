/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/provisioning_qr.h"

#include <cstring>

extern "C" {
#include "qrcodegen.h"
}

namespace AirgradientProvisioning {

namespace {

constexpr const char *GO_TO_APP_URL = "https://www.airgradient.com/go-to-app";

// Worst-case "WIFI:" payload: prefix + escaped SSID (32 -> 64) + ";P:"
// + escaped password (63 -> 126) + ";;" = ~208 B.  224 leaves headroom.
constexpr std::size_t WIFI_PAYLOAD_CAP = 224;

// Build "WIFI:T:<auth>;S:<ssid>;P:<password>;;" into out.  Reserved
// chars in ssid/password (`\ ; , : "`) are backslash-escaped per the
// Wi-Fi Alliance spec.  Returns bytes written (excluding NUL), or 0 on
// invalid input or overflow.
std::size_t format_wifi_payload(char *out, std::size_t cap, const char *ssid, const char *password,
                                WifiAuth auth) {
  if (out == nullptr || cap == 0 || ssid == nullptr || ssid[0] == '\0') {
    return 0;
  }

  std::size_t n = 0;
  auto put = [&](char c) -> bool {
    if (n + 1 >= cap) {
      return false;
    }
    out[n++] = c;
    return true;
  };
  auto put_str = [&](const char *s) -> bool {
    while (s != nullptr && *s != '\0') {
      if (!put(*s++)) {
        return false;
      }
    }
    return true;
  };
  auto put_escaped = [&](const char *s) -> bool {
    while (s != nullptr && *s != '\0') {
      const char c = *s++;
      const bool needs_escape = (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"');
      if (needs_escape && !put('\\')) {
        return false;
      }
      if (!put(c)) {
        return false;
      }
    }
    return true;
  };

  const char *auth_tag = nullptr;
  switch (auth) {
  case WifiAuth::None:
    auth_tag = "nopass";
    break;
  case WifiAuth::Wep:
    auth_tag = "WEP";
    break;
  case WifiAuth::Wpa:
    auth_tag = "WPA";
    break;
  }

  if (!put_str("WIFI:T:") || !put_str(auth_tag) || !put_str(";S:") || !put_escaped(ssid) ||
      !put_str(";P:")) {
    return 0;
  }
  if (auth != WifiAuth::None && password != nullptr) {
    if (!put_escaped(password)) {
      return 0;
    }
  }
  if (!put_str(";;")) {
    return 0;
  }
  out[n] = '\0';
  return n;
}

// Encode text into out at ECC LOW (boosted automatically), auto mask,
// up to QR_MAX_VERSION.  Zeros out on failure.
bool encode_text_into(const char *text, QrCode *out) {
  if (out == nullptr) {
    return false;
  }
  uint8_t temp[QR_BUFFER_LEN];
  const bool ok =
      qrcodegen_encodeText(text, temp, out->buffer, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                           QR_MAX_VERSION, qrcodegen_Mask_AUTO, /*boostEcl=*/true);
  if (!ok) {
    std::memset(out->buffer, 0, sizeof(out->buffer));
    return false;
  }
  return true;
}

} // namespace

int QrCode::size() const {
  // qrcodegen_getSize() asserts buffer[0] is in [21, 177]; short-circuit
  // on the zero sentinel so a never-encoded / failed QrCode is safe to
  // query.
  if (buffer[0] == 0) {
    return 0;
  }
  return qrcodegen_getSize(buffer);
}

bool QrCode::module_on(int x, int y) const {
  if (buffer[0] == 0) {
    return false;
  }
  return qrcodegen_getModule(buffer, x, y);
}

bool encode_go_to_app_qr(QrCode *out) { return encode_text_into(GO_TO_APP_URL, out); }

bool encode_wifi_qr(const char *ssid, const char *password, WifiAuth auth, QrCode *out) {
  if (out == nullptr) {
    return false;
  }
  char payload[WIFI_PAYLOAD_CAP];
  const std::size_t n = format_wifi_payload(payload, sizeof(payload), ssid, password, auth);
  if (n == 0) {
    std::memset(out->buffer, 0, sizeof(out->buffer));
    return false;
  }
  return encode_text_into(payload, out);
}

} // namespace AirgradientProvisioning
