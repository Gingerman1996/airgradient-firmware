/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "internal/dns_packet.h"

namespace {

// Build a minimal DNS query for "example.com" of TYPE A, CLASS IN.
//   header (12 bytes) | QNAME | QTYPE (2) | QCLASS (2)
// QNAME for example.com: 07 'example' 03 'com' 00 = 13 bytes.
size_t make_query(uint8_t *buf, size_t cap, uint16_t txid) {
  static const uint8_t qname[] = {
      7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
  };
  const size_t qname_len = sizeof(qname);
  const size_t total = 12 + qname_len + 4;
  if (cap < total) {
    return 0;
  }
  buf[0] = static_cast<uint8_t>(txid >> 8);
  buf[1] = static_cast<uint8_t>(txid & 0xff);
  buf[2] = 0x01; // flags: RD=1, standard query
  buf[3] = 0x00;
  buf[4] = 0x00;
  buf[5] = 0x01; // QDCOUNT = 1
  buf[6] = 0x00;
  buf[7] = 0x00; // ANCOUNT
  buf[8] = 0x00;
  buf[9] = 0x00; // NSCOUNT
  buf[10] = 0x00;
  buf[11] = 0x00; // ARCOUNT
  std::memcpy(buf + 12, qname, qname_len);
  buf[12 + qname_len] = 0x00;
  buf[12 + qname_len + 1] = 0x01; // QTYPE = A
  buf[12 + qname_len + 2] = 0x00;
  buf[12 + qname_len + 3] = 0x01; // QCLASS = IN
  return total;
}

} // namespace

TEST_CASE("DnsPacket parses a well-formed query", "[dns]") {
  uint8_t query[64];
  const size_t len = make_query(query, sizeof(query), 0x1234);
  REQUIRE(len > 0);

  DnsPacket::ParsedQuery parsed;
  REQUIRE(DnsPacket::parse_query(query, len, parsed));
  REQUIRE(parsed.transaction_id == 0x1234);
  REQUIRE(parsed.question_offset == 12);
  REQUIRE(parsed.question_length == len - 12);
}

TEST_CASE("DnsPacket rejects undersized buffers", "[dns]") {
  uint8_t buf[8] = {};
  DnsPacket::ParsedQuery parsed;
  REQUIRE_FALSE(DnsPacket::parse_query(buf, sizeof(buf), parsed));
  REQUIRE_FALSE(DnsPacket::parse_query(nullptr, 100, parsed));
}

TEST_CASE("DnsPacket rejects responses (QR=1)", "[dns]") {
  uint8_t query[64];
  const size_t len = make_query(query, sizeof(query), 0x1111);
  // Flip the QR bit so it looks like a response.
  query[2] |= 0x80;

  DnsPacket::ParsedQuery parsed;
  REQUIRE_FALSE(DnsPacket::parse_query(query, len, parsed));
}

TEST_CASE("DnsPacket rejects queries with multiple questions", "[dns]") {
  uint8_t query[64];
  const size_t len = make_query(query, sizeof(query), 0x2222);
  query[4] = 0x00;
  query[5] = 0x02; // QDCOUNT = 2

  DnsPacket::ParsedQuery parsed;
  REQUIRE_FALSE(DnsPacket::parse_query(query, len, parsed));
}

TEST_CASE("DnsPacket builds a valid A-record response", "[dns]") {
  uint8_t query[64];
  const size_t qlen = make_query(query, sizeof(query), 0xabcd);
  DnsPacket::ParsedQuery parsed;
  REQUIRE(DnsPacket::parse_query(query, qlen, parsed));

  // AP IP 192.168.4.1 in network byte order (octet 0 in low byte).
  const uint32_t ap_ip_be = (192) | (168 << 8) | (4 << 16) | (1u << 24);

  uint8_t response[128] = {};
  const size_t rlen =
      DnsPacket::build_a_response(query, qlen, parsed, ap_ip_be, 60, response, sizeof(response));
  REQUIRE(rlen > qlen);

  // Transaction ID echoed.
  REQUIRE(response[0] == 0xab);
  REQUIRE(response[1] == 0xcd);
  // QR=1 in flags.
  REQUIRE((response[2] & 0x80) == 0x80);
  // ANCOUNT == 1.
  REQUIRE(response[6] == 0x00);
  REQUIRE(response[7] == 0x01);

  // Answer rdata = the AP IP, in dotted-decimal order.
  const uint8_t *rdata = response + rlen - 4;
  REQUIRE(rdata[0] == 192);
  REQUIRE(rdata[1] == 168);
  REQUIRE(rdata[2] == 4);
  REQUIRE(rdata[3] == 1);
}

TEST_CASE("DnsPacket rejects undersized output buffers", "[dns]") {
  uint8_t query[64];
  const size_t qlen = make_query(query, sizeof(query), 0xbeef);
  DnsPacket::ParsedQuery parsed;
  REQUIRE(DnsPacket::parse_query(query, qlen, parsed));

  uint8_t tiny[10] = {};
  REQUIRE(DnsPacket::build_a_response(query, qlen, parsed, 0, 60, tiny, sizeof(tiny)) == 0);
}
