/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "pagecanvas.h"
#include "pageview.h"
#include "format_txt.h"
#include "notes.h"
#include "editing.h"
#include "layout.h"
#include "draw_host.h"
#include "paper.h"

/* Page view (pagecanvas.h), with a 96-dpi screen: 15 twips a pixel. */

static i32 px15(i32 t) { return t / 15; }
static i32 tw15(i32 p) { return p * 15; }

static void doc_of_lines(wp_document *doc, u32 lines)
{
    static u8 src[20000];
    u32 n = 0, k;
    for (k = 0; k < lines && n + 30 < sizeof(src); k++) {
        memcpy(src + n, "Line of text here for pages.\n", 29);
        n += 29;
    }
    ASSERT_TRUE(format_txt_import(src, n, doc) == WP_OK);
    (void)wp_pageview_relayout(doc, 0);
}

/* Fit width: the page fills the window less the gap each side; pages
   stack down the canvas with the gap between. */
void test_pagecanvas_fit_and_stack(void)
{
    wp_document doc;
    wp_pagecanvas c;
    i32 x, y, w, h, x1, y1, w1, h1;

    doc_of_lines(&doc, 150);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    ASSERT_TRUE(c.page_count >= 2);
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 0, &x, &y, &w, &h));
    /* a whole percent: up to one percent of the width spare */
    ASSERT_TRUE(w <= 640 - 2 * WP_PAGECANVAS_GAP_PX && w >= ((640 - 2 * WP_PAGECANVAS_GAP_PX) * 99) / 100 - 1);
    ASSERT_EQ_U32((u32)y, WP_PAGECANVAS_GAP_PX);
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 1, &x1, &y1, &w1, &h1));
    ASSERT_EQ_U32((u32)y1, (u32)(y + h + WP_PAGECANVAS_GAP_PX));
    ASSERT_TRUE(!wp_pagecanvas_page_rect(&c, c.page_count, &x1, &y1, &w1, &h1));
    /* Fit page: a whole page in the window's height. */
    c.fit = WP_ZOOM_FIT_PAGE;
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 0, &x, &y, &w, &h));
    ASSERT_TRUE(h <= 400 - 2 * WP_PAGECANVAS_GAP_PX);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

/* Painted, the first word sits at the page's corner plus its margins,
   at the zoom; the caret there agrees, and a click there comes back as
   the same place. */
void test_pagecanvas_paint_caret_hit(void)
{
    wp_document doc;
    wp_pagecanvas c;
    i32 x, y, w, h, tx, ty, cx, cy, ch;
    const char *t;
    wp_pos pos, back;
    const wp_section *sec;

    doc_of_lines(&doc, 3);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    sec = &doc.sections[0];
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 0, &x, &y, &w, &h));
    draw_host_reset_text();
    wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, 0, 640, 400);   /* the host draws nowhere */
    ASSERT_TRUE(draw_host_text_count() > 0);
    t = draw_host_text_at(0, &tx, &ty);
    ASSERT_TRUE(t != NULL && strncmp(t, "Line", 4) == 0);
    ASSERT_TRUE(tx >= x + wp_pagecanvas_zx(&c, sec->margin_left) - 1 && tx <= x + wp_pagecanvas_zx(&c, sec->margin_left) + 1);
    ASSERT_TRUE(ty > y + wp_pagecanvas_zy(&c, sec->margin_top) && ty < y + wp_pagecanvas_zy(&c, sec->margin_top + 600));

    memset(&pos, 0, sizeof(pos));
    pos.paragraph = 1;
    pos.offset = 5;
    ASSERT_TRUE(wp_pagecanvas_caret(&c, &doc, &pos, &cx, &cy, &ch));
    ASSERT_TRUE(wp_pagecanvas_hit(&c, &doc, cx, cy + ch / 2, &back));
    ASSERT_EQ_U32(back.paragraph, 1);
    ASSERT_EQ_U32(back.offset, 5);
    /* Far right of the line: its end. */
    ASSERT_TRUE(wp_pagecanvas_hit(&c, &doc, x + w - 2, cy + ch / 2, &back));
    ASSERT_EQ_U32(back.paragraph, 1);
    ASSERT_EQ_U32(back.offset, 28);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

