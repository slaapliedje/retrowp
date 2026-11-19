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

/* The biggest request sent as one frame: the gateway's one-frame
   reader takes a whole WIRE_MAX_PAYLOAD, but this side keeps the same
   margin its DOS heap needs for the frame buffer. Past it, parts. */
#define OFFLOAD_MAX_REQUEST_SINGLE    65000u

/* The most one response may come to, all its parts together. A reply
   past it is a gateway gone wrong (or a stream that is not ours), not a
   document: refused with WP_RANGE rather than gathered until memory, or
   the export's disk, runs out. */
#define OFFLOAD_MAX_RESULT            (4uL * 1024uL * 1024uL)

/* A request's deadline is its timeout_ms, plus this much for every byte
   that moves: 2 ms a byte is a 4800-baud line (480 bytes a second), so
   an honest slow link carrying a big document finishes, while a peer
   that has stalled -- or trickles bytes slower than any real line, which
   a per-read timeout would wait out forever -- does not. */
#define OFFLOAD_MS_PER_BYTE           2uL

/* How many frames that are not this request's reply may be dropped
   before the stream is given up on. A late reply or two is normal after
   a timeout; a stream of them is a peer that is not answering us, and
   since each moves bytes (and so lengthens the deadline) it must be
   bounded by count, not only by time. */
#define OFFLOAD_MAX_DROPPED           16u

/* One request's clock. Every read and write takes what is left of it,
   so the whole exchange, not each read, is bounded. Elapsed time is
   now - start in u32 arithmetic, which stays right across the tick
   counter's wrap. */
typedef struct {
    plat_transport *t;
    u32 start;
    u32 budget;     /* timeout_ms + OFFLOAD_MS_PER_BYTE * bytes moved */
} offload_clock;

static void clock_start(offload_clock *c, plat_transport *t, u32 timeout_ms)
{
    c->t = t;
    c->start = plat_ticks_ms();
    c->budget = timeout_ms;
}

static void clock_moved(offload_clock *c, u32 bytes)
{
    u32 more = (bytes > 0xFFFFFFFFuL / OFFLOAD_MS_PER_BYTE) ? 0xFFFFFFFFuL : bytes * OFFLOAD_MS_PER_BYTE;
    c->budget = (c->budget > 0xFFFFFFFFuL - more) ? 0xFFFFFFFFuL : c->budget + more;
}

/* What is left, or 0 once the deadline has passed. */
static u32 clock_left(const offload_clock *c)
{
    u32 elapsed = plat_ticks_ms() - c->start;
    return (elapsed >= c->budget) ? 0u : c->budget - elapsed;
}

static wp_status clock_read_adapter(void *ctx, u8 *out, u32 out_cap, u32 timeout_ms, u32 *out_len)
{
    offload_clock *c = (offload_clock *)ctx;
    u32 left = clock_left(c);
    wp_status st;

    if (left == 0) { *out_len = 0; return WP_TIMEOUT; }
    if (timeout_ms > left) timeout_ms = left;
    st = plat_transport_read(c->t, out, out_cap, timeout_ms, out_len);
    if (st == WP_OK) clock_moved(c, *out_len);
    return st;
}

static plat_transport       *g_transport = NULL;
static wp_bool                g_available = WP_FALSE;
static offload_capabilities   g_caps;
static u16                    g_next_seq = 1;

/* HELLO, then the CAPS, within timeout_ms: WP_OK with g_caps filled. */
static wp_status handshake(u32 timeout_ms)
{
    wp_status st;
    wire_header req_hdr, resp_hdr;
    u8 hello_buf[WIRE_HEADER_SIZE + WIRE_CRC_SIZE];
    u8 caps_payload[CAPS_PAYLOAD_SIZE];
    u32 caps_len = 0;
    u32 n, dropped;
    offload_clock clock;

    req_hdr.type = WIRE_MSG_HELLO;
    req_hdr.op = 0;
    req_hdr.status = 0;
    req_hdr.seq = 0;
    req_hdr.length = 0;

    n = wire_encode_frame(&req_hdr, NULL, hello_buf, sizeof(hello_buf));
    st = plat_transport_write(g_transport, hello_buf, n, timeout_ms);
    if (st != WP_OK) return st;

    /* A reply still in flight from before -- the program was run again
       while the gateway answered its last request -- may come ahead of
       the CAPS: skipped, inside the one handshake deadline. */
    clock_start(&clock, g_transport, timeout_ms);
    dropped = 0;
    for (;;) {
        st = wire_read_frame(clock_read_adapter, &clock, timeout_ms,
                              &resp_hdr, caps_payload, sizeof(caps_payload), &caps_len);
        if ((st != WP_OK && st != WP_RANGE) || resp_hdr.type == WIRE_MSG_CAPS) break;
        if (++dropped > OFFLOAD_MAX_DROPPED) { st = WP_ERR; break; }
    }
    if (st != WP_OK) return st;
    if (caps_len != CAPS_PAYLOAD_SIZE) return WP_ERR;
    caps_decode(caps_payload, &g_caps);
    return WP_OK;
}

