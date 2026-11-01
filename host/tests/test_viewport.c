/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "viewport.h"
#include "layout.h"
#include "format_txt.h"
#include "editing.h"
#include "app_ctx.h"
#include "keymap.h"
#include "actions.h"

/* The document every test here scrolls: four paragraphs laid out ten
   characters wide, which with the host stub's metrics (a cell is half
   the point size) wraps the two long ones into three and two display
   lines. Seven display lines in all:

       0  aaaa bbbb      paragraph 0
       1  cccc dddd
       2  eeee ffff
       3  (blank)        paragraph 1
       4  gggg hhhh      paragraph 2
       5  iiii jjjj
       6  kkkk           paragraph 3

   The counts are asserted, not assumed, so a change in the wrapper
   fails here rather than silently shifting every expectation below. */
#define VP_TEXT "aaaa bbbb cccc dddd eeee ffff\n\ngggg hhhh iiii jjjj\nkkkk\n"
#define VP_WIDTH_TWIPS 1200

/* A backend's conversion stand-in: 20 twips to the pixel. Every line
   here is 300 twips tall under the host stub (ascent = size, descent =
   size / 4, 12pt), so 15px, and VIEW_LINES * 15 is a view that holds
   exactly VIEW_LINES whole lines. */
#define VP_PX_PER_TWIP_DIV 20
#define VP_VIEW_LINES 3

static i32 twips_to_px(i32 twips)
{
    return twips / VP_PX_PER_TWIP_DIV;
}

static void build_laid_out_doc(wp_document *doc)
{
    wp_section *sec;
    u32 pi;

    ASSERT_TRUE(format_txt_import((const u8 *)VP_TEXT, (u32)strlen(VP_TEXT), doc) == WP_OK);
    sec = wp_document_section_get(doc, 0);
    ASSERT_EQ_U32(sec->para_count, 4);
    for (pi = 0; pi < sec->para_count; pi++) {
        ASSERT_TRUE(wp_paragraph_relayout(doc, wp_section_paragraph_get(sec, pi), VP_WIDTH_TWIPS) == WP_OK);
    }
    ASSERT_EQ_U32(((wp_paragraph_layout *)wp_section_paragraph_get(sec, 0)->layout)->line_count, 3);
    ASSERT_EQ_U32(((wp_paragraph_layout *)wp_section_paragraph_get(sec, 1)->layout)->line_count, 1);
    ASSERT_EQ_U32(((wp_paragraph_layout *)wp_section_paragraph_get(sec, 2)->layout)->line_count, 2);
    ASSERT_EQ_U32(((wp_paragraph_layout *)wp_section_paragraph_get(sec, 3)->layout)->line_count, 1);
}

static wp_pos pos_at(u32 paragraph, u32 offset)
{
    wp_pos p;

    p.section = 0;
    p.paragraph = paragraph;
    p.run = 0;
    p.offset = offset;
    return p;
}

static void viewport_for(wp_viewport *vp)
{
    i32 line_px = twips_to_px(300);

    wp_viewport_init(vp, twips_to_px);
    vp->view_h_px = line_px * VP_VIEW_LINES;
}

static wp_key_event special_key(u16 special)
{
    wp_key_event ev;

    ev.codepoint = 0;
    ev.special = special;
    ev.mods = 0;
    ev._pad = 0;
    return ev;
}

void test_viewport_counts_display_lines_across_paragraphs(void)
{
    wp_document doc;
    wp_pos p;
    u32 total = 0;
    u32 si = 99, pi = 99, li = 99;

    build_laid_out_doc(&doc);

    p = pos_at(0, 0);
    ASSERT_EQ_U32(wp_viewport_cursor_line(&doc, &p, &total), 0);
    ASSERT_EQ_U32(total, 7);

    p = pos_at(0, 12);  /* inside "cccc dddd" */
    ASSERT_EQ_U32(wp_viewport_cursor_line(&doc, &p, NULL), 1);

    p = pos_at(2, 0);
    ASSERT_EQ_U32(wp_viewport_cursor_line(&doc, &p, NULL), 4);

    /* Past the end of paragraph 2, where typing leaves the cursor: the
       paragraph's last line, as the painters place the caret. */
    p = pos_at(2, 19);
    ASSERT_EQ_U32(wp_viewport_cursor_line(&doc, &p, NULL), 5);

    p = pos_at(3, 0);
    ASSERT_EQ_U32(wp_viewport_cursor_line(&doc, &p, NULL), 6);

    ASSERT_TRUE(wp_viewport_line_at(&doc, 5, &si, &pi, &li));
    ASSERT_EQ_U32(si, 0);
    ASSERT_EQ_U32(pi, 2);
    ASSERT_EQ_U32(li, 1);
    ASSERT_TRUE(!wp_viewport_line_at(&doc, 7, &si, &pi, &li));

    wp_document_free(&doc);
}