/* A footnote is at the paper's foot in page view, as it prints. */
void test_pagecanvas_note_at_foot(void)
{
    wp_document doc;
    wp_undo_journal j;
    wp_pagecanvas c;
    wp_pos at, note;
    i32 x, y, w, h, cx, cy, ch, cy0, ch0;

    ASSERT_TRUE(format_txt_import((const u8 *)"Cited\nAfter\n", 12, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 5;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    (void)wp_pageview_relayout(&doc, 0);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 0, &x, &y, &w, &h));
    ASSERT_TRUE(wp_pagecanvas_caret(&c, &doc, &note, &cx, &cy, &ch));
    memset(&at, 0, sizeof(at));
    ASSERT_TRUE(wp_pagecanvas_caret(&c, &doc, &at, &cx, &cy0, &ch0));
    ASSERT_TRUE(cy > y + h - wp_pagecanvas_zy(&c, doc.sections[0].margin_bottom) - 3 * ch);
    ASSERT_TRUE(cy0 < y + h / 4);
    wp_undo_journal_free(&j);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

static u32 g_rects;
static void count_rect(void *u, i32 x, i32 y, i32 w, i32 h) { (void)u; (void)x; (void)y; (void)w; (void)h; g_rects++; }

/* Following the caret to page two scrolls there; a selection over two
   lines hands back two strips; snapping rounds to the grid only when
   both the grid and snapping are on. */
void test_pagecanvas_follow_select_snap(void)
{
    wp_document doc;
    wp_pagecanvas c;
    wp_pos pos;
    wp_selection sel;
    i32 x, y, w, h;

    doc_of_lines(&doc, 150);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    memset(&pos, 0, sizeof(pos));
    pos.paragraph = 140;
    ASSERT_TRUE(wp_pagecanvas_follow(&c, &doc, &pos));
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 1, &x, &y, &w, &h));
    ASSERT_TRUE(c.scroll_y > y - 400);
    ASSERT_TRUE(!wp_pagecanvas_follow(&c, &doc, &pos));   /* already in view */

    wp_pagecanvas_scroll_to(&c, 0, 0);
    memset(&sel, 0, sizeof(sel));
    sel.anchor.paragraph = 0;
    sel.anchor.offset = 2;
    sel.caret.paragraph = 1;
    sel.caret.offset = 4;
    g_rects = 0;
    wp_pagecanvas_selection(&c, &doc, sel, count_rect, NULL);
    ASSERT_EQ_U32(g_rects, 2);

    ASSERT_EQ_U32((u32)wp_pagecanvas_snap(&c, 500), 500);
    c.grid_on = WP_TRUE;
    ASSERT_EQ_U32((u32)wp_pagecanvas_snap(&c, 500), 500);
    c.snap_on = WP_TRUE;
    ASSERT_EQ_U32((u32)wp_pagecanvas_snap(&c, 500), 360);
    ASSERT_EQ_U32((u32)wp_pagecanvas_snap(&c, 560), 720);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

/* Only what lies wholly inside the view is drawn: a toolkit with no
   clip of its own (MUI's) would otherwise have the page over its status
   line. */
void test_pagecanvas_paint_keeps_to_view(void)
{
    wp_document doc;
    wp_pagecanvas c;
    u32 i, n;
    i32 tx, ty;

    doc_of_lines(&doc, 150);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    c.fit = WP_ZOOM_FIXED;
    c.set_zoom = 100;
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 200) == WP_OK);
    draw_host_reset_text();
    wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, 0, 640, 200);
    n = draw_host_text_count();
    ASSERT_TRUE(n > 0);
    for (i = 0; i < n; i++) {
        (void)draw_host_text_at(i, &tx, &ty);
        ASSERT_TRUE(ty < 200);
    }
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

/* At a zoom, a word goes where the layout put it: its place measured in
   the face the layout used, not in the zoomed face (which a screen
   without one that small opens bigger, and whose widths would carry the
   words past their column). A long word at a low zoom makes the two
   differ by pixels even with the host's proportional faces. */
