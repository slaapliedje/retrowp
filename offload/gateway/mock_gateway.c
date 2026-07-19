#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "types.h"
#include "endian.h"
#include "wire.h"
#include "caps_codec.h"
#include "offload.h"
#include "fixtures.h"

/* Host-side mock gateway (T6.4): a standalone POSIX executable, never
   linked into the engine or app. It answers OFFLOAD_THESAURUS and
   OFFLOAD_SPELL_CHECK from the fixture tables with real wire framing,
   so tests can prove the protocol round-trips deterministically without
   a Pi or an LLM.

   Two transports, same protocol logic:
   - Default (TCP): binds port 0 (OS-assigned) and prints the assigned
     port as a single line on stdout before accepting, so
     host/tests/gateway_harness.c never has to guess or poll for
     readiness.
   - --serial-rx <path> --serial-tx <path> (T6.3): reads frames from
     <rx> and writes them to <tx> instead -- the same two named FIFOs
     Hatari's --rs232-out/--rs232-in point at, letting this exact,
     already-tested handshake/fixture logic answer a REAL Atari-side
     offload_client() over emulated RS-232, not just a TCP client. Both
     FIFOs are opened O_RDWR (not O_RDONLY/O_WRONLY) specifically to
     avoid the classic named-FIFO open-blocks-until-peer-connects
     deadlock -- verified against this project's own
     platform/host/transport_host.c pipe: implementation, which relies
     on the same trick. Prints "serial\n" instead of a port number, so
     a harness can wait for the FIFOs to be open before starting Hatari
     the same way it already waits for the TCP port line.

   Fault injection (see docs/formats/offload-protocol.md for how each
   maps to a client-observable failure), same flags regardless of
   transport:
     --corrupt-after=N   flips a byte in the CRC of every response after
                          the Nth (0 = corrupt starting with the first).
     --die-after=N       exits abruptly (no response) after the Nth
                          request, simulating a crash mid-request.
     --delay-ms=N         sleeps N ms before every response.
     --short-frame        sends a response header promising a large
                          payload, writes a few bytes, then goes back to
                          waiting for the next request -- the promised
                          payload never arrives.
     --hello-timeout-ms=N how long to wait for the client's first HELLO
                          before giving up and exiting (default 5000,
                          fine for the TCP tests where the client
                          connects near-instantly). host/tools/
                          ci_atari_serial.sh overrides this to something
                          much larger: a real Atari boot + program load
                          under Hatari's cycle-exact CPU emulation can
                          take well over 5 real seconds before the ST
                          program ever reaches its first Bconout call,
                          and the ORIGINAL bug this flag fixes was ugly
                          in a specific way worth documenting -- on
                          timeout this process closes both FIFOs and
                          exits (correct behavior), but Hatari's *own*
                          later write() to the now-reader-less
                          --rs232-out FIFO then raises SIGPIPE, killing
                          the whole emulator (not just failing the
                          test): a real, once-reproduced failure mode
                          this comment exists to prevent re-discovering
                          the hard way. */

static int parse_int_flag(const char *arg, const char *name, int *out)
{
    size_t nlen = strlen(name);

    if (strncmp(arg, name, nlen) == 0 && arg[nlen] == '=') {
        *out = atoi(arg + nlen + 1);
        return 1;
    }
    return 0;
}

typedef struct { int read_fd; int write_fd; } gw_ctx;

static wp_status gw_read(void *ctx, u8 *out, u32 out_cap, u32 timeout_ms, u32 *out_len)
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

static int gw_write_all(int fd, const u8 *data, u32 len)
{
    u32 written = 0;

    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n <= 0) return -1;
        written += (u32)n;
    }
    return 0;
}

static void send_frame(int fd, wire_msg_type type, u8 op, u8 status, u16 seq,
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

/* count length-prefixed UTF-8 strings: u16 count, then count x
   (u16 len, bytes). Returns bytes written, or 0 if it wouldn't fit. */
static u32 encode_word_list(const char *const *words, u32 count, u8 *out, u32 out_cap)
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

static u32 encode_spellcheck_response(wp_bool is_correct, const char *const *suggestions,
                                       u32 count, u8 *out, u32 out_cap)
{
    u32 list_len;

    if (out_cap < 1) return 0;
    out[0] = (u8)is_correct;

    list_len = encode_word_list(suggestions, count, out + 1, out_cap - 1);
    if (list_len == 0) return 0;

    return 1 + list_len;
}

/* Opens `path` as a named FIFO (creating it if it doesn't exist yet)
   with O_RDWR -- not O_RDONLY/O_WRONLY -- specifically so this open
   never blocks waiting for a peer, regardless of whether Hatari has
   opened its end yet. See the file-level comment for why this matters. */
static int open_fifo_rdwr(const char *path)
{
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) return -1;
    return open(path, O_RDWR);
}

