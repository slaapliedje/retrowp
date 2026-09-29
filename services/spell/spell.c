/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "spell.h"
#include "platform.h"

struct spell_dict {
    plat_file *f;
    u32 size;
};

wp_status spell_dict_open(const char *native_path, spell_dict **out)
{
    spell_dict *d;
    plat_file *f;
    u32 size;

    if (plat_file_open(native_path, PLAT_FILE_READ, &f) != WP_OK) return WP_ERR;
    if (plat_file_size(f, &size) != WP_OK) { plat_file_close(f); return WP_ERR; }

    d = (spell_dict *)mem_alloc((u32)sizeof(spell_dict));
    if (d == NULL) { plat_file_close(f); return WP_NOMEM; }
    d->f = f;
    d->size = size;
    *out = d;
    return WP_OK;
}

void spell_dict_close(spell_dict *d)
{
    if (d == NULL) return;
    plat_file_close(d->f);
    mem_free(d);
}

/* Scans backward from `offset` to find the start of the line CONTAINING
   that byte (not the next line -- see the comment on
   read_line_at_or_before below for why that distinction matters).
   Byte-at-a-time reads keep peak RAM at one stack buffer regardless of
   dictionary size (docs/services/spell.md). offset must be < d->size
   (the only caller, spell_dict_find, maintains that invariant). */
static wp_status find_line_start(spell_dict *d, u32 offset, u32 *out_start)
{
    u32 pos = offset;

    while (pos > 0) {
        u8 c;
        u32 n;

        if (plat_file_seek(d->f, (i32)(pos - 1), PLAT_SEEK_SET) != WP_OK) return WP_ERR;
        if (plat_file_read(d->f, &c, 1, &n) != WP_OK || n != 1) return WP_ERR;
        if (c == '\n') break; /* the byte just before pos ends the previous line -- pos starts this one */
        pos--;
    }
    *out_start = pos;
    return WP_OK;
}

/* Reads the word (without its trailing '\n') of the line that CONTAINS
   byte offset `offset`, via *line_start (<= offset) and *word_len.

   This must land on the line CONTAINING offset, not the next line
   after it -- landing on the next line broke binary-search convergence
   in an earlier version of this function: when a probe fell inside a
   line whose word compared greater than the target, skipping ahead to
   read the *next* line's word instead (and comparing that) could set
   `hi` back to a value the search had already visited, so the loop
   never made progress and span forever on some inputs (a real bug,
   caught by test_spell_realistic_dictionary_sample hanging rather than
   failing -- see the git history for the trace that found it). Landing
   on the containing line instead guarantees `line_start <= offset`,
   which is what proves each iteration of spell_dict_find's loop
   strictly shrinks its search range. */
static wp_status read_line_at_or_before(spell_dict *d, u32 offset,
                                         char *word, u32 *word_len, u32 *line_start)
{
    u32 pos;

    if (find_line_start(d, offset, &pos) != WP_OK) return WP_ERR;
    if (plat_file_seek(d->f, (i32)pos, PLAT_SEEK_SET) != WP_OK) return WP_ERR;

    {
        u32 len = 0;
        for (;;) {
            u8 c;
            u32 n;

            if (plat_file_read(d->f, &c, 1, &n) != WP_OK || n == 0 || c == '\n') break;
            if (len < SPELL_MAX_WORD_LEN - 1) word[len++] = (char)c;
        }
        word[len] = '\0';
        *word_len = len;
    }
    *line_start = pos;
    return WP_OK;
}

static int spell_word_cmp(const char *a, u32 a_len, const char *b, u32 b_len)
{
    u32 n = (a_len < b_len) ? a_len : b_len;
    u32 i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return (a[i] < b[i]) ? -1 : 1;
    }
    if (a_len == b_len) return 0;
    return (a_len < b_len) ? -1 : 1;
}

/* Binary search over the sorted dictionary file. word must already be
   lowercase ASCII. */
static wp_status spell_dict_find(spell_dict *d, const char *word, u32 word_len, wp_bool *out_found)
{
    u32 lo = 0, hi = d->size;

    *out_found = WP_FALSE;
    if (word_len == 0 || word_len >= SPELL_MAX_WORD_LEN) return WP_OK;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        char line_word[SPELL_MAX_WORD_LEN];
        u32 line_word_len, line_start;
        int cmp;

        if (read_line_at_or_before(d, mid, line_word, &line_word_len, &line_start) != WP_OK) {
            hi = mid; /* unreadable at/before mid -- shrink the search up */
            continue;
        }

        cmp = spell_word_cmp(line_word, line_word_len, word, word_len);
        if (cmp == 0) { *out_found = WP_TRUE; return WP_OK; }
        /* line_start <= mid < hi always (find_line_start never scans
           past mid), so both updates below are guaranteed to strictly
           shrink [lo, hi) -- see read_line_at_or_before's comment. */
        if (cmp < 0) lo = line_start + line_word_len + 1; /* +1 for '\n'; > mid, since mid <= line_start + line_word_len */
        else         hi = line_start;
    }
    return WP_OK;
}

/* Lowercases utf8_word into out (capacity SPELL_MAX_WORD_LEN). Returns
   WP_FALSE if the word contains a byte >= 0x80 or doesn't fit --
   both outside this service's ASCII-only scope (docs/services/spell.md). */
static wp_bool spell_to_ascii_lower(const u8 *utf8_word, u32 len, char *out, u32 *out_len)
{
    u32 i;

    if (len == 0 || len >= SPELL_MAX_WORD_LEN) return WP_FALSE;
    for (i = 0; i < len; i++) {
        u8 c = utf8_word[i];
        if (c >= 0x80) return WP_FALSE;
        out[i] = (char)((c >= 'A' && c <= 'Z') ? (c + 32) : c);
    }
    *out_len = len;
    return WP_TRUE;
}

