#include <stdio.h>
#include <string.h>
#include "types.h"
#include "endian.h"
#include "offload.h"

/* Manual (NOT part of `make test`/ci.sh) end-to-end proof that the real
   offload_client.c API -- the exact same code path platform/atari's
   future Thesaurus menu action or a Pi deployment would use, not a
   hand-rolled protocol client -- gets genuine LLM-generated synonyms
   back from a live offload/gateway/ollama_gateway.c talking to a real
   Ollama server. Deliberately excluded from the automated suite: LLM
   output is not deterministic (see docs/formats/offload-protocol.md),
   and a contributor without Ollama installed must still be able to run
   `make test`/`ci.sh` successfully -- this tool exists so a human can
   verify the real thing works, not so CI can assert exact output.

   Usage: ollama_gateway_smoke <endpoint> [word]
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

int main(int argc, char **argv)
{
    const char *endpoint;
    const char *word = "happy";
    offload_capabilities caps;
    offload_request req;
    offload_response resp;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <endpoint> [word]\n", argv[0]);
        return 1;
    }
    endpoint = argv[1];
    if (argc >= 3) word = argv[2];

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
