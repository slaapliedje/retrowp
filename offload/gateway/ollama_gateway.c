#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "types.h"
#include "wire.h"
#include "caps_codec.h"
#include "offload.h"
#include "gw_common.h"
#include "http_client.h"

/* Real-backend gateway (post-M6): answers OFFLOAD_THESAURUS with an
   actual Ollama (http://localhost:11434 by default) call instead of
   mock_gateway.c's fixture table -- "not just a faked endpoint" per the
   request that prompted this file. Same wire protocol, same TCP/serial
   transport choice, same HELLO/CAPS handshake shape as mock_gateway.c
   (sharing gw_common.c so neither drifts); only the THESAURUS request
   handler's backend differs.

   Scope: THESAURUS ONLY. Every other op, including SPELL_CHECK (which
   mock_gateway.c does serve, from fixtures), responds OFFLOAD_UNSUPPORTED
   here -- the same honest "absent gracefully" path the protocol already
   defines for ops no gateway implements (docs/formats/offload-protocol.md).
   This was a deliberate choice made after testing, not an oversight:
   spelling correction needs to be RIGHT, and this project's on-device
   services/spell already covers that deterministically; a quick trial
   against the model this gateway defaults to (deepseek-coder-v2:16b, a
   code-tuned model -- the only non-"thinking" model available in this
   project's own test environment) reported "teh" as a correctly spelled
   word, which is a bad user experience swap for the deterministic local
   checker. THESAURUS has a much lower correctness bar (a real-vocabulary
   word list is valuable even if not exhaustive) and is exactly the kind
   of large-vocabulary knowledge CLAUDE.md's own project brief names as
   offload-worthy, so that's where this gateway spends its one real
   capability. A future gateway (or a --model override pointed at a
   better-suited model) implementing GRAMMAR_CHECK/SPELL_CHECK for real is
   M8 scope, not reshaped here.

   --model is a plain CLI override, so a differently-equipped machine (or
   a smaller/faster instruct model pulled later) isn't locked to the
   default. Whatever model is loaded costs real wall-clock time on a cold
   start -- ~20s to load an 8.9GB model into memory was the measured cost
   in this project's own dev environment even on an otherwise-idle GPU,
   BEFORE the model can answer the first request -- so --ollama-timeout-ms
   defaults generously (60s) rather than to something that suits an
   already-warm model. Ollama unloads an idle model after its own
   keep_alive window, so the first request after a gap repays that cost
   again; this is inherent to the "optional, offloaded" nature of the
   feature (CLAUDE.md's Offload Rules), not a bug in this file.

   No libcurl dependency: http_client.c hand-rolls the one HTTP/1.1 POST
   this needs directly over a socket. */

#define OLLAMA_DEFAULT_HOST "127.0.0.1"
#define OLLAMA_DEFAULT_PORT 11434
#define OLLAMA_DEFAULT_MODEL "deepseek-coder-v2:16b"
#define OLLAMA_DEFAULT_TIMEOUT_MS 60000u

#define THESAURUS_MAX_WORDS    8
#define THESAURUS_MAX_WORD_LEN 64
#define GATEWAY_WORD_MAX_LEN   128 /* truncation bound for the incoming request word */

static int parse_int_flag(const char *arg, const char *name, int *out)
{
    size_t nlen = strlen(name);

    if (strncmp(arg, name, nlen) == 0 && arg[nlen] == '=') {
        *out = atoi(arg + nlen + 1);
        return 1;
    }
    return 0;
}

static int parse_str_flag(const char *arg, const char *name, const char **out)
{
    size_t nlen = strlen(name);

    if (strncmp(arg, name, nlen) == 0 && arg[nlen] == '=') {
        *out = arg + nlen + 1;
        return 1;
    }
    return 0;
}

/* Escapes word_len bytes of word into out as the CONTENTS of a JSON
   string (no surrounding quotes). Control characters other than
   \n/\r/\t are emitted as \u00XX; everything else (including any raw
   UTF-8 continuation bytes) passes through unchanged -- JSON strings
   are UTF-8-safe as long as ", \, and control bytes are escaped, which
   is all this needs to guarantee for a single word embedded in a
   prompt. */
static u32 json_escape(const u8 *word, u32 word_len, char *out, u32 out_cap)
{
    static const char hex[] = "0123456789abcdef";
    u32 i, pos = 0;

    for (i = 0; i < word_len && pos + 6 < out_cap; i++) {
        u8 c = word[i];

        switch (c) {
        case '"':  out[pos++] = '\\'; out[pos++] = '"';  break;
        case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
        case '\n': out[pos++] = '\\'; out[pos++] = 'n';  break;
        case '\r': out[pos++] = '\\'; out[pos++] = 'r';  break;
        case '\t': out[pos++] = '\\'; out[pos++] = 't';  break;
        default:
            if (c < 0x20) {
                out[pos++] = '\\'; out[pos++] = 'u'; out[pos++] = '0'; out[pos++] = '0';
                out[pos++] = hex[(c >> 4) & 0xF];
                out[pos++] = hex[c & 0xF];
            } else {
                out[pos++] = (char)c;
            }
        }
    }
    out[pos] = '\0';
    return pos;
}

