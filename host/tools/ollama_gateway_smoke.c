#include <stdio.h>
#include <string.h>
#include "types.h"
#include "endian.h"
#include "offload.h"

/* Manual (NOT part of `make test`/ci.sh) end-to-end proof that the real
   offload_client.c API -- the exact same code path platform/atari's
   future Thesaurus/Grammar Check menu actions or a Pi deployment would
   use, not a hand-rolled protocol client -- gets genuine LLM-generated
   answers back from a live offload/gateway/ollama_gateway.c talking to a
   real Ollama server. Runs all three real ops this gateway implements
   (OFFLOAD_THESAURUS, OFFLOAD_GRAMMAR_CHECK added M8 T8.1, OFFLOAD_CHAT
   added M8 T8.3) over the same connection. Deliberately excluded from
   the automated suite: LLM
   output is not deterministic (see docs/formats/offload-protocol.md),
   and a contributor without Ollama installed must still be able to run
   `make test`/`ci.sh` successfully -- this tool exists so a human can
   verify the real thing works, not so CI can assert exact output.

   Usage: ollama_gateway_smoke <endpoint> [word] [paragraph]
   Run via host/tools/run_ollama_smoke.sh, which spawns bin/ollama_gateway
   itself and passes its endpoint here -- see that script for the
   port-capture mechanics (same trick as host/tests/gateway_harness.c). */

static int g_pass = 0;
static int g_fail = 0;

static void check(wp_bool cond, const char *what)
{
    if (cond) {
        g_pass++;
        printf("PASS: %s\n", what);
    } else {
        g_fail++;
        printf("FAIL: %s\n", what);
    }
}

static void print_word_list(const u8 *buf, u32 len)
{
    u16 count;
    u32 pos, i;

    if (len < 2) {
        printf("  (payload too short to contain a count)\n");
        return;
    }
    count = be_read16(buf);
    pos = 2;
    printf("  %u word(s):", (unsigned)count);
    for (i = 0; i < count && pos + 2 <= len; i++) {
        u16 wlen = be_read16(buf + pos);
        pos += 2;
        if (pos + wlen > len) break;
        printf(" %.*s", (int)wlen, buf + pos);
        pos += wlen;
    }
    printf("\n");
}

/* Decodes and prints the OFFLOAD_GRAMMAR_CHECK response shape (u16
   issue_count, then issue_count x (u16 start_offset, u16 span_len, u16
   message_len + bytes, u16 suggestion_len + bytes) --
   docs/formats/offload-protocol.md). Also re-slices `paragraph` at each
   reported offset/length and prints THAT alongside the model's own
   message/suggestion -- a human-visible confirmation that the offset
   the gateway computed via its own substring search genuinely lands on
   the flagged text, not just a self-consistent number. */
