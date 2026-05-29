/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_MQTT_CLIENT_H
#define AG_MQTT_CLIENT_H

#include <cstddef>
#include <cstdint>

// MqttClient is consumed by AgClient.  Backends are future work.
class MqttClient {
public:
  virtual ~MqttClient() = default;

  virtual bool connect(const char *client_id, const char *host, int port, const char *username,
                       const char *password) = 0;
  virtual bool disconnect() = 0;
  virtual bool publish(const char *topic, const uint8_t *payload, size_t len, int qos) = 0;
};

#endif // AG_MQTT_CLIENT_H
