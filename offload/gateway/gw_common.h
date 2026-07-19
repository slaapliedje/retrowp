#ifndef WP_GATEWAY_GW_COMMON_H
#define WP_GATEWAY_GW_COMMON_H

#include "types.h"
#include "wire.h"

/* Shared plumbing between offload/gateway's standalone executables
   (mock_gateway.c, ollama_gateway.c). Both speak the exact same framed
   wire protocol over either a real TCP connection or a pair of named
   FIFOs (T6.3's Hatari serial bridge) -- the transport-agnostic pieces
   (reading/writing raw bytes, framing a response, opening a FIFO the
   deadlock-free way, encoding a word-list payload) live here once
   rather than risking drift between two copies. Host-side tooling only
   (CLAUDE.md: offload/gateway is "modern C/Python"), never linked into
   the engine or platform/atari. */

typedef struct { int read_fd; int write_fd; } gw_ctx;

/* wire_read_frame's read callback signature (offload/transport/wire.h). */
wp_status gw_read(void *ctx, u8 *out, u32 out_cap, u32 timeout_ms, u32 *out_len);

int gw_write_all(int fd, const u8 *data, u32 len);

/* Encodes and sends one frame. If corrupt is WP_TRUE, flips the frame's
   last byte (inside the CRC) after encoding -- mock_gateway's fault
   injection; ollama_gateway always passes WP_FALSE. */
void gw_send_frame(int fd, wire_msg_type type, u8 op, u8 status, u16 seq,
                    const u8 *payload, u32 payload_len, wp_bool corrupt);

/* Opens `path` as a named FIFO (creating it if it doesn't exist) with
   O_RDWR -- not O_RDONLY/O_WRONLY -- so the open never blocks waiting
   for a peer to open the other end first (see docs/atari-platform.md's
   T6.3 section for the deadlock this avoids). */
int gw_open_fifo_rdwr(const char *path);

/* Encodes count length-prefixed UTF-8 strings: u16 count, then count x
   (u16 len, bytes). Returns bytes written, or 0 if it wouldn't fit --
   the OFFLOAD_THESAURUS response shape (docs/formats/offload-protocol.md). */
u32 gw_encode_word_list(const char *const *words, u32 count, u8 *out, u32 out_cap);

/* is_correct byte, then the same word-list shape -- OFFLOAD_SPELL_CHECK's
   response shape. */
u32 gw_encode_spellcheck_response(wp_bool is_correct, const char *const *suggestions,
                                   u32 count, u8 *out, u32 out_cap);

#endif /* WP_GATEWAY_GW_COMMON_H */
