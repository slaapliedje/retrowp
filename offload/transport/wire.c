/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "wire.h"
#include "endian.h"

#define WIRE_MAX_RESYNC_SKIP     4096uL
#define WIRE_MAX_REJECTED_FRAMES 8uL
#define WIRE_SCRATCH_CHUNK       64uL

u16 wire_crc16_update(u16 crc, const u8 *data, u32 len)
{
    u32 i;
    int bit;

    for (i = 0; i < len; i++) {
        crc = (u16)(crc ^ (u16)((u16)data[i] << 8));
        for (bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = (u16)((crc << 1) ^ 0x1021u);
            } else {
                crc = (u16)(crc << 1);
            }
        }
    }
    return crc;
}

u16 wire_crc16(const u8 *data, u32 len)
{
    return wire_crc16_update(0xFFFFu, data, len);
}

u32 wire_encode_frame(const wire_header *header, const u8 *payload,
                       u8 *out, u32 out_cap)
{
    u32 total;
    u16 crc;

    if (header->length > WIRE_MAX_PAYLOAD) return 0;
    total = WIRE_HEADER_SIZE + header->length + WIRE_CRC_SIZE;
    if (total > out_cap) return 0;

    out[0] = (u8)WIRE_MAGIC0;
    out[1] = (u8)WIRE_MAGIC1;
    out[2] = (u8)WIRE_VERSION;
    out[3] = header->type;
    out[4] = header->op;
    out[5] = header->status;
    be_write16(out + 6, header->seq);
    be_write32(out + 8, header->length);
    if (header->length > 0) memcpy(out + WIRE_HEADER_SIZE, payload, header->length);

    crc = wire_crc16(out + 2, 10u + header->length);
    be_write16(out + WIRE_HEADER_SIZE + header->length, crc);

    return total;
}

wp_status wire_read_frame(wire_read_fn read_fn, void *ctx, u32 timeout_ms,
                           wire_header *out_header, u8 *payload_out,
                           u32 payload_cap, u32 *payload_len)
{
    u32 magic_skipped = 0;
    u32 rejected_frames = 0;

    for (;;) {
        wp_status st;
        u8 m0, m1;
        u8 hdr[10]; /* version, type, op, status, seq(2), length(4) */
        u32 n, got;
        u32 length;
        u16 crc_running;

        /* --- scan for MAGIC0, MAGIC1 as a 2-byte sliding window --- */
        st = read_fn(ctx, &m1, 1, timeout_ms, &got);
        if (st != WP_OK) return st;
        for (;;) {
            m0 = m1;
            st = read_fn(ctx, &m1, 1, timeout_ms, &got);
            if (st != WP_OK) return st;
            if (m0 == (u8)WIRE_MAGIC0 && m1 == (u8)WIRE_MAGIC1) break;
            magic_skipped++;
            if (magic_skipped > WIRE_MAX_RESYNC_SKIP) return WP_ERR;
        }

        /* --- header: version..length (10 bytes) --- */
        n = 0;
        while (n < 10) {
            st = read_fn(ctx, hdr + n, 10 - n, timeout_ms, &got);
            if (st != WP_OK) return st;
            n += got;
        }

        length = be_read32(hdr + 6);
        if (hdr[0] != (u8)WIRE_VERSION || length > WIRE_MAX_PAYLOAD) {
            rejected_frames++;
            if (rejected_frames > WIRE_MAX_REJECTED_FRAMES) return WP_ERR;
            continue; /* resume scanning for magic from here */
        }

        crc_running = wire_crc16(hdr, 10);

        /* --- payload, streamed through a small fixed scratch buffer so
           this never needs a buffer sized to WIRE_MAX_PAYLOAD --- */
        {
            u8 scratch[WIRE_SCRATCH_CHUNK];
            u32 remaining = length;
            u32 delivered = 0;

            while (remaining > 0) {
                u32 chunk = (remaining < WIRE_SCRATCH_CHUNK) ? remaining : WIRE_SCRATCH_CHUNK;

                st = read_fn(ctx, scratch, chunk, timeout_ms, &got);
                if (st != WP_OK) return st;

                crc_running = wire_crc16_update(crc_running, scratch, got);

                if (delivered < payload_cap) {
                    u32 space = payload_cap - delivered;
                    u32 copy = (got < space) ? got : space;
                    memcpy(payload_out + delivered, scratch, copy);
                }
                delivered += got;
                remaining -= got;
            }
        }

        /* --- CRC field --- */
        {
            u8 crc_bytes[2];
            u16 crc_expected;

            n = 0;
            while (n < 2) {
                st = read_fn(ctx, crc_bytes + n, 2 - n, timeout_ms, &got);
                if (st != WP_OK) return st;
                n += got;
            }
            crc_expected = be_read16(crc_bytes);

            if (crc_expected != crc_running) {
                rejected_frames++;
                if (rejected_frames > WIRE_MAX_REJECTED_FRAMES) return WP_ERR;
                continue; /* resume scanning for magic from here */
            }
        }

        out_header->type = hdr[1];
        out_header->op = hdr[2];
        out_header->status = hdr[3];
        out_header->seq = be_read16(hdr + 4);
        out_header->length = length;

        if (payload_len) *payload_len = length;
        if (length > payload_cap) return WP_RANGE;
        return WP_OK;
    }
}
