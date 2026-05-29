/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "captive_dns_responder.h"

#include "ag_log.h"
#include "dns_packet.h"

#ifndef TEST_HOST

#include "lwip/igmp.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"

#include <cstring>

namespace {

constexpr const char *TAG = "CaptiveDns";
constexpr uint16_t DNS_PORT = 53;
constexpr uint32_t DNS_TTL_SECONDS = 60;

// AP IP shared with the lwIP receive callback. Only one provisioning
// session runs at a time, so a single static here is safe.
uint32_t g_captive_dns_ap_ip_be = 0;

void on_recv(void * /*arg*/, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr,
             u16_t port) {
  if (p == nullptr) {
    return;
  }

  AG_LOGD(TAG, "rx query: %u bytes from port %u", static_cast<unsigned>(p->tot_len),
          static_cast<unsigned>(port));

  uint8_t request[DnsPacket::MAX_MESSAGE_SIZE];
  uint8_t response[DnsPacket::MAX_MESSAGE_SIZE];

  const u16_t copied = pbuf_copy_partial(p, request, sizeof(request), 0);
  pbuf_free(p);

  if (copied == 0 || copied > sizeof(request)) {
    return;
  }

  DnsPacket::ParsedQuery parsed;
  if (!DnsPacket::parse_query(request, copied, parsed)) {
    AG_LOGW(TAG, "drop malformed query (%u bytes)", static_cast<unsigned>(copied));
    return;
  }

  const size_t resp_len = DnsPacket::build_a_response(
      request, copied, parsed, g_captive_dns_ap_ip_be, DNS_TTL_SECONDS, response, sizeof(response));
  if (resp_len == 0) {
    return;
  }

  AG_LOGD(TAG, "responding with AP IP (txid=0x%04x)", static_cast<unsigned>(parsed.transaction_id));

  struct pbuf *out = pbuf_alloc(PBUF_TRANSPORT, static_cast<u16_t>(resp_len), PBUF_RAM);
  if (out == nullptr) {
    return;
  }
  std::memcpy(out->payload, response, resp_len);
  udp_sendto(pcb, out, addr, port);
  pbuf_free(out);
}

} // namespace

CaptiveDnsResponder::~CaptiveDnsResponder() { stop(); }

bool CaptiveDnsResponder::start(uint32_t ap_ipv4_be) {
  if (_running) {
    return true;
  }

  struct udp_pcb *pcb = udp_new();
  if (pcb == nullptr) {
    AG_LOGE(TAG, "udp_new failed");
    return false;
  }
  if (udp_bind(pcb, IP_ADDR_ANY, DNS_PORT) != ERR_OK) {
    AG_LOGE(TAG, "udp_bind(:53) failed");
    udp_remove(pcb);
    return false;
  }

  _pcb = pcb;
  _ap_ipv4_be = ap_ipv4_be;
  g_captive_dns_ap_ip_be = ap_ipv4_be;
  udp_recv(pcb, &on_recv, this);
  _running = true;
  AG_LOGI(TAG, "captive DNS listening on UDP :53");
  return true;
}

void CaptiveDnsResponder::stop() {
  if (!_running) {
    return;
  }
  if (_pcb != nullptr) {
    udp_remove(static_cast<struct udp_pcb *>(_pcb));
    _pcb = nullptr;
  }
  _ap_ipv4_be = 0;
  g_captive_dns_ap_ip_be = 0;
  _running = false;
}

#else // TEST_HOST

// Host-test stub. The DNS codec is covered by dns_packet tests; the
// responder itself is hardware-only.
CaptiveDnsResponder::~CaptiveDnsResponder() = default;

bool CaptiveDnsResponder::start(uint32_t ap_ipv4_be) {
  _ap_ipv4_be = ap_ipv4_be;
  _running = true;
  return true;
}

void CaptiveDnsResponder::stop() {
  _running = false;
  _ap_ipv4_be = 0;
}

#endif // TEST_HOST