static void print_grammar_issues(const u8 *buf, u32 len, const char *paragraph, u32 paragraph_len)
{
    u16 count;
    u32 pos, i;

    if (len < 2) {
        printf("  (payload too short to contain a count)\n");
        return;
    }
    count = be_read16(buf);
    pos = 2;
    printf("  %u issue(s):\n", (unsigned)count);
    for (i = 0; i < count; i++) {
        u16 start_offset, span_len, mlen, slen;

        if (pos + 6 > len) { printf("  (truncated issue record)\n"); return; }
        start_offset = be_read16(buf + pos); pos += 2;
        span_len     = be_read16(buf + pos); pos += 2;
        mlen         = be_read16(buf + pos); pos += 2;
        if (pos + mlen + 2 > len) { printf("  (truncated message)\n"); return; }

        printf("    [%u..%u) \"", (unsigned)start_offset, (unsigned)(start_offset + span_len));
        if ((u32)start_offset + span_len <= paragraph_len) {
            printf("%.*s", (int)span_len, paragraph + start_offset);
        } else {
            printf("(offset out of range!)");
        }
        printf("\" -- %.*s", (int)mlen, buf + pos);
        pos += mlen;

        slen = be_read16(buf + pos); pos += 2;
        if (pos + slen > len) { printf(" (truncated suggestion)\n"); return; }
        if (slen > 0) printf(" -> \"%.*s\"", (int)slen, buf + pos);
        pos += slen;
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    const char *endpoint;
    const char *word = "happy";
    const char *paragraph = "The team are excited to announce there new product, "
                             "which dont require any special setup.";
    const char *chat_prompt = "In one short sentence, what is a good synonym for happy?";
    offload_capabilities caps;
    offload_request req;
    offload_response resp;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <endpoint> [word] [paragraph] [chat_prompt]\n", argv[0]);
        return 1;
    }
    endpoint = argv[1];
    if (argc >= 3) word = argv[2];
    if (argc >= 4) paragraph = argv[3];
    if (argc >= 5) chat_prompt = argv[4];

    printf("OLLAMA GATEWAY SMOKE: starting (endpoint=%s word=%s)\n", endpoint, word);

    check(offload_available() == WP_FALSE, "offload_available() false before offload_init");

    /* Generous timeout: a cold model load measured ~20s in this
       project's own dev environment for an 8.9GB model on an otherwise-
       idle GPU (see ollama_gateway.c's file comment); this is a manual
       tool run by a human, not a CI budget. */
    check(offload_init(endpoint) == WP_OK, "offload_init handshake succeeded");
    check(offload_available(), "offload_available() true after handshake");

    if (offload_query(&caps) == WP_OK) {
        char name[33];
        memcpy(name, caps.gateway_name, 32);
        name[32] = '\0';
        printf("  protocol_version=%u ops_supported=0x%lx max_payload_bytes=%lu gateway_name=%s\n",
               (unsigned)caps.protocol_version, (unsigned long)caps.ops_supported,
               (unsigned long)caps.max_payload_bytes, name);
        check(strcmp(name, "ollama-gateway") == 0, "negotiated gateway_name is \"ollama-gateway\"");
        check((caps.ops_supported & (1uL << OFFLOAD_THESAURUS)) != 0,
              "negotiated capabilities advertise OFFLOAD_THESAURUS");
        check((caps.ops_supported & (1uL << OFFLOAD_GRAMMAR_CHECK)) != 0,
              "negotiated capabilities advertise OFFLOAD_GRAMMAR_CHECK");
        check((caps.ops_supported & (1uL << OFFLOAD_CHAT)) != 0,
              "negotiated capabilities advertise OFFLOAD_CHAT");
        check((caps.ops_supported & (1uL << OFFLOAD_IMAGE)) == 0,
              "negotiated capabilities do NOT advertise OFFLOAD_IMAGE (never implemented anywhere)");
    } else {
        check(WP_FALSE, "offload_query succeeded");
    }

    req.op = OFFLOAD_THESAURUS;
    req.payload = (const u8 *)word;
    req.payload_len = (u32)strlen(word);
    req.timeout_ms = 90000; /* cold model load + generation, not a tight budget */

    if (offload_submit(&req, &resp) == WP_OK) {
        check(WP_TRUE, "offload_submit(OFFLOAD_THESAURUS) returned WP_OK");
        check(resp.status == OFFLOAD_OK, "response status is OFFLOAD_OK");
        printf("  synonyms for \"%s\":\n", word);
        print_word_list(resp.result, resp.result_len);
        offload_response_free(&resp);
    } else {
        check(WP_FALSE, "offload_submit(OFFLOAD_THESAURUS) returned WP_OK");
    }

    req.op = OFFLOAD_GRAMMAR_CHECK;
    req.payload = (const u8 *)paragraph;
    req.payload_len = (u32)strlen(paragraph);
    req.timeout_ms = 90000; /* cold model load (a second, different model) + generation */

    if (offload_submit(&req, &resp) == WP_OK) {
        check(WP_TRUE, "offload_submit(OFFLOAD_GRAMMAR_CHECK) returned WP_OK");
        check(resp.status == OFFLOAD_OK, "response status is OFFLOAD_OK");
        printf("  grammar issues for \"%s\":\n", paragraph);
        print_grammar_issues(resp.result, resp.result_len, paragraph, (u32)strlen(paragraph));
        offload_response_free(&resp);
    } else {
        check(WP_FALSE, "offload_submit(OFFLOAD_GRAMMAR_CHECK) returned WP_OK");
    }

    req.op = OFFLOAD_CHAT;
    req.payload = (const u8 *)chat_prompt;
    req.payload_len = (u32)strlen(chat_prompt);
    req.timeout_ms = 90000; /* cold model load (shares GRAMMAR_CHECK's model, may already be warm) + generation */

    if (offload_submit(&req, &resp) == WP_OK) {
        check(WP_TRUE, "offload_submit(OFFLOAD_CHAT) returned WP_OK");
        check(resp.status == OFFLOAD_OK, "response status is OFFLOAD_OK");
        printf("  chat reply to \"%s\":\n  %.*s\n", chat_prompt, (int)resp.result_len, resp.result);
        offload_response_free(&resp);
    } else {
        check(WP_FALSE, "offload_submit(OFFLOAD_CHAT) returned WP_OK");
    }

    offload_shutdown();
    check(offload_available() == WP_FALSE, "offload_available() false after offload_shutdown");

    printf("OLLAMA GATEWAY SMOKE: done\n");
    if (g_fail == 0) {
        printf("OLLAMA GATEWAY SMOKE: ALL PASSED (%d checks)\n", g_pass);
        return 0;
    }
    printf("OLLAMA GATEWAY SMOKE: %d FAILURE(S)\n", g_fail);
    return 1;
}
