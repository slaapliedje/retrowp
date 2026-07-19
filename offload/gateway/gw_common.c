#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "types.h"
#include "endian.h"
#include "gw_common.h"

wp_status gw_read(void *ctx, u8 *out, u32 out_cap, u32 timeout_ms, u32 *out_len)
{
    gw_ctx *c = (gw_ctx *)ctx;
    struct pollfd pfd;
    int pr;
    ssize_t n;

    pfd.fd = c->read_fd;
    pfd.events = POLLIN;

    pr = poll(&pfd, 1, (int)timeout_ms);
    if (pr == 0) return WP_TIMEOUT;
    if (pr < 0) return WP_ERR;
    if (pfd.revents & (POLLHUP | POLLERR)) return WP_ERR;

    n = read(c->read_fd, out, (size_t)out_cap);
    if (n <= 0) return WP_ERR;

    if (out_len) *out_len = (u32)n;
    return WP_OK;
}

int gw_write_all(int fd, const u8 *data, u32 len)
{
    u32 written = 0;

    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n <= 0) return -1;
        written += (u32)n;
    }
    return 0;
}

void gw_send_frame(int fd, wire_msg_type type, u8 op, u8 status, u16 seq,
                    const u8 *payload, u32 payload_len, wp_bool corrupt)
{
    u32 buf_size = WIRE_HEADER_SIZE + payload_len + WIRE_CRC_SIZE;
    u8 *buf = (u8 *)malloc(buf_size);
    wire_header hdr;
    u32 n;

    if (buf == NULL) return;

    hdr.type = (u8)type;
    hdr.op = op;
    hdr.status = status;
    hdr.seq = seq;
    hdr.length = payload_len;

    n = wire_encode_frame(&hdr, payload, buf, buf_size);
    if (n > 0 && corrupt) buf[n - 1] ^= 0xFFu;
    if (n > 0) gw_write_all(fd, buf, n);

    free(buf);
}

int gw_open_fifo_rdwr(const char *path)
{
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) return -1;
    return open(path, O_RDWR);
}

u32 gw_encode_word_list(const char *const *words, u32 count, u8 *out, u32 out_cap)
{
    u32 pos, i;

    if (out_cap < 2) return 0;
    be_write16(out, (u16)count);
    pos = 2;

    for (i = 0; i < count; i++) {
        u32 wlen = (u32)strlen(words[i]);
        if (pos + 2 + wlen > out_cap) return 0;
        be_write16(out + pos, (u16)wlen);
        pos += 2;
        memcpy(out + pos, words[i], wlen);
        pos += wlen;
    }
    return pos;
}

u32 gw_encode_spellcheck_response(wp_bool is_correct, const char *const *suggestions,
                                   u32 count, u8 *out, u32 out_cap)
{
    u32 list_len;

    if (out_cap < 1) return 0;
    out[0] = (u8)is_correct;

    list_len = gw_encode_word_list(suggestions, count, out + 1, out_cap - 1);
    if (list_len == 0) return 0;

    return 1 + list_len;
}

u32 gw_encode_grammar_issues(const u16 *start_offsets, const u16 *span_lens,
                              const char *const *messages, const char *const *suggestions,
                              u32 count, u8 *out, u32 out_cap)
{
    u32 pos, i;

    if (out_cap < 2) return 0;
    be_write16(out, (u16)count);
    pos = 2;

    for (i = 0; i < count; i++) {
        u32 mlen = (u32)strlen(messages[i]);
        u32 slen = (suggestions[i] != NULL) ? (u32)strlen(suggestions[i]) : 0;

        if (pos + 2 + 2 + 2 + mlen + 2 + slen > out_cap) return 0;

        be_write16(out + pos, start_offsets[i]);
        pos += 2;
        be_write16(out + pos, span_lens[i]);
        pos += 2;

        be_write16(out + pos, (u16)mlen);
        pos += 2;
        memcpy(out + pos, messages[i], mlen);
        pos += mlen;

        be_write16(out + pos, (u16)slen);
        pos += 2;
        if (slen > 0) {
            memcpy(out + pos, suggestions[i], slen);
            pos += slen;
        }
    }
    return pos;
}
