#include <string.h>
#include "fixtures.h"

typedef struct {
    const char *word;
    const char *const *synonyms;
    u32 synonym_count;
} thesaurus_entry;

static const char *const k_syn_happy[] = { "glad", "joyful", "content" };
static const char *const k_syn_fast[]  = { "quick", "swift", "rapid" };
static const char *const k_syn_big[]   = { "large", "huge", "massive" };

static const thesaurus_entry k_thesaurus[] = {
    { "happy", k_syn_happy, 3 },
    { "fast",  k_syn_fast,  3 },
    { "big",   k_syn_big,   3 }
};

typedef struct {
    const char *misspelling;
    const char *const *suggestions;
    u32 suggestion_count;
} spelling_entry;

static const char *const k_sugg_teh[]      = { "the" };
static const char *const k_sugg_recieve[]  = { "receive" };
static const char *const k_sugg_seperate[] = { "separate" };

static const spelling_entry k_spellings[] = {
    { "teh",      k_sugg_teh,      1 },
    { "recieve",  k_sugg_recieve,  1 },
    { "seperate", k_sugg_seperate, 1 }
};

/* Offsets below were computed with `python3 -c "print(s.index(...))"`
   against the literal paragraph string, not counted by hand -- this
   project has twice already shipped an off-by-one from hand-counted
   string lengths (see docs/atari-platform.md's T7.4 section), so every
   fixture offset here follows that same verify-with-code discipline. */
static const fixtures_grammar_issue k_issues_team_ready[] = {
    { 4, 8, "Subject-verb agreement", "team is" },
    { 20, 11, "Wrong word: possessive for contraction", "They're going" }
};

typedef struct {
    const char *paragraph;
    const fixtures_grammar_issue *issues;
    u32 issue_count;
} grammar_entry;

static const grammar_entry k_grammar[] = {
    { "The team are ready. Their going to present it today.",
      k_issues_team_ready, 2 }
};

static wp_bool word_eq(const u8 *word, u32 len, const char *candidate)
{
    u32 clen = (u32)strlen(candidate);

    if (clen != len) return WP_FALSE;
    return (memcmp(word, candidate, len) == 0) ? WP_TRUE : WP_FALSE;
}

u32 fixtures_thesaurus_lookup(const u8 *word, u32 len, const char *const **out)
{
    u32 i;

    for (i = 0; i < sizeof(k_thesaurus) / sizeof(k_thesaurus[0]); i++) {
        if (word_eq(word, len, k_thesaurus[i].word)) {
            *out = k_thesaurus[i].synonyms;
            return k_thesaurus[i].synonym_count;
        }
    }
    return 0;
}

wp_bool fixtures_spellcheck_lookup(const u8 *word, u32 len,
                                    const char *const **out, u32 *out_count)
{
    u32 i;

    for (i = 0; i < sizeof(k_spellings) / sizeof(k_spellings[0]); i++) {
        if (word_eq(word, len, k_spellings[i].misspelling)) {
            *out = k_spellings[i].suggestions;
            *out_count = k_spellings[i].suggestion_count;
            return WP_TRUE;
        }
    }
    return WP_FALSE;
}

u32 fixtures_grammar_lookup(const u8 *paragraph, u32 len, const fixtures_grammar_issue **out)
{
    u32 i;

    for (i = 0; i < sizeof(k_grammar) / sizeof(k_grammar[0]); i++) {
        if (word_eq(paragraph, len, k_grammar[i].paragraph)) {
            *out = k_grammar[i].issues;
            return k_grammar[i].issue_count;
        }
    }
    return 0;
}