/* OFFLOAD_SET_SPEED (docs/formats/offload-protocol.md): a link opened at
   a safe speed, moved to the fastest both ends can do. This side offers
   every rate its line can be switched to that is faster than now,
   fastest first; the gateway answers with the one it picked -- still at
   the old speed -- and both switch. A HELLO at the new speed must bring
   the CAPS back within OFFLOAD_SPEED_CONFIRM_MS. If it does not (a cable
   that will not carry it, a machine that cannot keep up), this side goes
   back, waits out the gateway's own wait (it gives up at
   SPEED_CONFIRM_S, longer than this side's, and goes back too), and
   shakes hands at the old speed again. Every failure here leaves the
   link as it was: the speed is an improvement, never a requirement. */
#define OFFLOAD_SPEED_MAX_RATES   8u
#define OFFLOAD_SPEED_CONFIRM_MS  1500u
#define OFFLOAD_SPEED_SETTLE_MS   100u     /* the gateway drains, then switches */
#define OFFLOAD_SPEED_GATEWAY_MS  3000u    /* wire_protocol.py's SPEED_CONFIRM_S */

static void negotiate_speed(void)
{
    u32 rates[OFFLOAD_SPEED_MAX_RATES], now = 0, count, i, n = 0;
    u8 payload[OFFLOAD_SPEED_MAX_RATES * 4u];
    offload_request req;
    offload_response resp;
    u32 pick, t0;

    if ((g_caps.ops_supported & (1uL << (u32)OFFLOAD_SET_SPEED)) == 0) return;
    count = plat_transport_speeds(g_transport, rates, OFFLOAD_SPEED_MAX_RATES, &now);
    for (i = 0; i < count; i++) {
        if (rates[i] <= now) continue;
        payload[n * 4u]      = (u8)(rates[i] >> 24);
        payload[n * 4u + 1u] = (u8)(rates[i] >> 16);
        payload[n * 4u + 2u] = (u8)(rates[i] >> 8);
        payload[n * 4u + 3u] = (u8)rates[i];
        n++;
    }
    if (n == 0) return;

    req.op = OFFLOAD_SET_SPEED;
    req.payload = payload;
    req.payload_len = n * 4u;
    req.timeout_ms = OFFLOAD_HANDSHAKE_TIMEOUT_MS;
    if (offload_submit(&req, &resp) != WP_OK) return;
    pick = 0;
    if (resp.status == OFFLOAD_OK && resp.result_len >= 4) {
        pick = ((u32)resp.result[0] << 24) | ((u32)resp.result[1] << 16) |
               ((u32)resp.result[2] << 8) | (u32)resp.result[3];
    }
    offload_response_free(&resp);
    if (pick <= now) return;

    t0 = plat_ticks_ms();
    if (plat_transport_set_speed(g_transport, pick) != WP_OK) {
        /* The gateway has switched and this side cannot follow: let it
           give up and go back, then say HELLO where it will be. */
        while (!PLAT_TICKS_PASSED(t0, OFFLOAD_SPEED_GATEWAY_MS + 200u)) plat_sleep_ms(50);
        (void)handshake(OFFLOAD_HANDSHAKE_TIMEOUT_MS);
        return;
    }
    plat_sleep_ms(OFFLOAD_SPEED_SETTLE_MS);
    if (handshake(OFFLOAD_SPEED_CONFIRM_MS) == WP_OK) return;   /* the new speed holds */

    (void)plat_transport_set_speed(g_transport, now);
    while (!PLAT_TICKS_PASSED(t0, OFFLOAD_SPEED_GATEWAY_MS + 200u)) plat_sleep_ms(50);
    if (handshake(OFFLOAD_HANDSHAKE_TIMEOUT_MS) != WP_OK) g_available = WP_FALSE;
}

/* Whether this line has speeds to move between -- a serial port, not a
   socket. */
static wp_bool line_can_change_speed(void)
{
    u32 rates[1], now = 0;

    return (wp_bool)(plat_transport_speeds(g_transport, rates, 1u, &now) > 0u || now != 0u);
}

wp_status offload_init(const char *endpoint)
{
    wp_status st;

    g_available = WP_FALSE;

    st = plat_transport_open(endpoint, OFFLOAD_CONNECT_TIMEOUT_MS, &g_transport);
    if (st != WP_OK) { g_transport = NULL; return st; }

    st = handshake(OFFLOAD_HANDSHAKE_TIMEOUT_MS);
    if (st != WP_OK && line_can_change_speed()) {
        /* A gateway an earlier session moved up is still listening at
           that speed; this HELLO, sent at the line's own, reached it as
           noise, and noise is what sends it back down
           (docs/formats/offload-protocol.md, OFFLOAD_SET_SPEED). Asked
           once more, it hears. A line that cannot change speed was never
           moved, so it is not asked twice. */
        plat_sleep_ms(OFFLOAD_SPEED_SETTLE_MS);
        st = handshake(OFFLOAD_HANDSHAKE_TIMEOUT_MS);
    }
    if (st != WP_OK) {
        plat_transport_close(g_transport);
        g_transport = NULL;
        return st;
    }
    g_next_seq = 1;
    g_available = WP_TRUE;

    negotiate_speed();
    if (!g_available) {
        plat_transport_close(g_transport);
        g_transport = NULL;
        return WP_ERR;
    }
    return WP_OK;
}