void test_pagecanvas_zoom_keeps_layout_places(void)
{
    static u8 src[128];
    wp_document doc;
    wp_pagecanvas c;
    wp_paragraph *para;
    const wp_line *line;
    u32 i, n = 0;
    i32 x, y, w, h, tx, ty, want;
    const char *t;

    for (i = 0; i < 70; i++) src[n++] = 'm';
    memcpy(src + n, " next\n", 6);
    n += 6;
    ASSERT_TRUE(format_txt_import(src, n, &doc) == WP_OK);
    (void)wp_pageview_relayout(&doc, 0);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    c.fit = WP_ZOOM_FIXED;
    c.set_zoom = 13;
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 0, &x, &y, &w, &h));
    para = &doc.sections[0].paras[0];
    line = &((const wp_paragraph_layout *)para->layout)->lines[0];
    want = x + wp_pagecanvas_zx(&c, doc.sections[0].margin_left + line->x_twips +
                                    wp_line_cursor_x_twips(&doc, para, line, 0, 71, 0));
    draw_host_reset_text();
    wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, 0, 640, 400);
    n = draw_host_text_count();
    for (i = 0; i < n; i++) {
        t = draw_host_text_at(i, &tx, &ty);
        if (t != NULL && strcmp(t, "next") == 0) break;
    }
    ASSERT_TRUE(i < n);
    ASSERT_TRUE(tx >= want - 1 && tx <= want + 1);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

/* A disc label's page shows the disc's edge and hole as guides -- two
   outlines, the outer as wide as the disc -- and an ordinary page none.
   The paper table names a size either way round. */
void test_pagecanvas_disc_guides(void)
{
    wp_document doc;
    wp_pagecanvas c;
    wp_section *sec;
    i32 x0, y0, x1, y1;
    wp_bool land = WP_FALSE;
    const wp_paper *pp;

    pp = wp_paper_match(13680, 5940, &land);
    ASSERT_TRUE(pp != NULL && strcmp(pp->name, "Envelope #10") == 0 && land);
    pp = wp_paper_match(11906, 16838, &land);
    ASSERT_TRUE(pp != NULL && strcmp(pp->name, "A4") == 0 && !land);
    ASSERT_TRUE(wp_paper_match(9999, 9999, NULL) == NULL);

    ASSERT_TRUE(format_txt_import((const u8 *)"Mix tape\n", 9, &doc) == WP_OK);
    (void)wp_pageview_relayout(&doc, 0);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    draw_host_reset_fills();
    wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, 0, 640, 400);
    ASSERT_EQ_U32(draw_host_polyline_calls(), 0);

    sec = wp_document_section_get(&doc, 0);
    sec->page_width = sec->page_height = WP_DISC_TWIPS;
    sec->margin_left = sec->margin_right = sec->margin_top = sec->margin_bottom = 567;
    sec->paras[0].layout_dirty = WP_TRUE;
    (void)wp_pageview_relayout(&doc, 0);
    c.fit = WP_ZOOM_FIT_PAGE;
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    draw_host_reset_fills();
    wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, 0, 640, 400);
    ASSERT_EQ_U32(draw_host_polyline_calls(), 2);
    ASSERT_TRUE(draw_host_vector_box(&x0, &y0, &x1, &y1));
    ASSERT_TRUE(x1 - x0 >= wp_pagecanvas_zx(&c, WP_DISC_TWIPS) - 2 && x1 - x0 <= wp_pagecanvas_zx(&c, WP_DISC_TWIPS) + 2);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

static i32 px15y(i32 t) { return t / 30; }   /* pixels twice as tall as wide */
static i32 tw15y(i32 p) { return p * 30; }

/* On a screen whose pixels are not square, the disc's outline is the
   page's: as wide as it and as tall as it, in each axis's own pixels. */
void test_pagecanvas_disc_guides_tall_pixels(void)
{
    wp_document doc;
    wp_pagecanvas c;
    wp_section *sec;
    i32 x0, y0, x1, y1;

    ASSERT_TRUE(format_txt_import((const u8 *)"Mix tape\n", 9, &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->page_width = sec->page_height = WP_DISC_TWIPS;
    sec->margin_left = sec->margin_right = sec->margin_top = sec->margin_bottom = 567;
    (void)wp_pageview_relayout(&doc, 0);
    wp_pagecanvas_init(&c, px15, px15y, tw15, tw15y);
    c.fit = WP_ZOOM_FIT_PAGE;
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    draw_host_reset_fills();
    wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, 0, 640, 400);
    ASSERT_TRUE(draw_host_vector_box(&x0, &y0, &x1, &y1));
    ASSERT_TRUE(y1 - y0 >= wp_pagecanvas_zy(&c, WP_DISC_TWIPS) - 2 && y1 - y0 <= wp_pagecanvas_zy(&c, WP_DISC_TWIPS) + 2);
    ASSERT_TRUE(x1 - x0 >= wp_pagecanvas_zx(&c, WP_DISC_TWIPS) - 2 && x1 - x0 <= wp_pagecanvas_zx(&c, WP_DISC_TWIPS) + 2);
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}
