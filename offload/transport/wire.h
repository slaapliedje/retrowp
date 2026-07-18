#ifndef WP_WIRE_H
#define WP_WIRE_H

#include "types.h"

/* Length-framed, versioned, CRC-checked wire protocol shared by the
   offload client (engine/src/offload_client.c) and the Pi-side/mock
   gateway. This module is pure framing -- it never opens a socket, pipe,
   or serial port itself. It encodes/decodes byte buffers and is driven
   by a caller-supplied read callback (wire_read_fn), so it is reusable
   unchanged on both ends of the link and fully unit-testable with no
   real I/O. It has no dependency on platform.h or offload.h -- `op` and
   `status` are raw bytes here; callers cast to/from their own enums.

   Frame layout (all multi-byte fields big-endian):
     offset  size  field
     0       1     magic0    (WIRE_MAGIC0)
     1       1     magic1    (WIRE_MAGIC1)
     2       1     version   (WIRE_VERSION)
     3       1     type      (wire_msg_type)
     4       1     op        (offload_op; meaningful for REQUEST/RESPONSE)
     5       1     status    (offload_status; meaningful for RESPONSE/ERROR)
     6       2     seq       matches responses to requests
     8       4     length    payload byte length, <= WIRE_MAX_PAYLOAD
     12      N     payload   N = length bytes
     12+N    2     crc16     CRC16-CCITT over bytes [version .. end of payload]
   Total frame size = WIRE_HEADER_SIZE + length + WIRE_CRC_SIZE.

   CRC16-CCITT (poly 0x1021, initial value 0xFFFF, a.k.a. CRC-16/CCITT-
   FALSE) rather than CRC32: payloads at this stage are small (single
   words, short strings), this variant catches all 1/2-bit errors and any
   burst error <=16 bits, and it is the same class of checksum used by
   XMODEM/Kermit/Modbus over serial links -- directly relevant to the
   eventual RS232 target. */

#define WIRE_MAGIC0 0x57u /* 'W' */
#define WIRE_MAGIC1 0x50u /* 'P' */
#define WIRE_VERSION 1u

#define WIRE_HEADER_SIZE 12u /* magic(2) + version..length(10) */
#define WIRE_CRC_SIZE     2u

#define WIRE_MAX_PAYLOAD 65536uL

typedef enum {
    WIRE_MSG_HELLO    = 0,
    WIRE_MSG_CAPS     = 1,
    WIRE_MSG_REQUEST  = 2,
    WIRE_MSG_RESPONSE = 3,
    WIRE_MSG_ERROR    = 4
} wire_msg_type;

typedef struct {
    u8  type;    /* wire_msg_type */
    u8  op;      /* offload_op */
    u8  status;  /* offload_status */
    u16 seq;
    u32 length;  /* payload byte length */
} wire_header;

/* CRC16-CCITT-FALSE over [data, data+len), starting from the algorithm's
   initial value (0xFFFF). wire_crc16_update continues a running CRC from
   a prior partial result -- used to CRC a frame's header and payload as
   they stream through, without ever buffering the whole frame. */
u16 wire_crc16(const u8 *data, u32 len);
u16 wire_crc16_update(u16 crc, const u8 *data, u32 len);

/* Encodes header+payload into out (caller-owned). Returns the total frame
   size written, or 0 on failure (header->length > WIRE_MAX_PAYLOAD, or
   out_cap too small) -- out is left untouched on failure. */
u32 wire_encode_frame(const wire_header *header, const u8 *payload,
                       u8 *out, u32 out_cap);

/* Matches plat_transport_read's signature (see platform.h) so callers can
   pass a thin adapter around it; ctx carries whatever the adapter needs
   (typically a plat_transport*). */
typedef wp_status (*wire_read_fn)(void *ctx, u8 *out, u32 out_cap,
                                   u32 timeout_ms, u32 *out_len);

/* Reads from the stream via read_fn until one complete, valid frame is
   found or timeout_ms elapses (the budget applies per read_fn call, not
   to the whole scan -- a slow but steady peer will not spuriously time
   out). On WP_OK, *out_header is filled in and up to payload_cap bytes of
   the frame's payload are copied into payload_out; *payload_len is
   always set to the frame's actual payload length, so payload_cap == 0
   can be used to just measure. If the actual length exceeds payload_cap,
   returns WP_RANGE (the frame was still fully consumed from the stream,
   just not delivered -- this is not a framing error, so no resync is
   triggered).

   A frame that fails validation (wrong version, length above
   WIRE_MAX_PAYLOAD, or a CRC mismatch) is discarded and scanning resumes
   from the next unread stream byte -- this module never seeks backward
   or re-examines bytes it already consumed (plat_transport_read is a
   forward-only, unbuffered read with no pushback), so a garbage LENGTH
   field can in principle cause part of a subsequent real frame to be
   skipped along with it; resync recovers as soon as the next MAGIC0,
   MAGIC1 pair is found in the stream. Two independent budgets bound how
   long this can go on for a truly garbage stream: total bytes skipped
   while scanning for magic, and total frames rejected after a full
   header was read. Exceeding either returns WP_ERR (the caller should
   reconnect, not keep retrying the same stream). A read_fn failure
   (WP_TIMEOUT or WP_ERR) propagates immediately. */
wp_status wire_read_frame(wire_read_fn read_fn, void *ctx, u32 timeout_ms,
                           wire_header *out_header, u8 *payload_out,
                           u32 payload_cap, u32 *payload_len);

#endif /* WP_WIRE_H */
