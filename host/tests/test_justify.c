#include <string.h>
#include "test.h"
#include "layout.h"
#include "format_txt.h"

/* Shares metrics_host's fixed metrics with test_layout.c: cell_width =
   size_twips/2 = 120 twips/char at the default cstyle (size_twips=240).
   "The"=360 "quick"=600 "brown"=600 "fox"=360 "jumps"=600, spaces=120. */

void test_justify_non_justify_align_returns_zero(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "The quick\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 2000) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;

    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[0],
                                                WP_ALIGN_LEFT, WP_FALSE, 2000), 0);

    wp_document_free(&doc);
}

void test_justify_last_line_of_paragraph_returns_zero(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "The quick brown fox jumps\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1100) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 3); /* "The quick" / "brown fox" / "jumps" */

    /* Last line ("jumps", no interior gap anyway) must stay 0 even if
       asked to justify -- the caller-supplied is_last_line flag alone
       decides this, independent of gap_count. */
    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[2],
                                                WP_ALIGN_JUSTIFY, WP_TRUE, 5000), 0);

    wp_document_free(&doc);
}

void test_justify_computes_extra_per_gap(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "The quick brown fox jumps\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1100) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;

    /* Line 0 "The quick" (width 1080, one interior gap) stretched to
       fill a 1300-twip column: (1300 - 1080) / 1 gap = 220. */
    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[0],
                                                WP_ALIGN_JUSTIFY, WP_FALSE, 1300), 220);

    /* Line 1 "brown fox" (width 1080, one interior gap) to 1080 exactly
       -- already fits, zero extra needed. */
    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[1],
                                                WP_ALIGN_JUSTIFY, WP_FALSE, 1080), 0);

    wp_document_free(&doc);
}

void test_justify_single_word_line_has_no_gap(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "supercalifragilistic\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 5000) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);

    /* No interior space at all -- must not divide by zero, must return 0. */
    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[0],
                                                WP_ALIGN_JUSTIFY, WP_FALSE, 5000), 0);

    wp_document_free(&doc);
}

/* A word split across a run boundary (as insert/delete naturally
   produce) must still yield one fragment PER RUN, not one merged
   fragment across the boundary -- unlike linebreak.c's tokenize(),
   which deliberately merges same-class fragments across runs so a word
   is never wrap-broken mid-token. Justify's fragment walk needs the
   opposite: per-run granularity, so a backend can switch font at each
   style change while still treating the interior space as a single
   gap. */
void test_justify_fragment_iter_splits_at_run_boundary_not_at_word(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_paragraph_layout *layout;
    wp_line_fragment_iter iter;
    wp_line_fragment frag;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    /* "wor" + "d" + " there" -- "word" spans runs 0 and 1, one space
       gap before run 2's "there". */
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"wor", 3, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"d", 1, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)" there", 6, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1200) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);

    wp_line_fragment_iter_init(&iter, &layout->lines[0]);

    ASSERT_TRUE(wp_line_next_fragment(&doc, para, &layout->lines[0], &iter, &frag));
    ASSERT_EQ_U32(frag.run_index, 0);
    ASSERT_EQ_U32(frag.byte_start, 0); ASSERT_EQ_U32(frag.byte_end, 3); /* "wor" */
    ASSERT_TRUE(!frag.is_space);

    ASSERT_TRUE(wp_line_next_fragment(&doc, para, &layout->lines[0], &iter, &frag));
    ASSERT_EQ_U32(frag.run_index, 1);
    ASSERT_EQ_U32(frag.byte_start, 0); ASSERT_EQ_U32(frag.byte_end, 1); /* "d" */
    ASSERT_TRUE(!frag.is_space);

    ASSERT_TRUE(wp_line_next_fragment(&doc, para, &layout->lines[0], &iter, &frag));
    ASSERT_EQ_U32(frag.run_index, 2);
    ASSERT_EQ_U32(frag.byte_start, 0); ASSERT_EQ_U32(frag.byte_end, 1); /* " " */
    ASSERT_TRUE(frag.is_space);

    ASSERT_TRUE(wp_line_next_fragment(&doc, para, &layout->lines[0], &iter, &frag));
    ASSERT_EQ_U32(frag.run_index, 2);
    ASSERT_EQ_U32(frag.byte_start, 1); ASSERT_EQ_U32(frag.byte_end, 6); /* "there" */
    ASSERT_TRUE(!frag.is_space);

    ASSERT_TRUE(!wp_line_next_fragment(&doc, para, &layout->lines[0], &iter, &frag));

    /* Exactly one gap -- the run-boundary split between "wor" and "d"
       must NOT be counted as a justify gap, only the real space is. */
    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[0],
                                                WP_ALIGN_JUSTIFY, WP_FALSE, 1300), 100);

    wp_document_free(&doc);
}