/* Builds the /api/generate request body. The prompt is deliberately
   rigid ("ONLY a comma-separated list... no explanation") because this
   gateway's parser (parse_word_list below) has no fallback for free-form
   prose -- tested against the default model, this phrasing reliably
   produces a bare list (see docs/formats/offload-protocol.md for the
   observed examples, including how an unknown word degrades to an empty
   answer rather than an error). Returns the body length, or 0 if it
   didn't fit (out_cap too small for this word + template). */
static u32 build_generate_request(const char *model, const u8 *word, u32 word_len,
                                   char *out, u32 out_cap)
{
    char escaped[GATEWAY_WORD_MAX_LEN * 6 + 1];
    int written;

    json_escape(word, word_len, escaped, (u32)sizeof(escaped));

    written = snprintf(out, out_cap,
        "{\"model\":\"%s\",\"prompt\":\"Word: %s\\nList up to %d English synonyms for "
        "this word. Reply with ONLY a comma-separated list of words, no numbering, no "
        "explanation. If you do not know this word or it has no synonyms, reply with an "
        "empty line.\",\"stream\":false}",
        model, escaped, THESAURUS_MAX_WORDS);

    if (written <= 0 || (size_t)written >= out_cap) return 0;
    return (u32)written;
}

/* Finds "field_name":"..." within json (json_len bytes, not necessarily
   NUL-terminated) and copies the unescaped string value into out
   (out_cap bytes, NUL-terminated). Handles \", \\, \n, \r, \t; \uXXXX
   escapes are skipped (dropped, not translated) -- English synonym
   lists are not expected to need them, and this gateway's ASCII-ish
   scope elsewhere (services/spell is ASCII-only too) makes that an
   acceptable simplification, not a silent correctness gap in the
   feature this exists for. Returns WP_FALSE if the field isn't found. */
static wp_bool extract_json_string_field(const u8 *json, u32 json_len, const char *field_name,
                                          char *out, u32 out_cap, u32 *out_len)
{
    char needle[32];
    u32 needle_len;
    u32 i;

    snprintf(needle, sizeof(needle), "\"%s\":\"", field_name);
    needle_len = (u32)strlen(needle);

    for (i = 0; i + needle_len <= json_len; i++) {
        u32 pos;
        u32 out_pos;

        if (memcmp(json + i, needle, needle_len) != 0) continue;

        pos = i + needle_len;
        out_pos = 0;
        while (pos < json_len && json[pos] != '"' && out_pos + 1 < out_cap) {
            if (json[pos] == '\\' && pos + 1 < json_len) {
                u8 esc = json[pos + 1];
                switch (esc) {
                case '"':  out[out_pos++] = '"';  pos += 2; break;
                case '\\': out[out_pos++] = '\\'; pos += 2; break;
                case 'n':  out[out_pos++] = '\n'; pos += 2; break;
                case 't':  out[out_pos++] = '\t'; pos += 2; break;
                case 'r':  out[out_pos++] = '\r'; pos += 2; break;
                case 'u':  pos += (json_len - pos >= 6) ? 6 : (json_len - pos); break;
                default:   out[out_pos++] = (char)esc; pos += 2; break;
                }
            } else {
                out[out_pos++] = (char)json[pos];
                pos++;
            }
        }
        out[out_pos] = '\0';
        if (out_len) *out_len = out_pos;
        return WP_TRUE;
    }
    return WP_FALSE;
}

/* Splits text on commas/newlines into up to max_words tokens, trims
   surrounding whitespace, and keeps only tokens that look like a single
   plausible word (letters, hyphen, apostrophe; 1..THESAURUS_MAX_WORD_LEN-1
   bytes) -- silently dropping anything else (an empty token from a
   trailing comma, a stray explanatory sentence the model added despite
   being told not to, and so on) rather than propagating garbage into
   the response payload. Returns the number of words written to out. */
static u32 parse_word_list(const char *text, u32 text_len,
                            char out[][THESAURUS_MAX_WORD_LEN], u32 max_words)
{
    u32 count = 0;
    u32 i = 0;

    while (i < text_len && count < max_words) {
        u32 start, end, len, k;
        wp_bool valid;

        while (i < text_len && (text[i] == ',' || text[i] == '\n' || text[i] == ' ' ||
                                 text[i] == '\t' || text[i] == '\r')) {
            i++;
        }
        start = i;
        while (i < text_len && text[i] != ',' && text[i] != '\n') i++;

        end = i;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                                text[end - 1] == '\r')) {
            end--;
        }
        len = end - start;

        if (len == 0 || len >= THESAURUS_MAX_WORD_LEN) continue;

        valid = WP_TRUE;
        for (k = 0; k < len; k++) {
            char c = text[start + k];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '\'')) {
                valid = WP_FALSE;
                break;
            }
        }
        if (!valid) continue;

        memcpy(out[count], text + start, len);
        out[count][len] = '\0';
        count++;
    }
    return count;
}

