/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "dns_packet.h"

#include <cstring>

namespace DnsPacket {

namespace {

constexpr size_t HEADER_SIZE = 12;
constexpr size_t QUESTION_SUFFIX_SIZE = 4; // QTYPE (2) + QCLASS (2)
constexpr size_t MIN_QUERY_SIZE = HEADER_SIZE + 1 + QUESTION_SUFFIX_SIZE;
constexpr uint16_t FLAG_QR_MASK = 0x8000;

// Length of the A-record answer appended after the question section.
//   NAME (compression pointer to question, 2 bytes)
//   TYPE (2)  CLASS (2)  TTL (4)  RDLENGTH (2)  RDATA (4)
constexpr size_t ANSWER_SIZE = 2 + 2 + 2 + 4 + 2 + 4;

uint16_t read_u16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]));
}

void write_u16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[1] = static_cast<uint8_t>(v & 0xff);
}

void write_u32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>((v >> 24) & 0xff);
  p[1] = static_cast<uint8_t>((v >> 16) & 0xff);
  p[2] = static_cast<uint8_t>((v >> 8) & 0xff);
  p[3] = static_cast<uint8_t>(v & 0xff);
}

} // namespace

bool parse_query(const uint8_t *data, size_t len, ParsedQuery &out) {
  if (data == nullptr || len < MIN_QUERY_SIZE || len > MAX_MESSAGE_SIZE) {
    return false;
  }

  const uint16_t flags = read_u16(data + 2);
  if ((flags & FLAG_QR_MASK) != 0) {
    return false; // not a query
  }

  const uint16_t qdcount = read_u16(data + 4);
  if (qdcount != 1) {
    return false;
  }

  // Walk QNAME labels.
  size_t pos = HEADER_SIZE;
  while (pos < len) {
    const uint8_t label_len = data[pos];
    if (label_len == 0) {
      // Reached terminating zero label.
      ++pos;
      break;
    }
    // Reject compression pointers in queries — clients should not send
    // them, and supporting them is unnecessary complexity here.
    if ((label_len & 0xc0) != 0) {
      return false;
    }
    if (label_len > 63) {
      return false;
    }
    pos += 1 + label_len;
    if (pos >= len) {
      return false;
    }
  }

  // After the terminating zero we expect QTYPE + QCLASS.
  if (pos + QUESTION_SUFFIX_SIZE > len) {
    return false;
  }
  pos += QUESTION_SUFFIX_SIZE;

  out.transaction_id = read_u16(data);
  out.question_offset = HEADER_SIZE;
  out.question_length = pos - HEADER_SIZE;
  return true;
}

size_t build_a_response(const uint8_t *query, size_t query_len, const ParsedQuery &parsed,
                        uint32_t ipv4_be, uint32_t ttl_seconds, uint8_t *out, size_t out_cap) {
  if (query == nullptr || out == nullptr) {
    return 0;
  }
  if (parsed.question_offset != HEADER_SIZE) {
    return 0;
  }
  if (parsed.question_length == 0 || parsed.question_offset + parsed.question_length > query_len) {
    return 0;
  }

  const size_t needed = HEADER_SIZE + parsed.question_length + ANSWER_SIZE;
  if (out_cap < needed || needed > MAX_MESSAGE_SIZE) {
    return 0;
  }

  // Header.
  write_u16(out, parsed.transaction_id);
  // Flags: QR=1 (response), Opcode=0, AA=1, TC=0, RD=1, RA=1, RCODE=0.
  // 0x8580: standard authoritative answer with recursion available.
  write_u16(out + 2, 0x8580);
  write_u16(out + 4, 1);  // QDCOUNT
  write_u16(out + 6, 1);  // ANCOUNT
  write_u16(out + 8, 0);  // NSCOUNT
  write_u16(out + 10, 0); // ARCOUNT

  // Echo the question section verbatim.
  std::memcpy(out + HEADER_SIZE, query + parsed.question_offset, parsed.question_length);

  uint8_t *ans = out + HEADER_SIZE + parsed.question_length;

  // NAME — compression pointer back to the question's qname at offset
  // HEADER_SIZE (0x0c). 0xc0 = pointer flag, 0x0c = offset.
  ans[0] = 0xc0;
  ans[1] = static_cast<uint8_t>(HEADER_SIZE);
  write_u16(ans + 2, 1); // TYPE  = A
  write_u16(ans + 4, 1); // CLASS = IN
  write_u32(ans + 6, ttl_seconds);
  write_u16(ans + 10, 4); // RDLENGTH = 4 bytes (IPv4)
  // RDATA — IPv4 in network byte order. ipv4_be holds the address with
  // octet 0 in the low byte (matches lwIP ip_addr_t representation on
  // little-endian targets), so write octets in network order.
  ans[12] = static_cast<uint8_t>(ipv4_be & 0xff);
  ans[13] = static_cast<uint8_t>((ipv4_be >> 8) & 0xff);
  ans[14] = static_cast<uint8_t>((ipv4_be >> 16) & 0xff);
  ans[15] = static_cast<uint8_t>((ipv4_be >> 24) & 0xff);

  return needed;
}

} // namespace DnsPacket
