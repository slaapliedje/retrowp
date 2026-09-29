/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "spell.h"
#include "mem_host.h"

#define SMALL_DICT "host/tests/fixtures/spell_test.dict"
#define BIG_DICT   "services/spell/data/en_sample.dict"

/* Not itself a test function -- ASSERT_TRUE's `return;` on failure only
   works inside a void-returning test_* function, so this helper can't
   use it internally. A non-WP_OK result here is a real spell.c I/O bug,
   but this project has no ASSERT-from-a-helper convention (see
   test_docmodel.c/test_layout.c: helpers return values, callers
   assert), so callers assert on the wp_status themselves when it
   matters and just treat WP_FALSE as the safe default otherwise. */
static wp_bool check(spell_dict *d, const char *word)
{
    wp_bool known = WP_FALSE;
    spell_check_word(d, (const u8 *)word, (u32)strlen(word), &known);
    return known;
}

void test_spell_dict_open_missing_file_fails(void)
{
    spell_dict *d;
    ASSERT_TRUE(spell_dict_open("host/tests/fixtures/does_not_exist.dict", &d) == WP_ERR);
}

void test_spell_check_exact_known_word(void)
{
    spell_dict *d;
    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(check(d, "cat"));
    ASSERT_TRUE(check(d, "world"));
    spell_dict_close(d);
}

void test_spell_check_unknown_word(void)
{
    spell_dict *d;
    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(!check(d, "xyzzyqux"));
    spell_dict_close(d);
}

void test_spell_check_case_insensitive(void)
{
    spell_dict *d;
    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(check(d, "Cat"));
    ASSERT_TRUE(check(d, "CAT"));
    ASSERT_TRUE(check(d, "WoRLD"));
    spell_dict_close(d);
}

void test_spell_check_affix_rules(void)
{
    spell_dict *d;
    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);

    ASSERT_TRUE(check(d, "cats"));      /* strip "s" -> cat */
    ASSERT_TRUE(check(d, "boxes"));     /* strip "es" -> box */
    ASSERT_TRUE(check(d, "flies"));     /* strip "ies", add "y" -> fly */
    ASSERT_TRUE(check(d, "walked"));    /* strip "ed" -> walk */
    ASSERT_TRUE(check(d, "baked"));     /* strip "ed", add "e" -> bake */
    ASSERT_TRUE(check(d, "walking"));   /* strip "ing" -> walk */
    ASSERT_TRUE(check(d, "baking"));    /* strip "ing", add "e" -> bake */
    ASSERT_TRUE(check(d, "quickly"));   /* strip "ly" -> quick */
    ASSERT_TRUE(check(d, "faster"));    /* strip "er" -> fast */
    ASSERT_TRUE(check(d, "fastest"));   /* strip "est" -> fast */

    /* Documented gap: no consonant-doubling undo, so "running" does NOT
       reduce to "run" via these rules (would need "n" + "ing" -> "run",
       i.e. undoing the doubled "n" -- out of scope, docs/services/spell.md). */
    ASSERT_TRUE(!check(d, "running"));

    spell_dict_close(d);
}

void test_spell_check_non_ascii_fails_open(void)
{
    spell_dict *d;
    static const u8 cafe[] = { 'c', 'a', 'f', 0xC3, 0xA9, '\0' }; /* "café" */

    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(check(d, (const char *)cafe));
    spell_dict_close(d);
}

void test_spell_check_no_leaks(void)
{
    spell_dict *d;

    mem_host_reset_counters();
    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(check(d, "cat"));
    spell_dict_close(d);
    ASSERT_EQ_U32(mem_host_live_count(), 0);
}

void test_spell_suggest_finds_transposition(void)
{
    spell_dict *d;
    char out[5][SPELL_MAX_WORD_LEN];
    u32 count = 0;
    u32 i;
    wp_bool found_world = WP_FALSE;
    static const char *typo = "wrold"; /* transposed "or" -- world */

    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(spell_suggest(d, (const u8 *)typo, (u32)strlen(typo), out, 5, &count) == WP_OK);

    for (i = 0; i < count; i++) {
        if (strcmp(out[i], "world") == 0) found_world = WP_TRUE;
    }
    ASSERT_TRUE(found_world);

    spell_dict_close(d);
}

void test_spell_suggest_respects_max(void)
{
    spell_dict *d;
    char out[1][SPELL_MAX_WORD_LEN];
    u32 count = 0;
    static const char *typo = "cbt"; /* substitution distance 1 from "cat" */

    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(spell_suggest(d, (const u8 *)typo, (u32)strlen(typo), out, 1, &count) == WP_OK);
    ASSERT_TRUE(count <= 1);

    spell_dict_close(d);
}

void test_spell_suggest_non_ascii_returns_nothing(void)
{
    spell_dict *d;
    char out[5][SPELL_MAX_WORD_LEN];
    u32 count = 1; /* deliberately non-zero, to prove it gets reset to 0 */
    static const u8 word[] = { 0xC3, 0xA9, '\0' };

    ASSERT_TRUE(spell_dict_open(SMALL_DICT, &d) == WP_OK);
    ASSERT_TRUE(spell_suggest(d, word, 2, out, 5, &count) == WP_OK);
    ASSERT_EQ_U32(count, 0);

    spell_dict_close(d);
}

/* Realistic scale: the 6000-word fixture derived from a real en_US
   dictionary (docs/services/spell.md) -- proves binary search works
   correctly well beyond the ~11-word hand-authored fixture above,
   including the file's first and last lines (the binary search
   algorithm's boundary conditions). */
void test_spell_realistic_dictionary_sample(void)
{
    spell_dict *d;

    ASSERT_TRUE(spell_dict_open(BIG_DICT, &d) == WP_OK);

    ASSERT_TRUE(check(d, "aa"));       /* first line of the file */
    ASSERT_TRUE(check(d, "zworykin")); /* last line of the file */
    ASSERT_TRUE(check(d, "abode"));
    ASSERT_TRUE(check(d, "laughs"));
    ASSERT_TRUE(check(d, "leaflet"));
    ASSERT_TRUE(!check(d, "zzznotarealword"));
    ASSERT_TRUE(!check(d, "aaaaaaaaaaaaaanotarealword"));

    spell_dict_close(d);
}