/* The one real capability this gateway has: word -> Ollama -> synonym
   list, wire-encoded exactly like mock_gateway's fixture-backed answer.
   Returns the offload_status byte for the response frame; *resp_len is
   left at 0 unless status is OFFLOAD_OK. count==0 (an empty word list)
   is still OFFLOAD_OK -- "no synonyms found" is a valid answer, same
   convention as the fixture-backed gateway. */
static u8 do_thesaurus(const char *ollama_host, u16 ollama_port, const char *model,
                        u32 ollama_timeout_ms, const u8 *word, u32 word_len,
                        u8 *resp_payload, u32 resp_cap, u32 *resp_len)
{
    char req_body[GATEWAY_WORD_MAX_LEN * 6 + 512];
    u32 req_len;
    u8 http_body[8192];
    u32 http_body_len = 0;
    char field[4096];
    u32 field_len = 0;
    char words[THESAURUS_MAX_WORDS][THESAURUS_MAX_WORD_LEN];
    const char *word_ptrs[THESAURUS_MAX_WORDS];
    u32 count, i;
    u32 clamped_len = (word_len > GATEWAY_WORD_MAX_LEN) ? GATEWAY_WORD_MAX_LEN : word_len;

    *resp_len = 0;

    req_len = build_generate_request(model, word, clamped_len, req_body, (u32)sizeof(req_body));
    if (req_len == 0) return (u8)OFFLOAD_ERR;

    if (http_post_json(ollama_host, ollama_port, "/api/generate",
                        (const u8 *)req_body, req_len, ollama_timeout_ms,
                        http_body, (u32)sizeof(http_body), &http_body_len) != WP_OK) {
        return (u8)OFFLOAD_ERR;
    }

    if (!extract_json_string_field(http_body, http_body_len, "response",
                                    field, (u32)sizeof(field), &field_len)) {
        return (u8)OFFLOAD_ERR;
    }

    count = parse_word_list(field, field_len, words, THESAURUS_MAX_WORDS);
    for (i = 0; i < count; i++) word_ptrs[i] = words[i];

    *resp_len = gw_encode_word_list(word_ptrs, count, resp_payload, resp_cap);
    return (u8)OFFLOAD_OK;
}

int main(int argc, char **argv)
{
    const char *ollama_host = OLLAMA_DEFAULT_HOST;
    int ollama_port = OLLAMA_DEFAULT_PORT;
    const char *model = OLLAMA_DEFAULT_MODEL;
    int ollama_timeout_ms = (int)OLLAMA_DEFAULT_TIMEOUT_MS;
    int hello_timeout_ms = 5000;
    const char *serial_rx = NULL;
    const char *serial_tx = NULL;
    int i;

    int listen_fd = -1;
    int conn_fd = -1;
    struct sockaddr_in addr;
    socklen_t addrlen;
    gw_ctx ctx;

    for (i = 1; i < argc; i++) {
        if (parse_str_flag(argv[i], "--ollama-host", &ollama_host)) continue;
        if (parse_int_flag(argv[i], "--ollama-port", &ollama_port)) continue;
        if (parse_str_flag(argv[i], "--model", &model)) continue;
        if (parse_int_flag(argv[i], "--ollama-timeout-ms", &ollama_timeout_ms)) continue;
        if (parse_int_flag(argv[i], "--hello-timeout-ms", &hello_timeout_ms)) continue;
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

    /* --- handshake: expect HELLO, respond CAPS (THESAURUS only) --- */
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
        caps.ops_supported = (1uL << OFFLOAD_THESAURUS);
        caps.max_payload_bytes = 4096;
        strncpy((char *)caps.gateway_name, "ollama-gateway", sizeof(caps.gateway_name) - 1);

        caps_encode(&caps, caps_payload);
        gw_send_frame(ctx.write_fd, WIRE_MSG_CAPS, 0, 0, 0, caps_payload, CAPS_PAYLOAD_SIZE, WP_FALSE);
    }

    /* --- request/response loop --- */
    for (;;) {
        wire_header req_hdr;
        u8 word_buf[256];
        u32 word_len;
        wp_status st;
        u8 resp_payload[2048];
        u32 resp_len = 0;
        u8 status;

        st = wire_read_frame(gw_read, &ctx, 120000, &req_hdr, word_buf, sizeof(word_buf), &word_len);
        if (st != WP_OK) break; /* client disconnected, or nothing for 120s -- exit cleanly */

        if (req_hdr.op == OFFLOAD_THESAURUS) {
            status = do_thesaurus(ollama_host, (u16)ollama_port, model, (u32)ollama_timeout_ms,
                                   word_buf, word_len, resp_payload, sizeof(resp_payload), &resp_len);
        } else {
            status = (u8)OFFLOAD_UNSUPPORTED;
            resp_len = 0;
        }

        gw_send_frame(ctx.write_fd, WIRE_MSG_RESPONSE, req_hdr.op, status, req_hdr.seq,
                      resp_payload, resp_len, WP_FALSE);
    }

    close(ctx.read_fd);
    if (ctx.write_fd != ctx.read_fd) close(ctx.write_fd);
    return 0;
}
