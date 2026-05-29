/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PAYLOAD_SERIALIZER_H
#define AG_PAYLOAD_SERIALIZER_H

#include <cstddef>

#include "../types/client_types.h"

// Serialize MeasuresInput to AirGradient HTTP JSON.  Only fields passing
// is_*_valid() are emitted; dual-channel fields are averaged when both
// channels are valid, otherwise the single valid channel is used.
// Writes NUL-terminated JSON; returns false on alloc failure or if `out`
// is too small (*bytes_written = 0).
bool serialize_measures_json(const MeasuresInput &input, int signal, char *out, size_t out_size,
                             size_t *bytes_written);

#endif // AG_PAYLOAD_SERIALIZER_H
