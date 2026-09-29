/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "grammar.h"

/* ASCII-only, same "fails open" scope discipline services/spell already
   uses (docs/services/spell.md): any byte >= 0x80 is simply not alpha/
   space/punctuation to this scanner, so UTF-8 multi-byte sequences are
   never misread as ASCII content -- they just act as inert separators,
   invisible to every rule below rather than flagged incorrectly. */
static wp_bool g_is_alpha(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static wp_bool g_is_punct(u8 c)
{
    return c == ',' || c == '.' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')';
}

static u8 g_to_upper(u8 c)
{
    return (c >= 'a' && c <= 'z') ? (u8)(c - 32) : c;
}

static void copy_fixed(char *out, u32 out_cap, const char *s)
{
    u32 len = (u32)strlen(s);
    if (len >= out_cap) len = out_cap - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

static void copy_fixed_bytes(char *out, u32 out_cap, const u8 *s, u32 s_len)
{
    u32 len = (s_len < out_cap - 1) ? s_len : out_cap - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

/* Two identical (case-insensitive) alpha words separated by nothing but
   spaces -- the classic "the the" typo. Flags the WHOLE phrase (both
   words and the space between) and suggests a single copy of the word,
   so Apply genuinely fixes it rather than just pointing it out. Résumés
   scanning from the end of the SECOND word on a match (not the first),
   so "a a a" reports one issue covering the first pair and then
   correctly re-examines the remaining "a" against nothing. */
static u32 check_doubled_words(const u8 *text, u32 len, grammar_issue *out, u32 max_issues, u32 count)
{
    u32 i = 0;

    while (i < len && count < max_issues) {
        u32 w1_start, w1_end, w2_start, w2_end, wlen;
        wp_bool same;

        while (i < len && !g_is_alpha(text[i])) i++;
        w1_start = i;
        while (i < len && g_is_alpha(text[i])) i++;
        w1_end = i;
        if (w1_end == w1_start) break; /* no more words in the text */

        w2_start = w1_end;
        while (w2_start < len && text[w2_start] == ' ') w2_start++;
        w2_end = w2_start;
        while (w2_end < len && g_is_alpha(text[w2_end])) w2_end++;

        wlen = w1_end - w1_start;
        same = ((w2_end - w2_start) == wlen);
        if (same) {
            u32 k;
            for (k = 0; k < wlen; k++) {
                if (g_to_upper(text[w1_start + k]) != g_to_upper(text[w2_start + k])) {
                    same = WP_FALSE;
                    break;
                }
            }
        }

        if (same) {
            out[count].start_offset = w1_start;
            out[count].span_len = w2_end - w1_start;
            copy_fixed(out[count].message, (u32)sizeof(out[count].message), "Repeated word");
            copy_fixed_bytes(out[count].suggestion, (u32)sizeof(out[count].suggestion),
                              text + w1_start, wlen);
            count++;
            i = w2_end;
        } else {
            i = w1_end;
        }
    }
    return count;
}

/* A run of 2+ literal spaces -- suggests collapsing to exactly one. */
static u32 check_multiple_spaces(const u8 *text, u32 len, grammar_issue *out, u32 max_issues, u32 count)
{
    u32 i = 0;

    while (i < len && count < max_issues) {
        u32 start;

        if (text[i] != ' ') { i++; continue; }
        start = i;
        while (i < len && text[i] == ' ') i++;

        if (i - start >= 2) {
            out[count].start_offset = start;
            out[count].span_len = i - start;
            copy_fixed(out[count].message, (u32)sizeof(out[count].message), "Multiple spaces");
            copy_fixed(out[count].suggestion, (u32)sizeof(out[count].suggestion), " ");
            count++;
        }
    }
    return count;
}

/* One or more spaces immediately followed by a closing punctuation mark
   (",.;:!?)"). The flagged span INCLUDES the punctuation mark itself,
   and the suggestion is just that mark alone -- "word , next" becomes
   "word, next" as a single delete-and-replace, not a bare deletion
   (which this module never produces -- see grammar.h). */
static u32 check_space_before_punct(const u8 *text, u32 len, grammar_issue *out, u32 max_issues, u32 count)
{
    u32 i = 0;

    while (i < len && count < max_issues) {
        u32 start;

        if (text[i] != ' ') { i++; continue; }
        start = i;
        while (i < len && text[i] == ' ') i++;

        if (i < len && g_is_punct(text[i])) {
            char s[2];

            out[count].start_offset = start;
            out[count].span_len = (i - start) + 1;
            copy_fixed(out[count].message, (u32)sizeof(out[count].message), "Space before punctuation");
            s[0] = (char)text[i];
            s[1] = '\0';
            copy_fixed(out[count].suggestion, (u32)sizeof(out[count].suggestion), s);
            count++;
            i++;
        }
    }
    return count;
}

/* Two or more of the SAME character, restricted to '!'/'?' only --
   deliberately excludes '.'/',' to avoid flagging a legitimate "..."
   ellipsis (docs/services/grammar.md). Doesn't catch mixed clusters
   like "?!?!" -- a documented, real gap, not an oversight. */
static u32 check_repeated_punct(const u8 *text, u32 len, grammar_issue *out, u32 max_issues, u32 count)
{
    u32 i = 0;

    while (i < len && count < max_issues) {
        u32 start;
        u8 c = text[i];

        if (c != '!' && c != '?') { i++; continue; }
        start = i;
        while (i < len && text[i] == c) i++;

        if (i - start >= 2) {
            char s[2];

            out[count].start_offset = start;
            out[count].span_len = i - start;
            copy_fixed(out[count].message, (u32)sizeof(out[count].message), "Repeated punctuation");
            s[0] = (char)c;
            s[1] = '\0';
            copy_fixed(out[count].suggestion, (u32)sizeof(out[count].suggestion), s);
            count++;
        }
    }
    return count;
}

/* The first alpha character in the WHOLE text is lowercase -- scoped to
   text-initial only, not every sentence boundary within it, to avoid
   the abbreviation/decimal-point false positives a real mid-text
   "starts a new sentence" detector would need to handle
   (docs/services/grammar.md). Since app_shell.c always passes exactly
   one paragraph, this is "this paragraph doesn't start with a capital
   letter" in practice. */
static u32 check_missing_initial_capital(const u8 *text, u32 len, grammar_issue *out, u32 max_issues, u32 count)
{
    u32 i = 0;

    if (count >= max_issues) return count;

    while (i < len && !g_is_alpha(text[i])) i++;
    if (i < len && text[i] >= 'a' && text[i] <= 'z') {
        char s[2];

        out[count].start_offset = i;
        out[count].span_len = 1;
        copy_fixed(out[count].message, (u32)sizeof(out[count].message),
                   "Missing capital letter");
        s[0] = (char)g_to_upper(text[i]);
        s[1] = '\0';
        copy_fixed(out[count].suggestion, (u32)sizeof(out[count].suggestion), s);
        count++;
    }
    return count;
}

u32 grammar_lite_check(const u8 *text, u32 len, grammar_issue *out, u32 max_issues)
{
    u32 count = 0;

    count = check_missing_initial_capital(text, len, out, max_issues, count);
    count = check_doubled_words(text, len, out, max_issues, count);
    count = check_multiple_spaces(text, len, out, max_issues, count);
    count = check_space_before_punct(text, len, out, max_issues, count);
    count = check_repeated_punct(text, len, out, max_issues, count);

    return count;
}
