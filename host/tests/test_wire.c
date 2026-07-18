#include <string.h>
#include "test.h"
#include "wire.h"

/* An in-memory "stream" driving wire_read_frame with no real I/O.
   max_chunk caps bytes returned per call below what was requested, so
   these tests exercise wire_read_frame's partial-read accumulation loops
   the same way a real, slow transport would. */
typedef struct {
    const u8 *data;
    u32 len;
    u32 pos;
    u32 max_chunk;
} mem_stream;

static wp_status mem_stream_read(void *ctx, u8 *out, u32 out_cap,
                                  u32 timeout_ms, u32 *out_len)
{
    mem_stream *s = (mem_stream *)ctx;
    u32 avail, n;

    WP_UNUSED(timeout_ms);

    avail = s->len - s->pos;
    if (avail == 0) return WP_TIMEOUT;

    n = out_cap;
    if (n > avail) n = avail;
    if (n > s->max_chunk) n = s->max_chunk;

    memcpy(out, s->data + s->pos, n);
    s->pos += n;
    if (out_len) *out_len = n;
    return WP_OK;
}

void test_wire_crc16_known_vector(void)
{
    /* Standard CRC-16/CCITT-FALSE check value for ASCII "123456789". */
    ASSERT_EQ_U32(wire_crc16((const u8 *)"123456789", 9), 0x29B1u);
}

void test_wire_encode_decode_roundtrip(void)
{
    u8 buf[256];
    wire_header hdr, out_hdr;
    u32 n;
    u8 payload_out[64];
    u32 payload_len;
    mem_stream stream;

    hdr.type = WIRE_MSG_REQUEST;
    hdr.op = 3;
    hdr.status = 0;
    hdr.seq = 42;
    hdr.length = 5;

    n = wire_encode_frame(&hdr, (const u8 *)"hello", buf, sizeof(buf));
    ASSERT_EQ_U32(n, WIRE_HEADER_SIZE + 5 + WIRE_CRC_SIZE);

    stream.data = buf;
    stream.len = n;
    stream.pos = 0;
    stream.max_chunk = 3; /* force short reads through every accumulation loop */

    ASSERT_TRUE(wire_read_frame(mem_stream_read, &stream, 1000, &out_hdr,
                                 payload_out, sizeof(payload_out), &payload_len) == WP_OK);
    ASSERT_EQ_U32(out_hdr.type, WIRE_MSG_REQUEST);
    ASSERT_EQ_U32(out_hdr.op, 3);
    ASSERT_EQ_U32(out_hdr.seq, 42);
    ASSERT_EQ_U32(payload_len, 5);
    ASSERT_MEM_EQ(payload_out, "hello", 5);
}

void test_wire_rejects_corrupt_frame_and_resyncs(void)
{
    u8 stream_buf[512];
    u32 pos = 0;
    u8 frame_a[128], frame_b[128], frame_c[128];
    u32 len_a, len_b, len_c;
    wire_header hdr, out_hdr;
    mem_stream stream;
    u8 payload_out[64];
    u32 payload_len;

    /* garbage prefix, no 'W','P' pair in it */
    memcpy(stream_buf + pos, "junk!!", 6);
    pos += 6;

    hdr.type = WIRE_MSG_RESPONSE; hdr.op = 2; hdr.status = 0; hdr.seq = 1; hdr.length = 6;
    len_a = wire_encode_frame(&hdr, (const u8 *)"wordA!", frame_a, sizeof(frame_a));
    memcpy(stream_buf + pos, frame_a, len_a);
    pos += len_a;

    hdr.seq = 2;
    len_b = wire_encode_frame(&hdr, (const u8 *)"wordB!", frame_b, sizeof(frame_b));
    frame_b[len_b - 1] ^= 0xFFu; /* flip a byte of the CRC field */
    memcpy(stream_buf + pos, frame_b, len_b);
    pos += len_b;

    hdr.seq = 3;
    len_c = wire_encode_frame(&hdr, (const u8 *)"wordC!", frame_c, sizeof(frame_c));
    memcpy(stream_buf + pos, frame_c, len_c);
    pos += len_c;

    stream.data = stream_buf;
    stream.len = pos;
    stream.pos = 0;
    stream.max_chunk = 7;

    /* First call: skips the garbage prefix, returns frame A. */
    ASSERT_TRUE(wire_read_frame(mem_stream_read, &stream, 1000, &out_hdr,
                                 payload_out, sizeof(payload_out), &payload_len) == WP_OK);
    ASSERT_EQ_U32(out_hdr.seq, 1);
    ASSERT_MEM_EQ(payload_out, "wordA!", 6);

    /* Second call: rejects corrupted frame B internally, resyncs, and
       returns frame C -- rejection and resync proven in one deterministic
       test (T6.1 AC). */
    ASSERT_TRUE(wire_read_frame(mem_stream_read, &stream, 1000, &out_hdr,
                                 payload_out, sizeof(payload_out), &payload_len) == WP_OK);
    ASSERT_EQ_U32(out_hdr.seq, 3);
    ASSERT_MEM_EQ(payload_out, "wordC!", 6);
}

void test_wire_timeout_on_empty_stream(void)
{
    mem_stream stream;
    wire_header out_hdr;
    u8 payload_out[16];
    u32 payload_len;

    stream.data = (const u8 *)"";
    stream.len = 0;
    stream.pos = 0;
    stream.max_chunk = 4;

    ASSERT_TRUE(wire_read_frame(mem_stream_read, &stream, 100, &out_hdr,
                                 payload_out, sizeof(payload_out), &payload_len) == WP_TIMEOUT);
}

void test_wire_payload_too_large_for_buffer(void)
{
    u8 buf[128];
    wire_header hdr, out_hdr;
    u32 n;
    u8 small_out[2];
    u32 payload_len;
    mem_stream stream;

    hdr.type = WIRE_MSG_RESPONSE; hdr.op = 1; hdr.status = 0; hdr.seq = 9; hdr.length = 5;
    n = wire_encode_frame(&hdr, (const u8 *)"hello", buf, sizeof(buf));

    stream.data = buf;
    stream.len = n;
    stream.pos = 0;
    stream.max_chunk = 64;

    ASSERT_TRUE(wire_read_frame(mem_stream_read, &stream, 1000, &out_hdr,
                                 small_out, sizeof(small_out), &payload_len) == WP_RANGE);
    ASSERT_EQ_U32(payload_len, 5); /* actual length is still reported */
}
