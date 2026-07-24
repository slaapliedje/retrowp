#include <string.h>
#include "test.h"
#include "layout.h"
#include "editing.h"
#include "mem_host.h"

/* M15 Phase 2: the wp_paragraph_relayout early-branch for WP_PARA_IMAGE,
   the justify.c kind-guards, and the paginate.c-needs-zero-changes
   regression proof. New file rather than additions to test_layout.c/
   test_justify.c/test_paginate.c so no pre-existing test file needs to
   change (same discipline test_image.c's own Phase 1 file already
   established). */

static void fill_test_desc(wp_image_desc *desc, i32 width_px, i32 height_px)
{
    memset(desc, 0, sizeof(*desc));
    desc->width_px = width_px;
    desc->height_px = height_px;
    desc->display_width_twips = width_px * 15;
    desc->display_height_twips = height_px * 15;
    desc->palette_count = 2;
}

static void build_image_only_doc(wp_document *doc, const wp_image_desc *desc,
                                  wp_paragraph **out_para)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_image handle;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    *out_para = wp_section_paragraph_get(sec, para_idx);

    ASSERT_TRUE(wp_document_image_add(doc, desc, &handle) == WP_OK);
    (*out_para)->kind = (u8)WP_PARA_IMAGE;
    (*out_para)->image = handle;
    (*out_para)->style = doc->default_pstyle;
}

void test_image_layout_relayout_produces_one_line_from_descriptor(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_image_desc desc;
    wp_paragraph_layout *layout;

    fill_test_desc(&desc, 80, 40); /* -> 1200 x 600 twips */
    build_image_only_doc(&doc, &desc, &para);

    /* content_width_twips is intentionally narrower than the image's own
       display width -- must NOT wrap/clip/fail; the image line's own
       width comes only from the descriptor, never from the available
       column width (no text-wrap-around-image in this milestone). */
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 100) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_TRUE(layout != NULL);
    ASSERT_EQ_U32(layout->line_count, 1);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 1200);
    ASSERT_EQ_U32(layout->lines[0].height_twips, 600);
    ASSERT_EQ_U32(layout->lines[0].start_run, 0);
    ASSERT_EQ_U32(layout->lines[0].end_run, 0);
    ASSERT_EQ_U32(layout->lines[0].start_offset, 0);
    ASSERT_EQ_U32(layout->lines[0].end_offset, 0);
    ASSERT_EQ_U32(para->layout_dirty, WP_FALSE);

    wp_document_free(&doc);
}

void test_image_layout_missing_handle_degrades_to_zero_size(void)
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

    /* kind == WP_PARA_IMAGE but image == 0 (the reserved sentinel,
       never registered) -- must degrade to a zero-size line, not crash
       or fail relayout outright. */
    para->kind = (u8)WP_PARA_IMAGE;
    para->image = 0;

    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1000) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;
    ASSERT_EQ_U32(layout->line_count, 1);
    ASSERT_EQ_U32(layout->lines[0].width_twips, 0);
    ASSERT_EQ_U32(layout->lines[0].height_twips, 0);

    wp_document_free(&doc);
}

void test_image_layout_fragment_iter_returns_false_immediately(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_image_desc desc;
    wp_paragraph_layout *layout;
    wp_line_fragment_iter iter;
    wp_line_fragment frag;

    fill_test_desc(&desc, 80, 40);
    build_image_only_doc(&doc, &desc, &para);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1000) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;

    wp_line_fragment_iter_init(&iter, &layout->lines[0]);
    ASSERT_TRUE(!wp_line_next_fragment(&doc, para, &layout->lines[0], &iter, &frag));

    wp_document_free(&doc);
}

void test_image_layout_justify_extra_is_zero(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_image_desc desc;
    wp_paragraph_layout *layout;

    fill_test_desc(&desc, 80, 40);
    build_image_only_doc(&doc, &desc, &para);
    ASSERT_TRUE(wp_paragraph_relayout(&doc, para, 1000) == WP_OK);
    layout = (wp_paragraph_layout *)para->layout;

    /* Even asked to justify a not-last line against a much wider target,
       an image line has nothing to stretch -- must return 0, not divide
       by a gap count derived from nonexistent run bytes. */
    ASSERT_EQ_U32(wp_justify_space_extra_twips(&doc, para, &layout->lines[0],
                                                WP_ALIGN_JUSTIFY, WP_FALSE, 5000), 0);

    wp_document_free(&doc);
}

/* The T14.6-style regression proof this phase's plan called for: a
   document mixing text and image paragraphs paginates correctly using
   ONLY each line's height_twips -- paginate.c itself needed zero
   changes, confirmed here rather than just asserted. Built via the real
   wp_edit_insert_image (Phase 1) rather than hand-assembled, so this
   exercises the exact paragraph shape a real caller produces. */
void test_image_layout_paginate_mixed_text_and_image(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_section *sec;
    wp_span span;
    u16 run_idx;
    u32 sec_idx, para_idx;
    wp_paragraph *para;
    wp_image_desc desc;
    wp_pos pos;
    wp_pagination pg;

    mem_host_reset_counters();

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    /* Text height at the default cstyle (host stub metrics) is 300
       twips/line (see test_layout.c's own header comment); pick an
       image height comfortably different (900) so a height-only bug
       (e.g. accidentally reusing the text line height) would show up
       as a wrong page split below, not silently pass by coincidence. */
    fill_test_desc(&desc, 100, 60); /* -> 1500 x 900 twips */

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_edit_insert_image(&doc, &journal, pos, &desc) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 3); /* "Hello" / image / " World" */

    /* Page fits exactly the first two paragraphs' lines (300 + 900 =
       1200) and no more -- proves paginate.c is correctly summing a
       mix of text-derived and image-derived height_twips values with
       no special-casing of its own. */
    sec->page_width = 10000; sec->margin_left = 0; sec->margin_right = 0;
    sec->page_height = 1200; sec->margin_top = 0; sec->margin_bottom = 0;

    ASSERT_TRUE(wp_document_paginate(&doc, &pg) == WP_OK);

    ASSERT_EQ_U32(pg.page_count, 2);
    ASSERT_EQ_U32(pg.pages[0].line_count, 2);
    ASSERT_EQ_U32(pg.pages[0].lines[0].paragraph, 0); /* "Hello" */
    ASSERT_EQ_U32(pg.pages[0].lines[1].paragraph, 1); /* image */
    ASSERT_EQ_U32(pg.pages[1].line_count, 1);
    ASSERT_EQ_U32(pg.pages[1].lines[0].paragraph, 2); /* " World" */

    wp_pagination_free(&pg);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