void test_viewport_follows_the_cursor_with_the_least_scroll(void)
{
    wp_document doc;
    wp_viewport vp;
    wp_pos p;

    build_laid_out_doc(&doc);
    viewport_for(&vp);

    p = pos_at(0, 0);
    ASSERT_TRUE(!wp_viewport_follow_cursor(&vp, &doc, &p));
    ASSERT_EQ_U32(vp.top_line, 0);

    /* Line 5 into a three-line view: lines 3..5, nothing further. */
    p = pos_at(2, 19);
    ASSERT_TRUE(wp_viewport_follow_cursor(&vp, &doc, &p));
    ASSERT_EQ_U32(vp.top_line, 3);

    /* Line 4 is already in view: no movement. */
    p = pos_at(2, 0);
    ASSERT_TRUE(!wp_viewport_follow_cursor(&vp, &doc, &p));
    ASSERT_EQ_U32(vp.top_line, 3);

    /* Back up to line 1: it becomes the top line, not line 0. */
    p = pos_at(0, 12);
    ASSERT_TRUE(wp_viewport_follow_cursor(&vp, &doc, &p));
    ASSERT_EQ_U32(vp.top_line, 1);

    /* The last line. */
    p = pos_at(3, 0);
    ASSERT_TRUE(wp_viewport_follow_cursor(&vp, &doc, &p));
    ASSERT_EQ_U32(vp.top_line, 4);

    wp_document_free(&doc);
}

void test_viewport_follow_keeps_the_top_inside_a_shorter_document(void)
{
    wp_document doc;
    wp_viewport vp;
    wp_pos p;

    build_laid_out_doc(&doc);
    viewport_for(&vp);

    vp.top_line = 40; /* where a longer document had been scrolled to */
    p = pos_at(3, 0);
    ASSERT_TRUE(wp_viewport_follow_cursor(&vp, &doc, &p));
    ASSERT_EQ_U32(vp.top_line, 6);

    wp_document_free(&doc);
}

void test_viewport_page_moves_view_and_cursor_together(void)
{
    wp_document doc;
    wp_viewport vp;
    wp_pos p;

    build_laid_out_doc(&doc);
    viewport_for(&vp);
    p = pos_at(0, 0);

    ASSERT_EQ_U32(wp_viewport_page_lines(&vp, &doc), 3);
    ASSERT_EQ_U32(wp_viewport_max_top(&vp, &doc), 4);

    wp_viewport_page(&vp, &doc, &p, WP_TRUE);
    ASSERT_EQ_U32(vp.top_line, 3);
    ASSERT_EQ_U32(p.paragraph, 1);     /* line 3: the blank paragraph */
    ASSERT_EQ_U32(p.offset, 0);

    /* The second page runs out of document: the view stops where the
       last lines still fill it, the cursor on the last line. */
    wp_viewport_page(&vp, &doc, &p, WP_TRUE);
    ASSERT_EQ_U32(vp.top_line, 4);
    ASSERT_EQ_U32(p.paragraph, 3);

    wp_viewport_page(&vp, &doc, &p, WP_FALSE);
    ASSERT_EQ_U32(vp.top_line, 1);
    ASSERT_EQ_U32(p.paragraph, 1);     /* line 6 - 3 = line 3 */

    wp_viewport_page(&vp, &doc, &p, WP_FALSE);
    ASSERT_EQ_U32(vp.top_line, 0);
    ASSERT_EQ_U32(p.paragraph, 0);
    ASSERT_EQ_U32(p.offset, 0);

    wp_document_free(&doc);
}

void test_viewport_page_lands_mid_paragraph_on_a_wrapped_line(void)
{
    wp_document doc;
    wp_viewport vp;
    wp_pos p;
    wp_section *sec;
    const wp_line *line;

    build_laid_out_doc(&doc);
    viewport_for(&vp);
    vp.view_h_px = twips_to_px(300) * 2;   /* two-line pages */
    p = pos_at(0, 0);

    wp_viewport_page(&vp, &doc, &p, WP_TRUE);
    ASSERT_EQ_U32(vp.top_line, 2);
    ASSERT_EQ_U32(p.paragraph, 0);
    sec = wp_document_section_get(&doc, 0);
    line = &((wp_paragraph_layout *)wp_section_paragraph_get(sec, 0)->layout)->lines[2];
    ASSERT_EQ_U32(p.run, line->start_run);
    ASSERT_EQ_U32(p.offset, line->start_offset);  /* "eeee ffff" */
    ASSERT_TRUE(p.offset > 0);

    wp_document_free(&doc);
}

