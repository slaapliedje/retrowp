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

/* ---- wp_line_cursor_x_twips (T157) -------------------------------- */

/* Same fixed host metrics as above: 120 twips per character at the
   default cstyle, so every expected value below is (character count from
   the LINE's own start) * 120 -- hand-computed, not captured from a
   previous run. */

void test_justify_cursor_x_measures_from_line_start_not_paragraph_start(void)
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

    /* THE regression this whole function exists for. Byte offset 13 is
       "brown fox"'s 'w' -- 13 characters into the PARAGRAPH but only 3
       into line 1, which starts at offset 10. A caret positioned from
       the paragraph offset would land 10 characters too far right; the
       old Mac code landed it at the margin regardless. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[1], 0, 13, 0), 360);

    /* Line 1's own start and end. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[1], 0, 10, 0), 0);
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[1], 0, 19, 0), 1080);

    /* And line 0, to prove the offset-13 answer above isn't an accident
       of measuring the whole paragraph and happening to match. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[0], 0, 4, 0), 480);

    wp_document_free(&doc);
}

void test_justify_cursor_x_past_line_end_clamps_to_line_width(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    static const char *text = "The quick brown fox jumps\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1100) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;

    /* An offset beyond this line entirely (it belongs to line 2) walks
       every fragment and stops -- the caret parks at end-of-content
       rather than running off into whatever the next line measures.
       This is also the ordinary typing case: the cursor sits one past
       the last character it just inserted. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[1], 0, 25, 0), 1080);

    wp_document_free(&doc);
}

void test_justify_cursor_x_accumulates_justify_slack(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    i32 extra;
    static const char *text = "The quick brown fox jumps\n";

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1100) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;

    /* Line 1 "brown fox" stretched to a 1300-twip column: one gap, 220
       twips of slack (test_justify_computes_extra_per_gap's own math). */
    extra = wp_justify_space_extra_twips(&doc, para, &layout->lines[1],
                                          WP_ALIGN_JUSTIFY, WP_FALSE, 1300);
    ASSERT_EQ_U32(extra, 220);

    /* Offset 16 is 'f' -- past the stretched gap, so the caret must
       include the slack the paint loop also added there:
       "brown" 600 + " " 120 + 220 = 940. Passing 0 for a justified line
       would put it 220 twips left of the glyph it points at. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[1], 0, 16, extra), 940);

    /* Before the gap, the slack must NOT be counted yet. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[1], 0, 15, extra), 600);

    wp_document_free(&doc);
}

void test_justify_cursor_x_crosses_run_boundaries(void)
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

    /* Same "wor" + "d" + " there" three-run shape the fragment-iterator
       test above uses -- a cursor offset is per-RUN, so the walk has to
       carry accumulated width across run boundaries, not restart. */
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"wor", 3, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"d", 1, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)" there", 6, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1200) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);

    /* run 1 offset 0 == run 0 offset 3: both mean "after wor". */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[0], 0, 3, 0), 360);
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[0], 1, 0, 0), 360);

    /* run 2 offset 3 == "wor" + "d" + " " + "th" = 7 chars. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[0], 2, 3, 0), 840);

    /* End of the last run -- the whole line, 10 characters. */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[0], 2, 6, 0), 1200);

    wp_document_free(&doc);
}

void test_justify_cursor_x_in_skipped_zero_length_run(void)
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

    /* An edit can leave a zero-length run behind (docs/editing.md), and
       the cursor can be parked in it. wp_line_next_fragment skips such a
       run entirely -- so a walk that only ever stops on a MATCHING
       fragment would never stop, and the caret would jump to the end of
       the line. */
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"ab", 2, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"", 0, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"cd", 2, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1200) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);

    /* After "ab", not after "abcd". */
    ASSERT_EQ_U32(wp_line_cursor_x_twips(&doc, para, &layout->lines[0], 1, 0, 0), 240);

    wp_document_free(&doc);
}
