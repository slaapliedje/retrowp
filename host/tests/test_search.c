#include <string.h>
#include "test.h"
#include "editing.h"
#include "mem_host.h"

/* wp_document_find_text / wp_document_match_selection (M11) -- the
   shared Find/Replace primitive promoted from vi-mode's own
   wp_vimode_search. Fixture-building mirrors test_selection.c's own
   build_two_para_doc pattern. */

static void build_two_para_doc(wp_document *doc, const char *p0_text, const char *p1_text)
{
    u32 sec_idx, p0, p1;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &p0) == WP_OK);
    para = wp_section_paragraph_get(sec, p0);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)p0_text, (u32)strlen(p0_text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &p1) == WP_OK);
    para = wp_section_paragraph_get(sec, p1);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)p1_text, (u32)strlen(p1_text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
}

/* "Hello World" as TWO runs ("Hello " + "World") in one paragraph --
   proves a match spanning a run boundary is found and its selection
   correctly crosses that boundary. */
static void build_two_run_doc(wp_document *doc)
{
    u32 sec_idx, p0;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &p0) == WP_OK);
    para = wp_section_paragraph_get(sec, p0);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"Hello ", 6, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"World", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
}

static wp_pos pos_at(u32 section, u32 paragraph, u32 run, u32 offset)
{
    wp_pos p;
    p.section = section; p.paragraph = paragraph; p.run = run; p.offset = offset;
    return p;
}

void test_search_find_forward_basic(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "Goodbye Moon");

    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"Moon", 4,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 1);
    ASSERT_EQ_U32(found.offset, 8);

    wp_document_free(&doc);
}

void test_search_find_forward_not_found(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "Goodbye Moon");

    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"xyz", 3,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_ERR);

    wp_document_free(&doc);
}

void test_search_find_forward_wraps_around_document(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "Goodbye Moon");

    /* starting search from inside paragraph 1 (past "Hello") must wrap
       back around to paragraph 0's own "Hello" */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 1, 0, 0), (const u8 *)"Hello", 5,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 0);
    ASSERT_EQ_U32(found.offset, 0);

    wp_document_free(&doc);
}

void test_search_find_backward_basic(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "Goodbye Moon");

    /* searching backward from the doc end finds "World" behind the cursor */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 1, 0, 12), (const u8 *)"World", 5,
                                       WP_FALSE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 0);
    ASSERT_EQ_U32(found.offset, 6);

    wp_document_free(&doc);
}

void test_search_find_backward_wraps_around_document(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "Goodbye Moon");

    /* searching backward from paragraph 0 for "Moon" (only in paragraph 1)
       must wrap around to the end of the document */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"Moon", 4,
                                       WP_FALSE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 1);
    ASSERT_EQ_U32(found.offset, 8);

    wp_document_free(&doc);
}

void test_search_find_backward_picks_nearest_preceding_match(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "aXaXaX", "unused");

    /* three 'a' occurrences at offsets 0, 2, 4 -- searching backward from
       offset 5 must land on the NEAREST one (offset 4), not the first */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 5), (const u8 *)"a", 1,
                                       WP_FALSE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 0);
    ASSERT_EQ_U32(found.offset, 4);

    wp_document_free(&doc);
}

void test_search_ignore_case_matches_different_case(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "unused");

    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"WORLD", 5,
                                       WP_TRUE, WP_TRUE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.offset, 6);

    /* without ignore_case, the same needle does not match */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"WORLD", 5,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_ERR);

    wp_document_free(&doc);
}

void test_search_whole_word_rejects_substring_match(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "cat catalog concatenate", "unused");

    /* "cat" appears as a substring of "catalog"/"concatenate" too --
       whole_word must land only on the standalone word at offset 0.
       Search starts from paragraph 1 (search is strictly-after-`from`,
       so a match sitting at the very start of the document needs the
       wraparound pass to reach it -- see wp_document_find_text's own
       doc comment) and wraps forward into paragraph 0. */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 1, 0, 0), (const u8 *)"cat", 3,
                                       WP_TRUE, WP_FALSE, WP_TRUE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 0);
    ASSERT_EQ_U32(found.offset, 0);

    /* searching again starting just past that match finds no further
       whole-word "cat" (both remaining occurrences are substrings) */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 1), (const u8 *)"cat", 3,
                                       WP_TRUE, WP_FALSE, WP_TRUE, &found) == WP_ERR);

    wp_document_free(&doc);
}

void test_search_whole_word_false_matches_substring(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "catalog", "unused");

    /* same strictly-after-`from` reasoning as the whole_word test above --
       start from paragraph 1 so the wraparound pass reaches offset 0. */
    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 1, 0, 0), (const u8 *)"cat", 3,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.paragraph, 0);
    ASSERT_EQ_U32(found.offset, 0);

    wp_document_free(&doc);
}

void test_search_match_crosses_run_boundary(void)
{
    wp_document doc;
    wp_pos found;
    wp_selection sel;

    build_two_run_doc(&doc); /* runs: "Hello " (6) + "World" (5) */

    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"lo Wo", 5,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_OK);
    ASSERT_EQ_U32(found.run, 0);
    ASSERT_EQ_U32(found.offset, 3);

    sel = wp_document_match_selection(&doc, found, 5);
    ASSERT_EQ_U32(sel.caret.run, 1);
    ASSERT_EQ_U32(sel.caret.offset, 2); /* "lo Wo" ends 2 bytes into the "World" run */

    wp_document_free(&doc);
}

void test_search_match_selection_within_single_run(void)
{
    wp_document doc;
    wp_selection sel;
    wp_pos start;
    u8 buf[16];
    u32 len;

    build_two_para_doc(&doc, "Hello World", "unused");
    start = pos_at(0, 0, 0, 6);

    sel = wp_document_match_selection(&doc, start, 5);
    ASSERT_TRUE(wp_pos_compare(sel.anchor, start) == 0);
    ASSERT_EQ_U32(sel.caret.offset, 11);

    ASSERT_TRUE(wp_document_extract_text(&doc, sel, buf, sizeof(buf), &len) == WP_OK);
    buf[len] = '\0';
    ASSERT_EQ_STR((const char *)buf, "World");

    wp_document_free(&doc);
}

void test_search_empty_needle_is_error(void)
{
    wp_document doc;
    wp_pos found;

    build_two_para_doc(&doc, "Hello World", "Goodbye Moon");

    ASSERT_TRUE(wp_document_find_text(&doc, pos_at(0, 0, 0, 0), (const u8 *)"", 0,
                                       WP_TRUE, WP_FALSE, WP_FALSE, &found) == WP_ERR);

    wp_document_free(&doc);
}
