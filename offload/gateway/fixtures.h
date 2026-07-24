#ifndef WP_GATEWAY_FIXTURES_H
#define WP_GATEWAY_FIXTURES_H

#include "types.h"

/* Canned THESAURUS/SPELL_CHECK data the mock gateway serves, so tests get
   deterministic responses without a real dictionary or LLM. Real gateway
   logic (M8) replaces this with an actual dictionary/LLM backend behind
   the same wire protocol -- callers of this module never change. */

/* Looks up word (UTF-8, `len` bytes) in the synonym table. Returns the
   number of synonyms found (0 if the word is unknown -- "no synonyms
   found" is a valid answer, not an error) and, if non-zero, sets *out to
   an array of that many NUL-terminated C strings (static storage, valid
   for the process lifetime). *out is untouched when the return value is
   0. */
u32 fixtures_thesaurus_lookup(const u8 *word, u32 len, const char *const **out);

/* Looks up word in the misspelling table. Returns WP_TRUE if the word is
   a KNOWN misspelling, and if so sets *out and *out_count to its
   suggestion list (static storage). Returns WP_FALSE (word is treated as
   correctly spelled) if not found -- out and out_count are untouched in
   that case. */
wp_bool fixtures_spellcheck_lookup(const u8 *word, u32 len,
                                    const char *const **out, u32 *out_count);

/* One canned OFFLOAD_GRAMMAR_CHECK issue. start_offset/span_len are byte
   offsets into the fixture paragraph they're associated with -- trusted
   by construction (hand-verified against the literal fixture string, not
   computed by a substring search the way ollama_gateway.c's real
   handler has to). */
typedef struct {
    u16         start_offset;
    u16         span_len;
    const char *message;
    const char *suggestion; /* may be "" for "flagged, no fix offered" */
} fixtures_grammar_issue;

/* Looks up paragraph (UTF-8, `len` bytes) in the canned grammar-issue
   table by EXACT match. Returns the number of issues found (0 if the
   paragraph isn't a recognized fixture -- "no issues" is a valid answer
   for an unrecognized/clean paragraph, not an error) and, if non-zero,
   sets *out to an array of that many issues (static storage, valid for
   the process lifetime). *out is untouched when the return value is 0. */
u32 fixtures_grammar_lookup(const u8 *paragraph, u32 len, const fixtures_grammar_issue **out);

/* Looks up prompt (UTF-8, `len` bytes) in the canned chat-reply table.
   ALWAYS succeeds -- an unrecognized prompt gets a fixed generic reply,
   never an error, the same "always answer, don't fail on unrecognized
   input" convention an unknown THESAURUS word already gets (an empty
   synonym list, not OFFLOAD_ERR). Returns a NUL-terminated C string
   (static storage, valid for the process lifetime) and its length via
   *out_len. */
const char *fixtures_chat_reply(const u8 *prompt, u32 len, u32 *out_len);

/* One canned OFFLOAD_IMAGE response (M15): a tiny, fixed 4x4 2-color
   checkerboard, returned unconditionally regardless of the request's
   own source bytes/target constraints. This mock gateway has no
   Pillow dependency and never decodes an image -- unlike THESAURUS/
   SPELL_CHECK/GRAMMAR_CHECK/CHAT there is nothing to look UP, so this
   exists purely to give test_offload.c deterministic wire-level
   coverage of IMAGE's encode/decode and capability gating. Real
   content correctness (a genuine Pillow decode/resize/quantize) is
   proven separately, against the real image_gateway.py, by
   host/tests/test_image_convert.c. */
typedef struct {
    u16        width_px;
    u16        height_px;
    u8         palette_count;
    const u8 (*palette)[3];
    const u8  *packed_pixels;
    u32        packed_len;
} fixtures_image;

const fixtures_image *fixtures_image_get(void);

#endif /* WP_GATEWAY_FIXTURES_H */
