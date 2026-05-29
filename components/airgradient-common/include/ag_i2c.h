/**
 * AirGradient — Generic I²C helpers
 *
 * Thin wrappers around ESP-IDF I²C master primitives.  Under TEST_HOST
 * the bus handle becomes void* so tests can supply their own definitions.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_I2C_H
#define AG_I2C_H

#include <cstdint>

#ifndef TEST_HOST
#include <driver/i2c_master.h>
#endif

/// Probe an I²C device by 7-bit address.  Returns true when the device
/// ACKs within @p timeout_ms.  Thin wrapper around i2c_master_probe().
///
/// Recommended timeout for fast presence checks is 100 ms.
///
/// Under TEST_HOST the function is declared with a void* bus handle;
/// tests provide their own definition.
#ifndef TEST_HOST
bool i2c_device_present(i2c_master_bus_handle_t bus, uint8_t address, int timeout_ms);
#else
bool i2c_device_present(void *bus, uint8_t address, int timeout_ms);
#endif

#endif // AG_I2C_H
