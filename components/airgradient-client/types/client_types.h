/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_CLIENT_TYPES_H
#define AG_CLIENT_TYPES_H

#include "measures_types.h"

enum class NetworkType {
  Wifi,
  Cellular,
};

enum class AgClientResult {
  Ok,             // Operation succeeded (HTTP 200, 201, or 429)
  BufferTooSmall, // Response did not fit in caller's buffer
  TransportError, // Could not reach server (connection, DNS, timeout)
  ServerError,    // Non-success HTTP status (generic)
  NotRegistered,  // Server returned 400 -- device not registered
};

// Pointer bundle for the AgClient serializer.  Null fields are omitted.
// Non-owning -- caller's storage must outlive use.
struct MeasuresInput {
  const TempHumData *temp_hum_a = nullptr;
  const TempHumData *temp_hum_b = nullptr;
  const PMData *pm_a = nullptr;
  const PMData *pm_b = nullptr;
  const CO2Data *co2 = nullptr;
  const TVOCNOxData *tvoc_nox = nullptr;
  const MeasuresPower *power = nullptr;
  const O3No2Data *electrode = nullptr;
};

#endif // AG_CLIENT_TYPES_H