int main(int argc, char **argv)
{
    int corrupt_after = -1;
    int die_after = -1;
    int delay_ms = 0;
    int hello_timeout_ms = 5000;
    wp_bool short_frame = WP_FALSE;
    const char *serial_rx = NULL;
    const char *serial_tx = NULL;
    int i;

    int listen_fd = -1;
    int conn_fd = -1;
    struct sockaddr_in addr;
    socklen_t addrlen;
    gw_ctx ctx;
    int respond_count = 0;

    for (i = 1; i < argc; i++) {
        if (parse_int_flag(argv[i], "--corrupt-after", &corrupt_after)) continue;
        if (parse_int_flag(argv[i], "--die-after", &die_after)) continue;
        if (parse_int_flag(argv[i], "--delay-ms", &delay_ms)) continue;
        if (parse_int_flag(argv[i], "--hello-timeout-ms", &hello_timeout_ms)) continue;
        if (strcmp(argv[i], "--short-frame") == 0) { short_frame = WP_TRUE; continue; }
        if (strncmp(argv[i], "--serial-rx=", 12) == 0) { serial_rx = argv[i] + 12; continue; }
        if (strncmp(argv[i], "--serial-tx=", 12) == 0) { serial_tx = argv[i] + 12; continue; }
    }

    if (serial_rx != NULL && serial_tx != NULL) {
        int rx_fd = open_fifo_rdwr(serial_rx);
        int tx_fd = open_fifo_rdwr(serial_tx);

        if (rx_fd < 0 || tx_fd < 0) return 1;

        ctx.read_fd = rx_fd;
        ctx.write_fd = tx_fd;

        printf("serial\n");
        fflush(stdout);
    } else {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) return 1;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return 1;
        if (listen(listen_fd, 1) != 0) return 1;

        addrlen = sizeof(addr);
        if (getsockname(listen_fd, (struct sockaddr *)&addr, &addrlen) != 0) return 1;

        printf("%d\n", (int)ntohs(addr.sin_port));
        fflush(stdout);

        conn_fd = accept(listen_fd, NULL, NULL);
        close(listen_fd);
        if (conn_fd < 0) return 1;

        ctx.read_fd = conn_fd;
        ctx.write_fd = conn_fd;
    }

    /* --- handshake: expect HELLO, respond CAPS --- */
    {
        wire_header hdr;
        u8 dummy[1];
        u32 dummy_len;
        offload_capabilities caps;
        u8 caps_payload[CAPS_PAYLOAD_SIZE];

        if (wire_read_frame(gw_read, &ctx, (u32)hello_timeout_ms, &hdr, dummy, 0, &dummy_len) != WP_OK
            || hdr.type != WIRE_MSG_HELLO) {
            close(ctx.read_fd);
            if (ctx.write_fd != ctx.read_fd) close(ctx.write_fd);
            return 1;
        }

        memset(&caps, 0, sizeof(caps));
        caps.protocol_version = WIRE_VERSION;
        caps.ops_supported = (1uL << OFFLOAD_SPELL_CHECK) | (1uL << OFFLOAD_THESAURUS);
        caps.max_payload_bytes = 4096;
        strncpy((char *)caps.gateway_name, "mock-gateway", sizeof(caps.gateway_name) - 1);

        caps_encode(&caps, caps_payload);
        send_frame(ctx.write_fd, WIRE_MSG_CAPS, 0, 0, 0, caps_payload, CAPS_PAYLOAD_SIZE, WP_FALSE);
    }

    /* --- request/response loop --- */
    for (;;) {
        wire_header req_hdr;
        u8 word_buf[256];
        u32 word_len;
        wp_status st;

        st = wire_read_frame(gw_read, &ctx, 30000, &req_hdr, word_buf, sizeof(word_buf), &word_len);
        if (st != WP_OK) break; /* client disconnected, or nothing for 30s -- exit cleanly */

        if (delay_ms > 0) {
            struct timespec ts;
            ts.tv_sec = delay_ms / 1000;
            ts.tv_nsec = (long)(delay_ms % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }

        respond_count++;
        if (die_after >= 0 && respond_count > die_after) {
            _exit(1); /* simulate a crash: no response, no clean close */
        }

        if (short_frame) {
            u8 hdr_buf[WIRE_HEADER_SIZE];

            hdr_buf[0] = (u8)WIRE_MAGIC0;
            hdr_buf[1] = (u8)WIRE_MAGIC1;
            hdr_buf[2] = (u8)WIRE_VERSION;
            hdr_buf[3] = (u8)WIRE_MSG_RESPONSE;
            hdr_buf[4] = req_hdr.op;
            hdr_buf[5] = (u8)OFFLOAD_OK;
            be_write16(hdr_buf + 6, req_hdr.seq);
            be_write32(hdr_buf + 8, 60000uL); /* promise far more than we send */

            gw_write_all(ctx.write_fd, hdr_buf, WIRE_HEADER_SIZE);
            gw_write_all(ctx.write_fd, (const u8 *)"only a few bytes", 17);
            continue; /* the rest never arrives; back to waiting for the next request */
        }

        {
            u8 resp_payload[512];
            u32 resp_len = 0;
            u8 status = (u8)OFFLOAD_UNSUPPORTED;
            wp_bool corrupt = (wp_bool)(corrupt_after >= 0 && respond_count > corrupt_after);

            if (req_hdr.op == OFFLOAD_THESAURUS) {
                const char *const *synonyms = NULL;
                u32 count = fixtures_thesaurus_lookup(word_buf, word_len, &synonyms);
                resp_len = encode_word_list(synonyms, count, resp_payload, sizeof(resp_payload));
                status = (u8)OFFLOAD_OK;
            } else if (req_hdr.op == OFFLOAD_SPELL_CHECK) {
                const char *const *suggestions = NULL;
                u32 count = 0;
                wp_bool is_misspelling = fixtures_spellcheck_lookup(word_buf, word_len,
                                                                     &suggestions, &count);
                wp_bool is_correct = (wp_bool)(is_misspelling ? WP_FALSE : WP_TRUE);

                resp_len = encode_spellcheck_response(is_correct, suggestions, count,
                                                       resp_payload, sizeof(resp_payload));
                status = (u8)OFFLOAD_OK;
            }

            send_frame(ctx.write_fd, WIRE_MSG_RESPONSE, req_hdr.op, status, req_hdr.seq,
                       resp_payload, resp_len, corrupt);
        }
    }

    close(ctx.read_fd);
    if (ctx.write_fd != ctx.read_fd) close(ctx.write_fd);
    return 0;
}
