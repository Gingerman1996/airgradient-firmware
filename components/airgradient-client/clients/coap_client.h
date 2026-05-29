/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_COAP_CLIENT_H
#define AG_COAP_CLIENT_H

#include <cstddef>
#include <cstdint>

// CoapClient encapsulates packet build/parse, CON/ACK, Block1 chunking,
// retry, and DNS fallback.  AgClient never sees CoAP internals.
// Backends are future work.
class CoapClient {
public:
  virtual ~CoapClient() = default;

  // Caller sizes the buffer to the maximum expected response; no
  // truncation reporting.
  virtual bool get(const char *host, int port, const char *uri_path, char *response_body,
                   size_t body_size, size_t *bytes_written) = 0;

  // Handles Block1 internally.  response_code_class/detail split the
  // CoAP response code (e.g. 2.04 -> class=2, detail=4).
  virtual bool post(const char *host, int port, const char *uri_path, const uint8_t *body,
                    size_t body_len, int &response_code_class, int &response_code_detail) = 0;
};

#endif // AG_COAP_CLIENT_H
