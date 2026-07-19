#ifndef WP_GRAMMAR_H
#define WP_GRAMMAR_H

#include "types.h"

/* Compact, purely mechanical grammar-lite checker. Pure C, data-driven,
   no UI, no platform-specific calls at all (not even platform.h's
   plat_file_ calls or mem_alloc -- there is no data file and no allocation,
   just an in-memory scan) -- per CLAUDE.md's architecture layers, this
   is shared, portable code, compiled once and linked everywhere. Full
   design (the rule set, why each rule is scoped the way it is, and the
   real gaps versus a real grammar checker) is in
   docs/services/grammar.md -- read it before changing this file.

   M8 T8.2: this exists so Grammar Check (platform/atari/app_shell.c's
   Tools > Grammar Check) has SOMETHING to fall back to when no offload
   device is attached -- CLAUDE.md's Offload Rules require the engine
   stay fully functional without one. It is NOT a smaller version of the
   LLM-backed OFFLOAD_GRAMMAR_CHECK path (M8 T8.1) -- it catches a much
   narrower set of purely mechanical issues that need no semantic
   understanding of the text at all. */

#define GRAMMAR_MAX_MESSAGE_LEN    40
#define GRAMMAR_MAX_SUGGESTION_LEN 40

typedef struct {
    u32  start_offset;                          /* byte offset into the checked text */
    u32  span_len;                               /* byte length of the flagged span */
    char message[GRAMMAR_MAX_MESSAGE_LEN];        /* NUL-terminated, ASCII */
    char suggestion[GRAMMAR_MAX_SUGGESTION_LEN];  /* NUL-terminated, ASCII, always non-empty */
} grammar_issue;

/* Scans text[0..len) for basic, purely mechanical issues (doubled
   words, multiple spaces, space before punctuation, repeated !/?, a
   lowercase letter at the very start of the text) and fills out[] with
   up to max_issues found. Returns the number of issues written -- 0 is
   a valid, common answer ("nothing wrong found"), not an error. Issues
   are NOT guaranteed to be in ascending start_offset order (each rule
   runs as its own independent left-to-right pass over the whole text);
   sort them yourself if presentation order matters -- callers that
   already need to sort a mixed local/gateway issue list (like
   app_shell.c's do_grammar_check()) get this for free by construction.
   Every issue's suggestion is a real, ready-to-apply replacement for
   its span -- never an empty "flagged, no fix offered" placeholder, by
   deliberate rule design (docs/services/grammar.md). */
u32 grammar_lite_check(const u8 *text, u32 len, grammar_issue *out, u32 max_issues);

#endif /* WP_GRAMMAR_H */
