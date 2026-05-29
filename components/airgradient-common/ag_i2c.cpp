/**
 * AirGradient — Generic I²C helpers (target implementation)
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TEST_HOST
#include "ag_i2c.h"

bool i2c_device_present(i2c_master_bus_handle_t bus, uint8_t address, int timeout_ms) {
  return i2c_master_probe(bus, address, timeout_ms) == ESP_OK;
}
#endif