/* The link's speed now, in baud; 0 when it has none (a socket) or there
   is no link. */
u32 offload_link_speed(void)
{
    u32 rates[1], now = 0;

    if (!g_available) return 0;
    (void)plat_transport_speeds(g_transport, rates, 1u, &now);
    return now;
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

/* One frame of the request: `type`, `len` bytes of payload. */
static wp_status send_frame(const offload_request *req, offload_clock *c, u8 type, u16 seq,
                            const u8 *data, u32 len)
{
    wire_header hdr;
    u8 *buf;
    u32 size, n, left;
    wp_status st;

    hdr.type = type;
    hdr.op = (u8)req->op;
    hdr.status = 0;
    hdr.seq = seq;
    hdr.length = len;
    size = WIRE_HEADER_SIZE + len + WIRE_CRC_SIZE;
    buf = (u8 *)mem_alloc(size);
    if (buf == NULL) return WP_NOMEM;
    n = wire_encode_frame(&hdr, data, buf, size);
    if (n == 0) { mem_free(buf); return WP_ERR; }
    clock_moved(c, n);
    left = clock_left(c);
    st = (left == 0) ? WP_TIMEOUT : plat_transport_write(g_transport, buf, n, left);
    mem_free(buf);
    return st;
}

/* The request: one frame, or -- past what one holds, to a gateway that
   reassembles them -- REQUEST_PART frames and a REQUEST with the rest.
   Each part is framed on its own, so no more than one part's buffer is
   ever held. WP_RANGE for a request too big for a gateway that cannot. */
static wp_status send_request(const offload_request *req, offload_clock *c, u16 seq)
{
    const u8 *p = req->payload;
    u32 left = req->payload_len;

    if (left > OFFLOAD_MAX_REQUEST_SINGLE) {
        if (g_caps.protocol_version < WIRE_CAPS_REQUEST_PARTS) return WP_RANGE;
        while (left > WIRE_REQUEST_PART_MAX) {
            wp_status st = send_frame(req, c, (u8)WIRE_MSG_REQUEST_PART, seq, p, WIRE_REQUEST_PART_MAX);
            if (st != WP_OK) return st;
            p += WIRE_REQUEST_PART_MAX;
            left -= WIRE_REQUEST_PART_MAX;
        }
    }
    return send_frame(req, c, (u8)WIRE_MSG_REQUEST, seq, p, left);
}

wp_status offload_submit_stream(const offload_request *req, offload_chunk_fn chunk, void *user,
                                offload_response *resp)
{
    wp_status st;
    wire_header resp_hdr;
    offload_clock clock;
    u8 *scratch;
    u32 scratch_len, dropped = 0;
    u16 seq;

    resp->status = OFFLOAD_ERR;
    resp->result = NULL;
    resp->result_len = 0;

    if (!g_available) return WP_ERR;

    clock_start(&clock, g_transport, req->timeout_ms);
    seq = g_next_seq++;
    if (g_next_seq == 0) g_next_seq = 1;    /* 0 is the handshake's */
    st = send_request(req, &clock, seq);
    if (st != WP_OK) return st;

    scratch = (u8 *)mem_alloc(OFFLOAD_MAX_RESPONSE_PAYLOAD);
    if (scratch == NULL) return WP_NOMEM;

    /* Parts until the final RESPONSE (or an ERROR); each frame's bytes
       go on before the next is read, so only one frame is ever held. */
    for (;;) {
        st = wire_read_frame(clock_read_adapter, &clock, req->timeout_ms,
                              &resp_hdr, scratch, OFFLOAD_MAX_RESPONSE_PAYLOAD, &scratch_len);
        if (st != WP_OK) break;
        if (resp_hdr.type != WIRE_MSG_RESPONSE && resp_hdr.type != WIRE_MSG_ERROR &&
            resp_hdr.type != WIRE_MSG_RESPONSE_PART) { st = WP_ERR; break; }
        /* Another request's reply -- one this side stopped waiting for
           when its deadline passed, arriving late -- is not this one's:
           dropped, and the wait goes on (within the same deadline). */
        if (resp_hdr.seq != seq || resp_hdr.op != (u8)req->op) {
            if (++dropped > OFFLOAD_MAX_DROPPED) { st = WP_ERR; break; }
            continue;
        }
        if (scratch_len > OFFLOAD_MAX_RESULT - resp->result_len) { st = WP_RANGE; break; }
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

wp_bool offload_takes_large_requests(void)
{
    return (wp_bool)(g_available && g_caps.protocol_version >= WIRE_CAPS_REQUEST_PARTS);
}

void offload_response_free(offload_response *resp)
{
    mem_free(resp->result);
    resp->result = NULL;
    resp->result_len = 0;
}
