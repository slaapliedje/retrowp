/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "offload.h"
#include "platform.h"
#include "wire.h"
#include "caps_codec.h"

#define OFFLOAD_CONNECT_TIMEOUT_MS    2000u
#define OFFLOAD_HANDSHAKE_TIMEOUT_MS  2000u

/* Bounds the scratch buffer offload_submit() allocates to receive a
   response -- NOT a wire-protocol limit (that's WIRE_MAX_PAYLOAD, 64KB,
   in offload/transport/wire.h). Originally 4096: comfortably more than
   THESAURUS/SPELL_CHECK's small word-list responses ever needed (M6).
   M7's CONVERT_IMPORT/CONVERT_EXPORT (docs/formats/offload-protocol.md)
   changed that: a real gateway-produced ODT or PDF for even a modest
   document routinely exceeds 4096 bytes (an early, larger fixture in
   host/tests/test_convert.c hit this exact ceiling during development --
   offload_submit returned WP_RANGE, not a gateway bug -- before the
   fixture was resized down and this constant was raised instead).
   It was 32768; PDF export of a real multi-page document outgrew it
   (2026-11-02), so it is now as large as one wire frame allows, less a
   margin: 65000, not WIRE_MAX_PAYLOAD's 65536, because a 16-bit DOS
   heap cannot hand out a full 64 KB in one block. The scratch buffer is
   freed as soon as the response is copied into resp->result (sized to
   the ACTUAL length), so this is a brief 64 KB on a 1 MB machine, not a
   resident one. It bounds ONE FRAME, not a response: a bigger response
   comes as WIRE_MSG_RESPONSE_PART frames (wire.h), each read into this
   same scratch and handed on before the next. */
#define OFFLOAD_MAX_RESPONSE_PAYLOAD  65000u

static plat_transport       *g_transport = NULL;
static wp_bool                g_available = WP_FALSE;
static offload_capabilities   g_caps;
static u16                    g_next_seq = 1;

static wp_status transport_read_adapter(void *ctx, u8 *out, u32 out_cap,
                                         u32 timeout_ms, u32 *out_len)
{
    return plat_transport_read((plat_transport *)ctx, out, out_cap, timeout_ms, out_len);
}

wp_status offload_init(const char *endpoint)
{
    wp_status st;
    wire_header req_hdr, resp_hdr;
    u8 hello_buf[WIRE_HEADER_SIZE + WIRE_CRC_SIZE];
    u8 caps_payload[CAPS_PAYLOAD_SIZE];
    u32 caps_len;
    u32 n;

    g_available = WP_FALSE;

    st = plat_transport_open(endpoint, OFFLOAD_CONNECT_TIMEOUT_MS, &g_transport);
    if (st != WP_OK) { g_transport = NULL; return st; }

    req_hdr.type = WIRE_MSG_HELLO;
    req_hdr.op = 0;
    req_hdr.status = 0;
    req_hdr.seq = 0;
    req_hdr.length = 0;

    n = wire_encode_frame(&req_hdr, NULL, hello_buf, sizeof(hello_buf));
    st = plat_transport_write(g_transport, hello_buf, n, OFFLOAD_HANDSHAKE_TIMEOUT_MS);
    if (st != WP_OK) {
        plat_transport_close(g_transport);
        g_transport = NULL;
        return st;
    }

    st = wire_read_frame(transport_read_adapter, g_transport, OFFLOAD_HANDSHAKE_TIMEOUT_MS,
                          &resp_hdr, caps_payload, sizeof(caps_payload), &caps_len);
    if (st != WP_OK || resp_hdr.type != WIRE_MSG_CAPS || caps_len != CAPS_PAYLOAD_SIZE) {
        plat_transport_close(g_transport);
        g_transport = NULL;
        return (st == WP_OK) ? WP_ERR : st;
    }

    caps_decode(caps_payload, &g_caps);
    g_next_seq = 1;
    g_available = WP_TRUE;
    return WP_OK;
}

void offload_shutdown(void)
{
    if (g_transport) {
        plat_transport_close(g_transport);
        g_transport = NULL;
    }
    g_available = WP_FALSE;
}

wp_bool offload_available(void)
{
    return g_available;
}