void test_viewport_scroll_by_hand_holds_until_a_keystroke(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_key_event ev;

    build_laid_out_doc(&doc);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    ctx.doc_loaded = WP_TRUE;
    viewport_for(&ctx.view);

    wp_viewport_scroll_by(&ctx.view, &doc, 2);
    ASSERT_EQ_U32(ctx.view.top_line, 2);
    ASSERT_TRUE(ctx.view.manual);

    /* The caret is on line 0, off the top -- and stays there. */
    ASSERT_TRUE(!wp_viewport_follow_cursor(&ctx.view, &doc, wp_keymap_cursor(&ctx)));
    ASSERT_EQ_U32(ctx.view.top_line, 2);

    /* Past the end clamps to the last full view. */
    wp_viewport_scroll_by(&ctx.view, &doc, 50);
    ASSERT_EQ_U32(ctx.view.top_line, 4);
    wp_viewport_scroll_by(&ctx.view, &doc, -50);
    ASSERT_EQ_U32(ctx.view.top_line, 0);
    wp_viewport_scroll_to(&ctx.view, &doc, 3);
    ASSERT_EQ_U32(ctx.view.top_line, 3);

    /* Any keystroke -- a vi motion here -- releases the hold. */
    ev.codepoint = 'l';
    ev.special = (u16)WP_KEY_NONE;
    ev.mods = 0;
    ev._pad = 0;
    (void)wp_keymap_feed_key(&ctx, ev, 'l');
    ASSERT_TRUE(!ctx.view.manual);
    ASSERT_TRUE(wp_viewport_follow_cursor(&ctx.view, &doc, wp_keymap_cursor(&ctx)));
    ASSERT_EQ_U32(ctx.view.top_line, 0);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_viewport_page_keys_work_under_every_scheme(void)
{
    static const wp_keymap_id schemes[] = {
        WP_KEYMAP_VI, WP_KEYMAP_WORDSTAR, WP_KEYMAP_EMACS, WP_KEYMAP_WORDPERFECT, WP_KEYMAP_MSWORD
    };
    u32 s;

    for (s = 0; s < sizeof(schemes) / sizeof(schemes[0]); s++) {
        wp_document doc;
        wp_undo_journal journal;
        wp_app_ctx ctx;
        wp_keymap_outcome out;

        build_laid_out_doc(&doc);
        wp_undo_journal_init(&journal);
        wp_app_ctx_init(&ctx, schemes[s], &doc, &journal);
        ctx.doc_loaded = WP_TRUE;
        viewport_for(&ctx.view);

        out = wp_keymap_feed_key(&ctx, special_key((u16)WP_KEY_PAGE_DOWN), 0);
        ASSERT_EQ_U32(out.kind, (u32)WP_KEYMAP_RESULT_NONE);
        ASSERT_EQ_U32(ctx.view.top_line, 3);
        ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 1);

        out = wp_keymap_feed_key(&ctx, special_key((u16)WP_KEY_PAGE_UP), 0);
        ASSERT_EQ_U32(out.kind, (u32)WP_KEYMAP_RESULT_NONE);
        ASSERT_EQ_U32(ctx.view.top_line, 0);
        ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 0);

        wp_keymap_free(&ctx);
        wp_undo_journal_free(&journal);
        wp_document_free(&doc);
    }
}

