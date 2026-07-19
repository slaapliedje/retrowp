#include <string.h>
#include "test.h"
#include "layout.h"
#include "format_txt.h"
#include "editing.h"
#include "metrics_host.h"

/* Host stub metrics (platform/host/metrics_host.c): cell_width =
   size_twips/2, ascent = size_twips, descent = size_twips/4, leading =
   0. At the default cstyle's size_twips = 240: cell_width = 120
   twips/char, height = 240 + 60 + 0 = 300. Every width/height assertion
   below is hand-computed from those two constants (T3.1 AC). */

void test_layout_greedy_wrap_hand_computed_fixture(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "The quick brown fox jumps\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);

    /* "The"=360 "quick"=600 "brown"=600 "fox"=360 "jumps"=600, spaces=120 each.
       At width 1100: "The quick"=360+120+600=1080 fits; +"brown" would be
       1080+120+600=1800, doesn't -- matches the hand trace in docs/layout.md's
       greedy algorithm description. */
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1100) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_TRUE(layout != NULL);
    ASSERT_EQ_U32(layout->line_count, 3);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 1080); /* "The quick" */
    ASSERT_EQ_U32(layout->lines[1].width_twips, 1080); /* "brown fox" */
    ASSERT_EQ_U32(layout->lines[2].width_twips, 600);  /* "jumps" */
    ASSERT_EQ_U32(layout->lines[0].height_twips, 300);

    /* Narrow enough that no two words fit together -- one word per line. */
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1000) == WP_OK);
    ASSERT_EQ_U32(layout->line_count, 5);

    wp_document_free(&doc);
}

void test_layout_single_word_wider_than_column_overflows(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "supercalifragilistic\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);

    /* 20 chars * 120 = 2400 twips, but column is only 500 -- must still
       place it as one line (overflowing), never split mid-word. */
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 500) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 2400);

    wp_document_free(&doc);
}

void test_layout_empty_paragraph_gets_one_zero_width_line(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_paragraph_layout *layout;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1000) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 0);
    ASSERT_EQ_U32(layout->lines[0].height_twips, 300); /* doc->default_cstyle */

    wp_document_free(&doc);
}

/* A word deliberately split across two runs (as insert/delete naturally
   produce -- docs/editing.md) must be treated as ONE token: measured by
   summing each run-fragment's width, and never broken mid-word. */
void test_layout_word_spanning_run_boundary_is_one_token(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_paragraph_layout *layout;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    /* "wor" + "d" + " there" -- "word" spans runs 0 and 1. */
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"wor", 3, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"d", 1, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)" there", 6, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    /* "word"=480, " "=120, "there"=600. Wide enough for all of it: 1 line. */
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1200) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 1200);

    /* Narrow enough that "word" (480) fits alone but "there" (600, after
       the elided space) must wrap to a new line -- proves "word" itself
       was never split even though it crosses a run boundary mid-token. */
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 500) == WP_OK);
    ASSERT_EQ_U32(layout->line_count, 2);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 480);
    ASSERT_EQ_U32(layout->lines[0].end_run, 1); /* "word" ends inside run 1 ("d") */
    ASSERT_EQ_U32(layout->lines[1].width_twips, 600);

    wp_document_free(&doc);
}

/* T3.3 AC: editing paragraph K relayouts K (and following only if
   reflow needed). Proven quantitatively via metrics_host's measure-call
   counter (docs/layout.md), not just by trusting the code path:
   repaginating after editing ONE paragraph out of five must cost far
   fewer plat_measure_text calls than a full from-scratch pagination of
   the same document. */
void test_layout_editing_one_paragraph_does_not_remeasure_whole_document(void)
{
    static const char *text =
        "alpha beta gamma delta\n"
        "epsilon zeta eta theta\n"
        "iota kappa lambda mu\n"
        "nu xi omicron pi\n"
        "rho sigma tau upsilon\n";
    wp_document full_doc;
    wp_pagination full_pg;
    u32 full_count;

    wp_document incr_doc;
    wp_pagination incr_pg;
    u32 incr_count;
    wp_section *sec;
    wp_paragraph *para;
    wp_undo_journal journal;
    wp_pos pos, out_pos;

    /* Baseline: full from-scratch pagination of the whole document. */
    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &full_doc) == WP_OK);
    sec = wp_document_section_get(&full_doc, 0);
    sec->page_width = 100000; sec->margin_left = 0; sec->margin_right = 0;
    sec->page_height = 100000; sec->margin_top = 0; sec->margin_bottom = 0;

    metrics_host_reset_measure_count();
    ASSERT_TRUE(wp_document_paginate(&full_doc, &full_pg) == WP_OK);
    full_count = metrics_host_measure_call_count();
    ASSERT_TRUE(full_count > 0);

    wp_pagination_free(&full_pg);
    wp_document_free(&full_doc);

    /* Incremental: paginate once to lay out everything and clear all
       dirty flags, edit ONE paragraph, then paginate again. */
    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &incr_doc) == WP_OK);
    sec = wp_document_section_get(&incr_doc, 0);
    sec->page_width = 100000; sec->margin_left = 0; sec->margin_right = 0;
    sec->page_height = 100000; sec->margin_top = 0; sec->margin_bottom = 0;

    ASSERT_TRUE(wp_document_paginate(&incr_doc, &incr_pg) == WP_OK);
    wp_pagination_free(&incr_pg);

    wp_undo_journal_init(&journal);
    para = wp_section_paragraph_get(sec, 2); /* "iota kappa lambda mu" */
    pos.section = 0; pos.paragraph = 2; pos.run = 0; pos.offset = 0;
    ASSERT_TRUE(wp_edit_insert_text(&incr_doc, &journal, pos, (const u8 *)"X", 1, &out_pos) == WP_OK);
    ASSERT_TRUE(para->layout_dirty); /* only paragraph 2 is dirty */

    metrics_host_reset_measure_count();
    ASSERT_TRUE(wp_document_paginate(&incr_doc, &incr_pg) == WP_OK);
    incr_count = metrics_host_measure_call_count();

    /* 5 roughly-equal paragraphs; editing 1 should cost roughly 1/5 of
       a full pass, comfortably under half even with rounding/overhead. */
    ASSERT_TRUE(incr_count > 0);
    ASSERT_TRUE(incr_count < full_count / 2);

    wp_undo_journal_free(&journal);
    wp_pagination_free(&incr_pg);
    wp_document_free(&incr_doc);
}
