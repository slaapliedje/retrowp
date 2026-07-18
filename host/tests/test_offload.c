#include <string.h>
#include "test.h"
#include "offload.h"
#include "endian.h"
#include "gateway_harness.h"

/* Decodes a length-prefixed word-list payload (u16 count, then count x
   (u16 len, UTF-8 bytes) -- see docs/formats/offload-protocol.md) just
   far enough to check whether `target` is one of the words. */
static wp_bool word_list_contains(const u8 *buf, u32 len, const char *target)
{
    u16 count;
    u32 pos, i;
    u32 target_len = (u32)strlen(target);

    if (len < 2) return WP_FALSE;
    count = be_read16(buf);
    pos = 2;

    for (i = 0; i < count; i++) {
        u16 wlen;
        if (pos + 2 > len) return WP_FALSE;
        wlen = be_read16(buf + pos);
        pos += 2;
        if (pos + wlen > len) return WP_FALSE;
        if (wlen == target_len && memcmp(buf + pos, target, wlen) == 0) return WP_TRUE;
        pos += wlen;
    }
    return WP_FALSE;
}

static wp_status submit_word(offload_op op, const char *word, u32 timeout_ms,
                              offload_response *resp)
{
    offload_request req;

    req.op = op;
    req.payload = (const u8 *)word;
    req.payload_len = (u32)strlen(word);
    req.timeout_ms = timeout_ms;
    return offload_submit(&req, resp);
}

void test_offload_available_false_without_device(void)
{
    ASSERT_TRUE(offload_init("tcp:127.0.0.1:1") != WP_OK);
    ASSERT_TRUE(offload_available() == WP_FALSE);
    offload_shutdown(); /* must be safe even though init failed */
}

void test_offload_thesaurus_happy_path(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ASSERT_TRUE(offload_available());

    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "happy", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(word_list_contains(resp.result, resp.result_len, "glad"));
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_thesaurus_unknown_word_returns_empty_list(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "zzznotaword", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_EQ_U32(resp.result_len, 2); /* just the u16 count == 0 */
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_spellcheck_flags_known_misspelling(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(submit_word(OFFLOAD_SPELL_CHECK, "teh", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len >= 1);
    ASSERT_EQ_U32(resp.result[0], 0); /* is_correct == false */
    ASSERT_TRUE(word_list_contains(resp.result + 1, resp.result_len - 1, "the"));
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_spellcheck_correct_word(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(submit_word(OFFLOAD_SPELL_CHECK, "hello", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_TRUE(resp.result_len >= 1);
    ASSERT_EQ_U32(resp.result[0], 1); /* is_correct == true: not in the misspelling table */
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_unsupported_op_reports_unsupported(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(submit_word(OFFLOAD_CHAT, "hello", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_corrupt_response_is_rejected(void)
{
    gateway_handle gw;
    offload_response resp;
    static const char *args[] = { "--corrupt-after=0", NULL };

    ASSERT_TRUE(gateway_start(args, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* The gateway's one reply is corrupted and silently rejected by
       wire_read_frame; since it never sends anything more, the client
       must time out rather than accept a corrupted frame or hang. */
    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "happy", 800, &resp) == WP_TIMEOUT);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_timeout_on_delayed_gateway(void)
{
    gateway_handle gw;
    offload_response resp;
    static const char *args[] = { "--delay-ms=1500", NULL };

    ASSERT_TRUE(gateway_start(args, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* Client timeout (300ms) is well under the gateway's artificial
       1500ms delay -- a fully deterministic timeout, no race. */
    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "happy", 300, &resp) == WP_TIMEOUT);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_gateway_dies_before_responding(void)
{
    gateway_handle gw;
    offload_response resp;
    static const char *args[] = { "--die-after=0", NULL };

    ASSERT_TRUE(gateway_start(args, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* Gateway receives the request, then exits abruptly instead of
       responding (T6.4 AC: "timeout path verified by killing the
       gateway mid-request"). The client must detect this and return
       promptly -- not hang -- whether that surfaces as a connection
       error or a timeout depends on how quickly the OS notices the
       peer is gone, so either is an acceptable pass here. */
    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "happy", 2000, &resp) != WP_OK);

    offload_shutdown();
    gateway_stop(&gw); /* already dead; just reaps if anything is left */
}

void test_offload_short_frame_times_out(void)
{
    gateway_handle gw;
    offload_response resp;
    static const char *args[] = { "--short-frame", NULL };

    ASSERT_TRUE(gateway_start(args, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* Gateway promises a large payload and delivers only a few bytes of
       it -- the client must time out waiting for the rest, not hang or
       misinterpret the partial data as a complete frame. */
    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "happy", 800, &resp) == WP_TIMEOUT);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_submit_fails_after_gateway_killed(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    gateway_kill(&gw);

    ASSERT_TRUE(submit_word(OFFLOAD_THESAURUS, "happy", 2000, &resp) != WP_OK);

    offload_shutdown();
}
