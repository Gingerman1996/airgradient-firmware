/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_CLIENT_TEST_ACCESS_H
#define AG_CLIENT_TEST_ACCESS_H

#include <cstring>

#include "services/ag_client.h"

// Friend test helper: inject mock protocol clients, bypass begin().
class AgClientTestAccess {
public:
  explicit AgClientTestAccess(AgClient &client) : _client(client) {}

  void inject_http_client(HttpClient *h) { _client._http = h; }
  void inject_mqtt_client(MqttClient *m) { _client._mqtt = m; }
  void inject_coap_client(CoapClient *c) { _client._coap = c; }

  void set_network(NetworkType n) { _client._network = n; }
  void set_serial_number(const char *sn) {
    std::strncpy(_client._serial_number, sn, sizeof(_client._serial_number) - 1);
    _client._serial_number[sizeof(_client._serial_number) - 1] = '\0';
  }
  void set_http_domain(const char *d) { _client._http_domain = d; }

private:
  AgClient &_client;
};

#endif // AG_CLIENT_TEST_ACCESS_H
