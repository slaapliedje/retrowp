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

#endif /* WP_GATEWAY_FIXTURES_H */
