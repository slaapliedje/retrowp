/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "grammar.h"

/* Finds the (first) issue in out[0..count) whose message matches want --
   tests below don't want to depend on cross-rule ordering (grammar.h
   documents that grammar_lite_check() doesn't sort), just on whether a
   given rule fired and with what span/suggestion. */
static const grammar_issue *find_issue(const grammar_issue *out, u32 count, const char *want_message)
{
    u32 i;
    for (i = 0; i < count; i++) {
        if (strcmp(out[i].message, want_message) == 0) return &out[i];
    }
    return NULL;
}

void test_grammar_doubled_word_flags_whole_phrase_and_suggests_one_copy(void)
{
    static const char text[] = "I saw the the dog run.";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    const grammar_issue *issue = find_issue(out, count, "Repeated word");

    ASSERT_TRUE(issue != NULL);
    ASSERT_EQ_U32(issue->start_offset, 6);  /* "the the" starts at byte 6 */
    ASSERT_EQ_U32(issue->span_len, 7);      /* "the the" is 7 bytes */
    ASSERT_TRUE(strcmp(issue->suggestion, "the") == 0);
}

void test_grammar_doubled_word_is_case_insensitive(void)
{
    static const char text[] = "The The end.";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    const grammar_issue *issue = find_issue(out, count, "Repeated word");

    ASSERT_TRUE(issue != NULL);
    ASSERT_EQ_U32(issue->start_offset, 0);
    ASSERT_EQ_U32(issue->span_len, 7); /* "The The" */
}

void test_grammar_no_doubled_word_for_different_words(void)
{
    static const char text[] = "the cat sat on the mat";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);

    ASSERT_TRUE(find_issue(out, count, "Repeated word") == NULL);
}

void test_grammar_triple_word_reports_one_issue_and_moves_past_it(void)
{
    /* "a a a": the first pair ("a a") is flagged and resolved; scanning
       resumes after it, so the trailing lone "a" isn't itself a match
       (nothing follows it) -- exactly one issue, not two overlapping
       ones. */
    static const char text[] = "a a a";
    grammar_issue out[8];
    u32 count = 0, i, hits = 0;

    count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    for (i = 0; i < count; i++) {
        if (strcmp(out[i].message, "Repeated word") == 0) hits++;
    }
    ASSERT_EQ_U32(hits, 1);
}

void test_grammar_multiple_spaces_suggests_single_space(void)
{
    static const char text[] = "Hello   world";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    const grammar_issue *issue = find_issue(out, count, "Multiple spaces");

    ASSERT_TRUE(issue != NULL);
    ASSERT_EQ_U32(issue->start_offset, 5);
    ASSERT_EQ_U32(issue->span_len, 3);
    ASSERT_TRUE(strcmp(issue->suggestion, " ") == 0);
}

void test_grammar_single_space_is_not_flagged(void)
{
    static const char text[] = "Hello world";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);

    ASSERT_TRUE(find_issue(out, count, "Multiple spaces") == NULL);
}

void test_grammar_space_before_punctuation_includes_the_mark(void)
{
    static const char text[] = "Hello , world";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    const grammar_issue *issue = find_issue(out, count, "Space before punctuation");

    ASSERT_TRUE(issue != NULL);
    ASSERT_EQ_U32(issue->start_offset, 5);
    ASSERT_EQ_U32(issue->span_len, 2); /* " ," */
    ASSERT_TRUE(strcmp(issue->suggestion, ",") == 0);
}

void test_grammar_no_space_before_punctuation_is_not_flagged(void)
{
    static const char text[] = "Hello, world.";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);

    ASSERT_TRUE(find_issue(out, count, "Space before punctuation") == NULL);
}

void test_grammar_repeated_exclamation_flagged(void)
{
    static const char text[] = "Wow!!! amazing";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    const grammar_issue *issue = find_issue(out, count, "Repeated punctuation");

    ASSERT_TRUE(issue != NULL);
    ASSERT_EQ_U32(issue->start_offset, 3);
    ASSERT_EQ_U32(issue->span_len, 3);
    ASSERT_TRUE(strcmp(issue->suggestion, "!") == 0);
}

void test_grammar_ellipsis_is_not_flagged_as_repeated_punctuation(void)
{
    /* Deliberate scope choice (docs/services/grammar.md): '.'/',' are
       excluded from the repeated-punctuation rule specifically so a
       real "..." ellipsis is never a false positive. */
    static const char text[] = "Wait... what?";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);

    ASSERT_TRUE(find_issue(out, count, "Repeated punctuation") == NULL);
}

void test_grammar_missing_initial_capital_flagged(void)
{
    static const char text[] = "this needs a capital.";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);
    const grammar_issue *issue = find_issue(out, count, "Missing capital letter");

    ASSERT_TRUE(issue != NULL);
    ASSERT_EQ_U32(issue->start_offset, 0);
    ASSERT_EQ_U32(issue->span_len, 1);
    ASSERT_TRUE(strcmp(issue->suggestion, "T") == 0);
}

void test_grammar_capitalized_start_is_not_flagged(void)
{
    static const char text[] = "This is fine.";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);

    ASSERT_TRUE(find_issue(out, count, "Missing capital letter") == NULL);
}

void test_grammar_clean_text_reports_no_issues(void)
{
    static const char text[] = "This is a perfectly clean sentence, with no problems at all.";
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 8);

    ASSERT_EQ_U32(count, 0);
}

void test_grammar_empty_text_reports_no_issues(void)
{
    grammar_issue out[8];
    u32 count = grammar_lite_check((const u8 *)"", 0, out, 8);

    ASSERT_EQ_U32(count, 0);
}

void test_grammar_max_issues_cap_is_respected(void)
{
    /* Ten independent doubled-word instances in one string, but a cap
       of 3 -- must never write past out[2], and must return exactly 3,
       not silently overrun the caller's buffer. */
    static const char text[] =
        "cat cat dog dog bird bird fish fish frog frog "
        "wolf wolf lion lion bear bear";
    grammar_issue out[3];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 3);

    ASSERT_EQ_U32(count, 3);
}

void test_grammar_realistic_paragraph_finds_every_planted_issue(void)
{
    /* A single, moderately long paragraph combining every rule at once
       -- the same "don't trust a tiny hand-written fixture alone"
       lesson services/spell's own history documents (docs/services/
       spell.md): a longer, more realistic input is what would have
       caught a scanning loop that terminates early or double-counts. */
    static const char text[] =
        "this paragraph  has a lot of problems . It says the the "
        "same word twice, and it uses spaces   badly, and it gets "
        "way way too excited!!! but at least it ends normally.";
    grammar_issue out[16];
    u32 count = grammar_lite_check((const u8 *)text, (u32)strlen(text), out, 16);

    ASSERT_TRUE(find_issue(out, count, "Missing capital letter") != NULL);
    ASSERT_TRUE(find_issue(out, count, "Multiple spaces") != NULL);
    ASSERT_TRUE(find_issue(out, count, "Space before punctuation") != NULL);
    ASSERT_TRUE(find_issue(out, count, "Repeated punctuation") != NULL);
    {
        u32 i, doubled = 0;
        for (i = 0; i < count; i++) {
            if (strcmp(out[i].message, "Repeated word") == 0) doubled++;
        }
        ASSERT_EQ_U32(doubled, 2); /* "the the" and "way way" */
    }
}
