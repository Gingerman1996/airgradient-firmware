/**
 * AirGradient Go — CloudService public types
 *
 * Standalone event-payload header for go_events.h.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_CLOUD_TYPES_H
#define GO_CLOUD_TYPES_H

#include <cstdint>

/// AgClientResult stored as uint8_t so go_events.h avoids the
/// airgradient-client header dependency.
using CloudResultByte = uint8_t;

#endif // GO_CLOUD_TYPES_H
