/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_CAPTIVE_DNS_RESPONDER_H
#define AG_PROVISIONING_CAPTIVE_DNS_RESPONDER_H

#include <cstdint>

// Lightweight DNS responder that redirects every query to a fixed IPv4
// address. Used to trigger captive-portal detection on phones and
// browsers connected to the provisioning soft-AP.
//
// Backed by the lwIP raw UDP API on hardware (`udp_new`, `udp_bind`,
// `udp_recv`). Under TEST_HOST start()/stop() are no-ops; the
// DNS-packet codec is covered separately by dns_packet tests.
class CaptiveDnsResponder {
public:
  CaptiveDnsResponder() = default;
  ~CaptiveDnsResponder();

  CaptiveDnsResponder(const CaptiveDnsResponder &) = delete;
  CaptiveDnsResponder &operator=(const CaptiveDnsResponder &) = delete;

  // Bind to UDP port 53 on the soft-AP interface and start answering
  // queries with `ap_ipv4_be` (network byte order — i.e. lwIP's
  // ip4_addr_t representation). Returns false if binding fails or the
  // responder is already running.
  bool start(uint32_t ap_ipv4_be);

  // Tear down the UDP socket. Safe to call when not started.
  void stop();

  // True once start() has succeeded.
  bool is_running() const { return _running; }

private:
  void *_pcb = nullptr; // lwIP udp_pcb*, opaque to avoid leaking headers
  uint32_t _ap_ipv4_be = 0;
  bool _running = false;
};

#endif // AG_PROVISIONING_CAPTIVE_DNS_RESPONDER_H
