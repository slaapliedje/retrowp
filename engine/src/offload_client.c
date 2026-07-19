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
   32768 is a deliberate, still-bounded choice, not "as large as
   possible": comfortably covers a real 1MB-tier document's ODT/PDF
   export, is freed immediately after copying into resp->result (sized
   to the ACTUAL response length, not this constant), and stays a small
   fraction of the 1MB Atari ST budget even as a temporary allocation. */
#define OFFLOAD_MAX_RESPONSE_PAYLOAD  32768u

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

wp_status offload_submit(const offload_request *req, offload_response *resp)
{
    wp_status st;
    wire_header req_hdr, resp_hdr;
    u8 *req_buf;
    u32 req_buf_size, n;
    u8 *scratch;
    u32 scratch_len;

    resp->status = OFFLOAD_ERR;
    resp->result = NULL;
    resp->result_len = 0;

    if (!g_available) return WP_ERR;

    req_hdr.type = WIRE_MSG_REQUEST;
    req_hdr.op = (u8)req->op;
    req_hdr.status = 0;
    req_hdr.seq = g_next_seq++;
    req_hdr.length = req->payload_len;

    req_buf_size = WIRE_HEADER_SIZE + req->payload_len + WIRE_CRC_SIZE;
    req_buf = (u8 *)mem_alloc(req_buf_size);
    if (req_buf == NULL) return WP_NOMEM;

    n = wire_encode_frame(&req_hdr, req->payload, req_buf, req_buf_size);
    if (n == 0) { mem_free(req_buf); return WP_ERR; }

    st = plat_transport_write(g_transport, req_buf, n, req->timeout_ms);
    mem_free(req_buf);
    if (st != WP_OK) return st;

    scratch = (u8 *)mem_alloc(OFFLOAD_MAX_RESPONSE_PAYLOAD);
    if (scratch == NULL) return WP_NOMEM;

    st = wire_read_frame(transport_read_adapter, g_transport, req->timeout_ms,
                          &resp_hdr, scratch, OFFLOAD_MAX_RESPONSE_PAYLOAD, &scratch_len);
    if (st != WP_OK) { mem_free(scratch); return st; }

    if (resp_hdr.type != WIRE_MSG_RESPONSE && resp_hdr.type != WIRE_MSG_ERROR) {
        mem_free(scratch);
        return WP_ERR;
    }

    resp->status = (offload_status)resp_hdr.status;
    resp->result_len = scratch_len;
    if (scratch_len > 0) {
        resp->result = (u8 *)mem_alloc(scratch_len);
        if (resp->result == NULL) { mem_free(scratch); return WP_NOMEM; }
        memcpy(resp->result, scratch, scratch_len);
    } else {
        resp->result = NULL;
    }
    mem_free(scratch);

    return WP_OK;
}

void offload_response_free(offload_response *resp)
{
    mem_free(resp->result);
    resp->result = NULL;
    resp->result_len = 0;
}