wp_status offload_query(offload_capabilities *out)
{
    if (!g_available) return WP_ERR;
    *out = g_caps;
    return WP_OK;
}

/* Sends the request frame. */
static wp_status send_request(const offload_request *req, u16 seq)
{
    wire_header req_hdr;
    u8 *req_buf;
    u32 req_buf_size, n;
    wp_status st;

    req_hdr.type = WIRE_MSG_REQUEST;
    req_hdr.op = (u8)req->op;
    req_hdr.status = 0;
    req_hdr.seq = seq;
    req_hdr.length = req->payload_len;

    req_buf_size = WIRE_HEADER_SIZE + req->payload_len + WIRE_CRC_SIZE;
    req_buf = (u8 *)mem_alloc(req_buf_size);
    if (req_buf == NULL) return WP_NOMEM;
    n = wire_encode_frame(&req_hdr, req->payload, req_buf, req_buf_size);
    if (n == 0) { mem_free(req_buf); return WP_ERR; }
    st = plat_transport_write(g_transport, req_buf, n, req->timeout_ms);
    mem_free(req_buf);
    return st;
}

wp_status offload_submit_stream(const offload_request *req, offload_chunk_fn chunk, void *user,
                                offload_response *resp)
{
    wp_status st;
    wire_header resp_hdr;
    u8 *scratch;
    u32 scratch_len;

    resp->status = OFFLOAD_ERR;
    resp->result = NULL;
    resp->result_len = 0;

    if (!g_available) return WP_ERR;

    st = send_request(req, g_next_seq++);
    if (st != WP_OK) return st;

    scratch = (u8 *)mem_alloc(OFFLOAD_MAX_RESPONSE_PAYLOAD);
    if (scratch == NULL) return WP_NOMEM;

    /* Parts until the final RESPONSE (or an ERROR); each frame's bytes
       go on before the next is read, so only one frame is ever held. */
    for (;;) {
        st = wire_read_frame(transport_read_adapter, g_transport, req->timeout_ms,
                              &resp_hdr, scratch, OFFLOAD_MAX_RESPONSE_PAYLOAD, &scratch_len);
        if (st != WP_OK) break;
        if (resp_hdr.type != WIRE_MSG_RESPONSE && resp_hdr.type != WIRE_MSG_ERROR &&
            resp_hdr.type != WIRE_MSG_RESPONSE_PART) { st = WP_ERR; break; }
        if (scratch_len > 0 && chunk != NULL) {
            st = chunk(user, scratch, scratch_len);
            if (st != WP_OK) break;
        }
        resp->result_len += scratch_len;
        if (resp_hdr.type != WIRE_MSG_RESPONSE_PART) {
            resp->status = (offload_status)resp_hdr.status;
            break;
        }
    }
    mem_free(scratch);
    return st;
}

/* offload_submit's sink: the parts gathered into one block, grown as
   they come. */
typedef struct {
    u8 *bytes;
    u32 len, cap;
} gather_buf;

static wp_status gather(void *user, const u8 *data, u32 len)
{
    gather_buf *g = (gather_buf *)user;

    if (g->len + len > g->cap) {
        u32 cap = (g->cap == 0) ? len : g->cap;
        u8 *grown;
        while (cap < g->len + len) cap *= 2;
        grown = (u8 *)mem_realloc(g->bytes, g->cap, cap);
        if (grown == NULL) return WP_NOMEM;
        g->bytes = grown;
        g->cap = cap;
    }
    memcpy(g->bytes + g->len, data, len);
    g->len += len;
    return WP_OK;
}

wp_status offload_submit(const offload_request *req, offload_response *resp)
{
    gather_buf g;
    wp_status st;

    g.bytes = NULL;
    g.len = g.cap = 0;
    st = offload_submit_stream(req, gather, &g, resp);
    if (st != WP_OK) {
        mem_free(g.bytes);
        resp->result = NULL;
        resp->result_len = 0;
        return st;
    }
    resp->result = g.bytes;
    resp->result_len = g.len;
    return WP_OK;
}

void offload_response_free(offload_response *resp)
{
    mem_free(resp->result);
    resp->result = NULL;
    resp->result_len = 0;
}
