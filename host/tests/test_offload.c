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

typedef struct {
    u16       start_offset;
    u16       span_len;
    const u8 *message;
    u32       message_len;
    const u8 *suggestion;
    u32       suggestion_len;
} decoded_grammar_issue;

/* Decodes the OFFLOAD_GRAMMAR_CHECK response shape (u16 issue_count, then
   issue_count x (u16 start_offset, u16 span_len, u16 message_len + bytes,
   u16 suggestion_len + bytes) -- docs/formats/offload-protocol.md) far
   enough to pull out issue `index`. Returns WP_FALSE on a short/malformed
   buffer or an out-of-range index. */
static wp_bool grammar_issue_at(const u8 *buf, u32 len, u32 index, decoded_grammar_issue *out)
{
    u16 count;
    u32 pos, i;

    if (len < 2) return WP_FALSE;
    count = be_read16(buf);
    pos = 2;
    if (index >= count) return WP_FALSE;

    for (i = 0; i <= index; i++) {
        u16 mlen, slen;

        if (pos + 6 > len) return WP_FALSE;
        out->start_offset = be_read16(buf + pos); pos += 2;
        out->span_len     = be_read16(buf + pos); pos += 2;
        mlen = be_read16(buf + pos); pos += 2;

        if (pos + mlen + 2 > len) return WP_FALSE;
        if (i == index) { out->message = buf + pos; out->message_len = mlen; }
        pos += mlen;

        slen = be_read16(buf + pos); pos += 2;
        if (pos + slen > len) return WP_FALSE;
        if (i == index) { out->suggestion = buf + pos; out->suggestion_len = slen; }
        pos += slen;
    }
    return WP_TRUE;
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

void test_offload_grammar_check_known_fixture(void)
{
    gateway_handle gw;
    offload_response resp;
    decoded_grammar_issue issue;
    static const char *para = "The team are ready. Their going to present it today.";

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(submit_word(OFFLOAD_GRAMMAR_CHECK, para, 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_EQ_U32(be_read16(resp.result), 2);

    ASSERT_TRUE(grammar_issue_at(resp.result, resp.result_len, 0, &issue));
    ASSERT_EQ_U32(issue.start_offset, 4);
    ASSERT_EQ_U32(issue.span_len, 8);
    ASSERT_TRUE(issue.message_len == strlen("Subject-verb agreement"));
    ASSERT_MEM_EQ(issue.message, "Subject-verb agreement", issue.message_len);
    ASSERT_TRUE(issue.suggestion_len == strlen("team is"));
    ASSERT_MEM_EQ(issue.suggestion, "team is", issue.suggestion_len);
    /* The response's own offset/length actually locate the flagged text
       inside the ORIGINAL request paragraph -- not just self-consistent
       with the encoder, but genuinely usable by a caller. */
    ASSERT_MEM_EQ(para + issue.start_offset, "team are", issue.span_len);

    ASSERT_TRUE(grammar_issue_at(resp.result, resp.result_len, 1, &issue));
    ASSERT_EQ_U32(issue.start_offset, 20);
    ASSERT_EQ_U32(issue.span_len, 11);
    ASSERT_MEM_EQ(para + issue.start_offset, "Their going", issue.span_len);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_grammar_check_clean_paragraph_returns_no_issues(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* Not a recognized fixture paragraph -- same "unrecognized input
       degrades to an empty, still-OK response" convention THESAURUS
       already uses for an unknown word. */
    ASSERT_TRUE(submit_word(OFFLOAD_GRAMMAR_CHECK, "Nothing wrong with this one.",
                             2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_EQ_U32(resp.result_len, 2); /* just the u16 issue_count == 0 */

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_chat_happy_path(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(submit_word(OFFLOAD_CHAT, "Hello", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_EQ_U32(resp.result_len, (u32)strlen("Hi there! How can I help with your document?"));
    ASSERT_MEM_EQ(resp.result, "Hi there! How can I help with your document?", resp.result_len);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_chat_unrecognized_prompt_gets_generic_reply(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* Not a recognized fixture prompt -- same "always answer, don't
       fail on unrecognized input" convention THESAURUS's unknown-word
       case already established (an empty list, not an error). */
    ASSERT_TRUE(submit_word(OFFLOAD_CHAT, "What time is it?", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);
    ASSERT_EQ_U32(resp.result_len, (u32)strlen("I don't have anything to add."));
    ASSERT_MEM_EQ(resp.result, "I don't have anything to add.", resp.result_len);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_capabilities_advertise_chat_but_not_convert_import(void)
{
    gateway_handle gw;
    offload_capabilities caps;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_CHAT)) != 0);
    /* OFFLOAD_CONVERT_IMPORT, not OFFLOAD_IMAGE: IMAGE is now a real,
       supported op here too (M15 T15.4 -- mock_gateway.c answers it
       from a canned fixture, see fixtures.c), so it can no longer
       stand in as "something this gateway doesn't implement". mock_
       gateway.c permanently, by design, never implements ODT/PDF
       conversion -- that's odt_gateway.py's own exclusive domain
       (docs/formats/offload-protocol.md) -- making CONVERT_IMPORT an
       equally permanent, equally honest replacement stand-in. */
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_CONVERT_IMPORT)) == 0);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_unsupported_op_reports_unsupported(void)
{
    gateway_handle gw;
    offload_response resp;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    /* OFFLOAD_CONVERT_IMPORT, not OFFLOAD_IMAGE: see the capabilities
       test above for why IMAGE no longer serves as "something this
       gateway doesn't implement" stand-in. */
    ASSERT_TRUE(submit_word(OFFLOAD_CONVERT_IMPORT, "hello", 2000, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_UNSUPPORTED);
    offload_response_free(&resp);

    offload_shutdown();
    gateway_stop(&gw);
}

/* M15: OFFLOAD_IMAGE is now real here too, answered from fixtures.c's
   single canned 4x4 checkerboard (fixtures_image_get) -- deterministic,
   Pillow-free wire-level coverage of the encode/decode shape and
   capability advertisement. Real Pillow-backed content correctness is
   proven separately, against the real image_gateway.py, by
   host/tests/test_image_convert.c. */
void test_offload_capabilities_advertise_image(void)
{
    gateway_handle gw;
    offload_capabilities caps;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_IMAGE)) != 0);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_image_returns_canned_fixture_bitmap(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    /* target_max_colors=16, target_flags=0, target_max_width_px=320,
       target_max_height_px=200 -- mock_gateway.c's fixture ignores all
       of this (and the source bytes below) and always answers the same
       canned bitmap; the request still needs a well-formed 6-byte
       header + something after it, matching what a real caller would
       send. */
    static const u8 req_payload[] = {
        16, 0, 0x01, 0x40, 0x00, 0xC8, 'x'
    };

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    req.op = OFFLOAD_IMAGE;
    req.payload = req_payload;
    req.payload_len = (u32)sizeof(req_payload);
    req.timeout_ms = 2000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);

    /* width_px(2) height_px(2) palette_count(1) palette(2*3)
       packed_pixels(8) = 19 bytes, per fixtures.c's k_image. */
    ASSERT_EQ_U32(resp.result_len, 19);
    ASSERT_EQ_U32(be_read16(resp.result), 4);      /* width_px */
    ASSERT_EQ_U32(be_read16(resp.result + 2), 4);  /* height_px */
    ASSERT_EQ_U32(resp.result[4], 2);              /* palette_count */
    ASSERT_EQ_U32(resp.result[5], 0);   /* palette[0] = black */
    ASSERT_EQ_U32(resp.result[6], 0);
    ASSERT_EQ_U32(resp.result[7], 0);
    ASSERT_EQ_U32(resp.result[8], 255); /* palette[1] = white */
    ASSERT_EQ_U32(resp.result[9], 255);
    ASSERT_EQ_U32(resp.result[10], 255);
    /* packed pixels: checkerboard rows 0x01,0x01 / 0x10,0x10 / repeat */
    ASSERT_EQ_U32(resp.result[11], 0x01);
    ASSERT_EQ_U32(resp.result[12], 0x01);
    ASSERT_EQ_U32(resp.result[13], 0x10);
    ASSERT_EQ_U32(resp.result[14], 0x10);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

/* T174: OFFLOAD_DICTATE, answered from fixtures.c's single canned
   transcript (fixtures_dictate_transcript) -- deterministic, wire-level
   coverage of the op's request encoding, its response shape and its
   capability advertisement, with no whisper.cpp build, no 142MB model
   and no microphone anywhere near `make test`.

   Real transcription correctness is deliberately NOT proven here. It is
   proven against the real whisper_gateway.py on the actual Pi (see
   docs/formats/offload-protocol.md's OFFLOAD_DICTATE section) -- the
   same split M15 drew between this file's canned IMAGE checkerboard and
   test_image_convert.c's genuine Pillow decode. The difference is that
   Pillow is pip-installable on any host, so its real test can live in
   `make test`; a whisper model plus a capture device cannot, which is
   an honest limit on what this suite can assert rather than a gap in
   what was verified. */
void test_offload_capabilities_advertise_dictate(void)
{
    gateway_handle gw;
    offload_capabilities caps;

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    ASSERT_TRUE(offload_query(&caps) == WP_OK);
    ASSERT_TRUE((caps.ops_supported & (1uL << OFFLOAD_DICTATE)) != 0);

    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_dictate_returns_canned_transcript(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    static const char k_expected[] = "The quick brown fox jumps over the lazy dog.";
    /* max_duration_s=15, flags=0x01 (trim trailing silence),
       language="en" -- the whole request, exactly 4 bytes. The audio
       never travels, which is what keeps this op viable at 9600 baud. */
    static const u8 req_payload[] = { 15, 0x01, 'e', 'n' };

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    req.op = OFFLOAD_DICTATE;
    req.payload = req_payload;
    req.payload_len = (u32)sizeof(req_payload);
    req.timeout_ms = 2000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_OK);

    /* The response payload IS the transcript: no header, no length
       prefix, just UTF-8 -- so a backend can hand it straight to
       wp_edit_insert_text without an intermediate decode step. */
    ASSERT_EQ_U32(resp.result_len, (u32)strlen(k_expected));
    ASSERT_MEM_EQ(resp.result, k_expected, resp.result_len);

    offload_response_free(&resp);
    offload_shutdown();
    gateway_stop(&gw);
}

void test_offload_dictate_rejects_malformed_request(void)
{
    gateway_handle gw;
    offload_request req;
    offload_response resp;
    /* Three bytes: one short of the fixed 4-byte request. The canned
       transcript is returned unconditionally otherwise, so without this
       check a client that encoded the request wrongly would still see a
       perfectly good transcript come back and the bug would ship. */
    static const u8 short_payload[] = { 15, 0x01, 'e' };

    ASSERT_TRUE(gateway_start(NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);

    req.op = OFFLOAD_DICTATE;
    req.payload = short_payload;
    req.payload_len = (u32)sizeof(short_payload);
    req.timeout_ms = 2000;
    ASSERT_TRUE(offload_submit(&req, &resp) == WP_OK);
    ASSERT_TRUE(resp.status == OFFLOAD_ERR);
    ASSERT_EQ_U32(resp.result_len, 0);

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