typedef struct {
    const char *strip_suffix;
    const char *add_suffix;
} spell_affix_rule;

/* Compact, fixed suffix-stripping rules -- not Hunspell-compatible, not
   exhaustive (no consonant doubling, no irregular forms, no prefixes).
   docs/services/spell.md explains why this scope was chosen. */
static const spell_affix_rule AFFIX_RULES[] = {
    { "s",   "" },
    { "es",  "" },
    { "ies", "y" },
    { "ed",  "" },
    { "ed",  "e" },
    { "ing", "" },
    { "ing", "e" },
    { "ly",  "" },
    { "er",  "" },
    { "est", "" }
};
#define AFFIX_RULE_COUNT (sizeof(AFFIX_RULES) / sizeof(AFFIX_RULES[0]))

wp_status spell_check_word(spell_dict *d, const u8 *utf8_word, u32 len, wp_bool *out_known)
{
    char lower[SPELL_MAX_WORD_LEN];
    u32 lower_len;
    u32 i;
    wp_bool found;

    if (!spell_to_ascii_lower(utf8_word, len, lower, &lower_len)) {
        *out_known = WP_TRUE; /* fail open -- see docs/services/spell.md */
        return WP_OK;
    }

    if (spell_dict_find(d, lower, lower_len, &found) != WP_OK) return WP_ERR;
    if (found) { *out_known = WP_TRUE; return WP_OK; }

    for (i = 0; i < AFFIX_RULE_COUNT; i++) {
        u32 strip_len = (u32)strlen(AFFIX_RULES[i].strip_suffix);
        u32 add_len = (u32)strlen(AFFIX_RULES[i].add_suffix);
        char candidate[SPELL_MAX_WORD_LEN];
        u32 cand_len;

        if (lower_len <= strip_len) continue; /* stem must be non-empty */
        if (memcmp(lower + lower_len - strip_len, AFFIX_RULES[i].strip_suffix, strip_len) != 0) continue;

        cand_len = lower_len - strip_len;
        if (cand_len + add_len >= SPELL_MAX_WORD_LEN) continue;
        memcpy(candidate, lower, cand_len);
        memcpy(candidate + cand_len, AFFIX_RULES[i].add_suffix, add_len);
        cand_len += add_len;

        if (spell_dict_find(d, candidate, cand_len, &found) != WP_OK) return WP_ERR;
        if (found) { *out_known = WP_TRUE; return WP_OK; }
    }

    *out_known = WP_FALSE;
    return WP_OK;
}

static wp_status try_candidate(spell_dict *d, const char *cand, u32 cand_len,
                                char out[][SPELL_MAX_WORD_LEN], u32 *count, u32 max)
{
    wp_bool found;
    u32 i;

    if (*count >= max) return WP_OK;
    if (spell_dict_find(d, cand, cand_len, &found) != WP_OK) return WP_ERR;
    if (!found) return WP_OK;

    for (i = 0; i < *count; i++) {
        if (strlen(out[i]) == cand_len && memcmp(out[i], cand, cand_len) == 0) return WP_OK;
    }

    memcpy(out[*count], cand, cand_len);
    out[*count][cand_len] = '\0';
    (*count)++;
    return WP_OK;
}

wp_status spell_suggest(spell_dict *d, const u8 *utf8_word, u32 len,
                         char out[][SPELL_MAX_WORD_LEN], u32 max_suggestions, u32 *out_count)
{
    char lower[SPELL_MAX_WORD_LEN];
    u32 lower_len;
    u32 i, j;
    char c;

    *out_count = 0;
    if (!spell_to_ascii_lower(utf8_word, len, lower, &lower_len)) return WP_OK;
    if (lower_len + 1 >= SPELL_MAX_WORD_LEN) return WP_OK; /* insertion candidates need +1 room */

    /* deletions */
    for (i = 0; i < lower_len && *out_count < max_suggestions; i++) {
        char cand[SPELL_MAX_WORD_LEN];
        u32 k = 0;

        for (j = 0; j < lower_len; j++) if (j != i) cand[k++] = lower[j];
        if (try_candidate(d, cand, k, out, out_count, max_suggestions) != WP_OK) return WP_ERR;
    }

    /* transpositions */
    for (i = 0; i + 1 < lower_len && *out_count < max_suggestions; i++) {
        char cand[SPELL_MAX_WORD_LEN];

        memcpy(cand, lower, lower_len);
        cand[i] = lower[i + 1];
        cand[i + 1] = lower[i];
        if (try_candidate(d, cand, lower_len, out, out_count, max_suggestions) != WP_OK) return WP_ERR;
    }

    /* substitutions */
    for (i = 0; i < lower_len && *out_count < max_suggestions; i++) {
        for (c = 'a'; c <= 'z' && *out_count < max_suggestions; c++) {
            char cand[SPELL_MAX_WORD_LEN];

            if (c == lower[i]) continue;
            memcpy(cand, lower, lower_len);
            cand[i] = c;
            if (try_candidate(d, cand, lower_len, out, out_count, max_suggestions) != WP_OK) return WP_ERR;
        }
    }

    /* insertions */
    for (i = 0; i <= lower_len && *out_count < max_suggestions; i++) {
        for (c = 'a'; c <= 'z' && *out_count < max_suggestions; c++) {
            char cand[SPELL_MAX_WORD_LEN];

            memcpy(cand, lower, i);
            cand[i] = c;
            memcpy(cand + i + 1, lower + i, lower_len - i);
            if (try_candidate(d, cand, lower_len + 1, out, out_count, max_suggestions) != WP_OK) return WP_ERR;
        }
    }

    return WP_OK;
}