void test_viewport_replace_document_goes_back_to_the_top(void)
{
    wp_document doc, fresh;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    build_laid_out_doc(&doc);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    ctx.doc_loaded = WP_TRUE;
    viewport_for(&ctx.view);
    wp_viewport_scroll_to(&ctx.view, &doc, 4);
    ASSERT_EQ_U32(ctx.view.top_line, 4);

    ASSERT_TRUE(format_txt_import((const u8 *)"new\n", 4, &fresh) == WP_OK);
    wp_action_replace_document(&ctx, &fresh);
    ASSERT_EQ_U32(ctx.view.top_line, 0);
    ASSERT_TRUE(!ctx.view.manual);
    ASSERT_TRUE(ctx.view.line_px == twips_to_px); /* the backend's conversion survives a load */

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* The x-axis stand-in for the hit test: the same 20 twips per pixel. */
static i32 px_to_twips(i32 px)
{
    return px * VP_PX_PER_TWIP_DIV;
}

void test_viewport_finds_the_line_under_y_and_the_position_under_a_click(void)
{
    wp_document doc;
    wp_viewport vp;
    u32 si = 9, pi = 9, li = 9;
    i32 top = -1;
    wp_pos p;
    const wp_line *l1;

    build_laid_out_doc(&doc);
    viewport_for(&vp);   /* 15 px lines */

    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 0, &si, &pi, &li, &top));
    ASSERT_EQ_U32(pi, 0); ASSERT_EQ_U32(li, 0); ASSERT_EQ_U32((u32)top, 0);
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 14, &si, &pi, &li, &top));
    ASSERT_EQ_U32(li, 0);
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 15, &si, &pi, &li, &top));
    ASSERT_EQ_U32(li, 1); ASSERT_EQ_U32((u32)top, 15);
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 40, &si, &pi, &li, &top));
    ASSERT_EQ_U32(pi, 0); ASSERT_EQ_U32(li, 2);
    /* Below everything: the last line. Above: the first. */
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 500, &si, &pi, &li, &top));
    ASSERT_EQ_U32(pi, 3); ASSERT_EQ_U32((u32)top, 90);
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, -10, &si, &pi, &li, &top));
    ASSERT_EQ_U32(pi, 0); ASSERT_EQ_U32(li, 0);

    /* Scrolled: y counts from the top line in view. */
    vp.top_line = 3;
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 0, &si, &pi, &li, &top));
    ASSERT_EQ_U32(pi, 1);
    ASSERT_TRUE(wp_viewport_line_at_y(&vp, &doc, 16, &si, &pi, &li, &top));
    ASSERT_EQ_U32(pi, 2); ASSERT_EQ_U32(li, 0);
    vp.top_line = 0;

    /* A click on "cccc dddd" (line 1 of paragraph 0), 14 px in: cells
       are 6 px wide, so 280 twips is nearest the boundary at 240, two
       characters into the line. */
    l1 = &((wp_paragraph_layout *)wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0)->layout)->lines[1];
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, VP_WIDTH_TWIPS, 14, 20, px_to_twips, twips_to_px, &p));
    ASSERT_EQ_U32(p.paragraph, 0);
    ASSERT_EQ_U32(p.run, 0);
    ASSERT_EQ_U32(p.offset, l1->start_offset + 2);
    /* Far right of a line: its end. Left of it: its start. */
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, VP_WIDTH_TWIPS, 400, 20, px_to_twips, twips_to_px, &p));
    ASSERT_EQ_U32(p.offset, l1->end_offset);
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, VP_WIDTH_TWIPS, -3, 20, px_to_twips, twips_to_px, &p));
    ASSERT_EQ_U32(p.offset, l1->start_offset);
    /* The blank paragraph: its one empty line, start. */
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, VP_WIDTH_TWIPS, 50, 50, px_to_twips, twips_to_px, &p));
    ASSERT_EQ_U32(p.paragraph, 1); ASSERT_EQ_U32(p.offset, 0);

    wp_document_free(&doc);
}

void test_viewport_click_moves_or_extends_under_every_scheme(void)
{
    static const wp_keymap_id schemes[] = {
        WP_KEYMAP_VI, WP_KEYMAP_WORDSTAR, WP_KEYMAP_EMACS, WP_KEYMAP_WORDPERFECT, WP_KEYMAP_MSWORD
    };
    u32 s;

    for (s = 0; s < sizeof(schemes) / sizeof(schemes[0]); s++) {
        wp_document doc;
        wp_undo_journal journal;
        wp_app_ctx ctx;
        wp_selection sel;
        wp_pos a = pos_at(0, 2), b = pos_at(2, 3);

        build_laid_out_doc(&doc);
        wp_undo_journal_init(&journal);
        wp_app_ctx_init(&ctx, schemes[s], &doc, &journal);
        ctx.doc_loaded = WP_TRUE;
        viewport_for(&ctx.view);
        wp_viewport_scroll_by(&ctx.view, &doc, 2);
        ASSERT_TRUE(ctx.view.manual);

        /* A plain click: caret moves, no selection, the view is released. */
        wp_keymap_click(&ctx, a, WP_FALSE);
        ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 2);
        ASSERT_TRUE(!wp_keymap_selection(&ctx, &sel));
        ASSERT_TRUE(!ctx.view.manual);

        /* Shift+click: from where the caret was to here. */
        wp_keymap_click(&ctx, b, WP_TRUE);
        ASSERT_TRUE(wp_keymap_selection(&ctx, &sel));
        ASSERT_EQ_U32(sel.anchor.paragraph, 0); ASSERT_EQ_U32(sel.anchor.offset, 2);
        ASSERT_EQ_U32(sel.caret.paragraph, 2);  ASSERT_EQ_U32(sel.caret.offset, 3);
        ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 2);

        /* Another Shift+click keeps the anchor and moves the far end. */
        wp_keymap_click(&ctx, pos_at(3, 1), WP_TRUE);
        ASSERT_TRUE(wp_keymap_selection(&ctx, &sel));
        ASSERT_EQ_U32(sel.anchor.offset, 2);
        ASSERT_EQ_U32(sel.caret.paragraph, 3);

        /* A plain click ends it. */
        wp_keymap_click(&ctx, a, WP_FALSE);
        ASSERT_TRUE(!wp_keymap_selection(&ctx, &sel));

        wp_keymap_free(&ctx);
        wp_undo_journal_free(&journal);
        wp_document_free(&doc);
    }
}
