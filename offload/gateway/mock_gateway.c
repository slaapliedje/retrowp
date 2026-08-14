#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "types.h"
#include "endian.h"
#include "wire.h"
#include "caps_codec.h"
#include "offload.h"
#include "fixtures.h"
#include "gw_common.h"

/* Host-side mock gateway (T6.4, GRAMMAR_CHECK added T8.1, CHAT added
   T8.3): a standalone POSIX executable, never linked into the engine or
   app. It answers OFFLOAD_THESAURUS, OFFLOAD_SPELL_CHECK,
   OFFLOAD_GRAMMAR_CHECK, and OFFLOAD_CHAT from the fixture tables with
   real wire framing, so tests can prove the protocol round-trips
   deterministically without a Pi or an LLM.

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
        int rx_fd = gw_open_fifo_rdwr(serial_rx);
        int tx_fd = gw_open_fifo_rdwr(serial_tx);

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
        caps.ops_supported = (1uL << OFFLOAD_SPELL_CHECK) | (1uL << OFFLOAD_THESAURUS) |
                             (1uL << OFFLOAD_GRAMMAR_CHECK) | (1uL << OFFLOAD_CHAT) |
                             (1uL << OFFLOAD_IMAGE) | (1uL << OFFLOAD_DICTATE);
        caps.max_payload_bytes = 4096;
        strncpy((char *)caps.gateway_name, "mock-gateway", sizeof(caps.gateway_name) - 1);

        caps_encode(&caps, caps_payload);
        gw_send_frame(ctx.write_fd, WIRE_MSG_CAPS, 0, 0, 0, caps_payload, CAPS_PAYLOAD_SIZE, WP_FALSE);
    }

    /* --- request/response loop --- */
    for (;;) {
        wire_header req_hdr;
        /* Sized for a whole paragraph (GRAMMAR_CHECK), not just a word --
           THESAURUS/SPELL_CHECK requests still fit comfortably inside
           this same buffer, just using far less of it. */
        u8 word_buf[2048];
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
                resp_len = gw_encode_word_list(synonyms, count, resp_payload, sizeof(resp_payload));
                status = (u8)OFFLOAD_OK;
            } else if (req_hdr.op == OFFLOAD_SPELL_CHECK) {
                const char *const *suggestions = NULL;
                u32 count = 0;
                wp_bool is_misspelling = fixtures_spellcheck_lookup(word_buf, word_len,
                                                                     &suggestions, &count);
                wp_bool is_correct = (wp_bool)(is_misspelling ? WP_FALSE : WP_TRUE);

                resp_len = gw_encode_spellcheck_response(is_correct, suggestions, count,
                                                       resp_payload, sizeof(resp_payload));
                status = (u8)OFFLOAD_OK;
            } else if (req_hdr.op == OFFLOAD_GRAMMAR_CHECK) {
                enum { GRAMMAR_MAX_ISSUES = 16 };
                const fixtures_grammar_issue *issues = NULL;
                u32 count = fixtures_grammar_lookup(word_buf, word_len, &issues);
                u16 starts[GRAMMAR_MAX_ISSUES];
                u16 lens[GRAMMAR_MAX_ISSUES];
                const char *messages[GRAMMAR_MAX_ISSUES];
                const char *suggestions[GRAMMAR_MAX_ISSUES];
                u32 j;

                if (count > GRAMMAR_MAX_ISSUES) count = GRAMMAR_MAX_ISSUES;
                for (j = 0; j < count; j++) {
                    starts[j]      = issues[j].start_offset;
                    lens[j]        = issues[j].span_len;
                    messages[j]    = issues[j].message;
                    suggestions[j] = issues[j].suggestion;
                }

                resp_len = gw_encode_grammar_issues(starts, lens, messages, suggestions, count,
                                                     resp_payload, sizeof(resp_payload));
                status = (u8)OFFLOAD_OK;
            } else if (req_hdr.op == OFFLOAD_CHAT) {
                u32 reply_len;
                const char *reply = fixtures_chat_reply(word_buf, word_len, &reply_len);

                if (reply_len <= sizeof(resp_payload)) {
                    memcpy(resp_payload, reply, reply_len);
                    resp_len = reply_len;
                    status = (u8)OFFLOAD_OK;
                }
            } else if (req_hdr.op == OFFLOAD_IMAGE) {
                const fixtures_image *img = fixtures_image_get();

                resp_len = gw_encode_image_response(img->width_px, img->height_px,
                                                      img->palette, img->palette_count,
                                                      img->packed_pixels, img->packed_len,
                                                      resp_payload, sizeof(resp_payload));
                status = (u8)OFFLOAD_OK;
            } else if (req_hdr.op == OFFLOAD_DICTATE) {
                /* A DICTATE request is exactly 4 bytes (max_duration_s,
                   flags, language[2]) -- rejecting anything shorter here
                   rather than answering anyway is what gives the host
                   tests a way to prove the client encodes the request
                   correctly, since the canned transcript itself is
                   returned unconditionally and would otherwise hide a
                   malformed request entirely. */
                if (word_len < 4) {
                    status = (u8)OFFLOAD_ERR;
                } else {
                    u32 transcript_len;
                    const char *transcript = fixtures_dictate_transcript(&transcript_len);

                    if (transcript_len <= sizeof(resp_payload)) {
                        memcpy(resp_payload, transcript, transcript_len);
                        resp_len = transcript_len;
                        status = (u8)OFFLOAD_OK;
                    }
                }
            }

            gw_send_frame(ctx.write_fd, WIRE_MSG_RESPONSE, req_hdr.op, status, req_hdr.seq,
                       resp_payload, resp_len, corrupt);
        }
    }

    close(ctx.read_fd);
    if (ctx.write_fd != ctx.read_fd) close(ctx.write_fd);
    return 0;
}
