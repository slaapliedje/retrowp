#ifndef WP_CAPS_CODEC_H
#define WP_CAPS_CODEC_H

#include "types.h"
#include "offload.h"

/* Wire byte layout for a WIRE_MSG_CAPS payload (full protocol writeup in
   docs/formats/offload-protocol.md), big-endian throughout:
     offset  size  field
     0       2     protocol_version
     2       4     ops_supported      (bitmask, bit (1uL << offload_op))
     6       4     max_payload_bytes
     10      32    gateway_name       (UTF-8, NUL-padded)
   Lives under offload/transport (not engine/) so both the offload client
   (engine/src/offload_client.c) and the gateway (offload/gateway) can
   compile and link it without either depending on the other's build --
   offload/gateway links this file directly, never engine/src. */

#define CAPS_PAYLOAD_SIZE 42u

void caps_encode(const offload_capabilities *caps, u8 *out /* CAPS_PAYLOAD_SIZE bytes */);
void caps_decode(const u8 *buf /* CAPS_PAYLOAD_SIZE bytes */, offload_capabilities *out);

#endif /* WP_CAPS_CODEC_H */
