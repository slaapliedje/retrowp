#ifndef WP_OFFLOAD_H
#define WP_OFFLOAD_H

#include "types.h"

/* Client interface to the offload link (CLAUDE.md "Offload Rules"): the
   engine is fully functional without a device attached, and every
   feature here is an optional enhancement requested through this one
   interface. The wire protocol (offload/transport/wire.h) is versioned,
   framed, and CRC-checked; timeouts here never let the UI hang. */

typedef enum {
    OFFLOAD_SPELL_CHECK,     /* word/run -> suggestions */
    OFFLOAD_GRAMMAR_CHECK,   /* paragraph -> issues */
    OFFLOAD_THESAURUS,       /* word -> synonyms */
    OFFLOAD_CONVERT_IMPORT,  /* ODT/other bytes -> internal model */
    OFFLOAD_CONVERT_EXPORT,  /* internal model -> ODT/PDF bytes */
    OFFLOAD_CHAT,            /* freeform assist (optional) */
    OFFLOAD_IMAGE,           /* source image bytes -> a small palette-
                                 indexed bitmap matched to the requesting
                                 target's own color ceiling (M15); see
                                 docs/formats/offload-protocol.md's
                                 OFFLOAD_IMAGE section for the wire shape */
    OFFLOAD_DICTATE          /* listen on the GATEWAY's own microphone ->
                                 UTF-8 text (T174). The audio never crosses
                                 the link: the request is 4 fixed bytes and
                                 the response is just the transcript, which
                                 is why this stays comfortable even at 9600
                                 baud. See docs/formats/offload-protocol.md's
                                 OFFLOAD_DICTATE section for the wire shape */
} offload_op;

typedef enum {
    OFFLOAD_OK          = 0,
    OFFLOAD_TIMEOUT     = 1,
    OFFLOAD_UNSUPPORTED = 2,
    OFFLOAD_ERR         = 3
} offload_status;

/* Learned during the HELLO/CAPS handshake in offload_init. Byte layout on
   the wire is documented in docs/formats/offload-protocol.md and
   implemented by offload/transport/caps_codec.h. */
typedef struct {
    u16 protocol_version;
    u32 ops_supported;      /* bitmask, bit (1uL << offload_op) */
    u32 max_payload_bytes;
    u8  gateway_name[32];   /* UTF-8, NUL-padded */
} offload_capabilities;

typedef struct {
    offload_op  op;
    const u8   *payload;
    u32         payload_len;
    u32         timeout_ms;
} offload_request;

typedef struct {
    offload_status status;
    u8             *result;     /* mem_alloc'd; free via offload_response_free */
    u32             result_len;
} offload_response;

/* Opens the offload link at `endpoint` (a plat_transport endpoint
   string, e.g. "tcp:host:port" or, on a future backend, a serial
   endpoint) and performs the HELLO/CAPS handshake. Safe to call with no
   device present -- on any failure this returns the underlying error but
   leaves the engine fully usable; offload_available() simply reports
   false afterward and every other function degrades gracefully. */
wp_status offload_init(const char *endpoint);
void      offload_shutdown(void);

/* O(1), never blocks: true only after a successful offload_init AND
   handshake. */
wp_bool offload_available(void);

/* Returns the capabilities learned during the handshake. WP_ERR if
   offload_available() is false. */
wp_status offload_query(offload_capabilities *out);

/* Submits one request and waits up to req->timeout_ms for a response.
   Returns WP_ERR immediately (not WP_TIMEOUT) if offload_available() is
   false -- callers should check availability once rather than relying on
   every submit to fail fast. On WP_OK, resp->result is heap-allocated
   and owned by the caller until passed to offload_response_free. */
wp_status offload_submit(const offload_request *req, offload_response *resp);
void      offload_response_free(offload_response *resp);

#endif /* WP_OFFLOAD_H */
