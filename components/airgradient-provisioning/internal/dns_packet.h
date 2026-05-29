/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_DNS_PACKET_H
#define AG_PROVISIONING_DNS_PACKET_H

#include <cstddef>
#include <cstdint>

// Pure-C++ DNS query parser and A-record response builder used by the
// captive DNS responder. Operating on byte arrays keeps the codec
// independent of lwIP so it can be exercised in host tests.
//
// The captive responder only needs the bare minimum: parse the
// transaction ID and question section, then emit a single A-record
// response that points every query to the AP's IP address.
namespace DnsPacket {

// Maximum DNS message size we support (well below the 512-byte UDP limit
// for classic DNS — captive portal queries are tiny).
inline constexpr size_t MAX_MESSAGE_SIZE = 512;

struct ParsedQuery {
  uint16_t transaction_id = 0;
  // Offset where the question section begins (after the 12-byte header)
  // and length of the question section (qname + qtype + qclass).
  size_t question_offset = 0;
  size_t question_length = 0;
};

// Parse the incoming DNS query. Returns true on success. Performs only
// the validation needed by the captive responder:
//   - non-null buffer, length >= 12 (header) + 5 (smallest question)
//   - QR bit == 0 (this is a query, not a response)
//   - exactly one question (QDCOUNT == 1)
//   - QNAME fits within the buffer and terminates with a zero label
//
// On success, the transaction id and the byte range of the question
// section are stored in `out`.
bool parse_query(const uint8_t *data, size_t len, ParsedQuery &out);

// Build an A-record response into `out` of capacity `out_cap`. The
// response echoes the original transaction id and question section and
// appends a single A-record answer pointing to `ipv4_be` (IPv4 address,
// network byte order). `ttl_seconds` is encoded into the answer.
//
// Returns the number of bytes written to `out`, or 0 on failure
// (buffer too small, parsed offsets inconsistent, etc.).
size_t build_a_response(const uint8_t *query, size_t query_len, const ParsedQuery &parsed,
                        uint32_t ipv4_be, uint32_t ttl_seconds, uint8_t *out, size_t out_cap);

} // namespace DnsPacket

#endif // AG_PROVISIONING_DNS_PACKET_H
