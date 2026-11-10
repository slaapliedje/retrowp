/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "layout.h"
#include "format_rtf.h"
#include "editing.h"
#include "table.h"
#include "pageview.h"
#include "pageprint.h"
#include "draw_host.h"
#include "viewport.h"

/* Tables (docs/document-model.md, "Tables"): a two-cell row, cells
   ending at 2000 and 4000 twips with a gap of 100, "Alpha" in the first
   and two paragraphs, "B" and "C", in the second. Host metrics: a line
   is 300 twips. */
static const char g_row[] =
    "{\\rtf1\\ansi\\trowd\\trgaph100\\cellx2000\\cellx4000"
    "\\intbl Alpha\\cell B\\par C\\cell\\row\\pard after\\par}";

static wp_pos cell_start_pos(u32 para)
{
    wp_pos p;
    p.section = 0; p.paragraph = para; p.run = 0; p.offset = 0;
    return p;
}

static wp_line *line0(wp_document *doc, u32 para)
{
    wp_paragraph_layout *l = (wp_paragraph_layout *)doc->sections[0].paras[para].layout;
    return (l != NULL && l->line_count > 0) ? &l->lines[0] : NULL;
}

/* Each cell wraps at its text width (2000 less the gap each side), its
   lines placed from the row's top: Alpha first in the row, B beside it,
   C below B. Advances walk a table-blind walker past the row: Alpha's
   goes back to the row's top (0), B's down a line, C's to the row's
   bottom -- 600, the second cell's height -- so they sum to the row. */
void test_table_layout_places_cells(void)
{
    wp_document doc;
    wp_line *a, *b, *c, *after;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_row, (u32)strlen(g_row), &doc) == WP_OK);
    (void)wp_pageview_relayout(&doc, 9000);
    a = line0(&doc, 0); b = line0(&doc, 1); c = line0(&doc, 2); after = line0(&doc, 3);
    ASSERT_TRUE(a != NULL && b != NULL && c != NULL && after != NULL);
    if (a == NULL || b == NULL || c == NULL || after == NULL) { wp_document_free(&doc); return; }
    ASSERT_EQ_U32((u32)((wp_paragraph_layout *)doc.sections[0].paras[0].layout)->wrap_twips, 1800);
    ASSERT_EQ_U32(a->flags, WP_LINE_ROW_FIRST);
    ASSERT_EQ_U32((u32)a->cell_x0, 100);
    ASSERT_EQ_U32((u32)a->cell_x1, 1900);
    ASSERT_EQ_U32((u32)a->row_h_twips, 600);
    ASSERT_EQ_U32((u32)a->advance_twips, 0);
    ASSERT_EQ_U32((u32)b->cell_x0, 2100);
    ASSERT_EQ_U32((u32)b->row_dy_twips, 0);
    ASSERT_EQ_U32((u32)b->advance_twips, 300);
    ASSERT_EQ_U32((u32)c->row_dy_twips, 300);
    ASSERT_EQ_U32((u32)c->advance_twips, 300);
    ASSERT_EQ_U32(after->flags, 0);
    ASSERT_EQ_U32((u32)after->cell_x1, 0);
    ASSERT_EQ_U32((u32)after->advance_twips, 300);
    wp_document_free(&doc);
}

/* Out and back: the row's shape before its first paragraph, \cell and
   \cell\row ending the paragraphs, and the same table read back. */
void test_table_rtf_round_trip(void)
{
    wp_document doc, back;
    u8 out[2048];
    u32 len = 0;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_row, (u32)strlen(g_row), &doc) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *)out, "\\trowd\\trgaph100\\trleft0\\cellx2000\\cellx4000") != NULL);
    ASSERT_TRUE(strstr((const char *)out, "\\cell\\row") != NULL);
    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    ASSERT_EQ_U32(back.sections[0].para_count, 4);
    ASSERT_EQ_U32(back.sections[0].paras[0].tflags, WP_TCELL_END);
    ASSERT_EQ_U32(back.sections[0].paras[1].tflags, 0);
    ASSERT_EQ_U32(back.sections[0].paras[2].tflags, WP_TCELL_END | WP_TROW_END);
    ASSERT_TRUE(back.sections[0].paras[1].trow != 0);
    ASSERT_EQ_U32(back.sections[0].paras[3].trow, 0);
    wp_document_free(&back);
    wp_document_free(&doc);
}

/* Printed (pageprint, device pixels = twips): margins of 500, so Alpha
   at 500 + 100, B beside it at 500 + 2100 on the same baseline, C a line
   below B, and "after" back at the margin under the row (two lines,
   the taller cell's). Baselines are a line's top plus 240 of ascent. */
static i32 tw(i32 t) { return t; }
static wp_status no_end(void *user) { WP_UNUSED(user); return WP_OK; }

void test_table_pageprint_places_cells(void)
{
    wp_document doc;
    wp_page_device dev;
    wp_section *sec;
    u32 pages = 0, i;
    i32 x[4] = { 0, 0, 0, 0 }, y[4] = { 0, 0, 0, 0 };
    static const char *const want[4] = { "Alpha", "B", "C", "after" };

    ASSERT_TRUE(format_rtf_import((const u8 *)g_row, (u32)strlen(g_row), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->margin_left = sec->margin_right = sec->margin_top = sec->margin_bottom = 500;
    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = tw;
    dev.y_px = tw;
    dev.end_page = no_end;
    draw_host_reset_text();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(draw_host_text_count(), 4);
    for (i = 0; i < 4; i++) {
        const char *t = draw_host_text_at(i, &x[i], &y[i]);
        ASSERT_TRUE(t != NULL && strcmp(t, want[i]) == 0);
    }
    ASSERT_EQ_U32((u32)x[0], 600);
    ASSERT_EQ_U32((u32)x[1], 2600);
    ASSERT_EQ_U32((u32)y[1], (u32)y[0]);
    ASSERT_EQ_U32((u32)x[2], 2600);
    ASSERT_EQ_U32((u32)y[2], (u32)(y[0] + 300));
    ASSERT_EQ_U32((u32)x[3], 500);
    ASSERT_EQ_U32((u32)y[3], (u32)(y[0] + 600));
    ASSERT_EQ_U32((u32)y[0], 500 + 240);
    wp_document_free(&doc);
}

/* A click in a row (viewport.h, pixels = twips): the cell is the one
   nearest the click's x, the line the one in that cell at its height. */
void test_table_hit_test_finds_the_cell(void)
{
    wp_document doc;
    wp_viewport vp;
    wp_pos pos;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_row, (u32)strlen(g_row), &doc) == WP_OK);
    (void)wp_pageview_relayout(&doc, 9000);
    wp_viewport_init(&vp, tw);
    vp.view_h_px = 100000;
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, 9000, 2500, 350, tw, tw, &pos));
    ASSERT_EQ_U32(pos.paragraph, 2);   /* C: second cell, second line */
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, 9000, 2500, 100, tw, tw, &pos));
    ASSERT_EQ_U32(pos.paragraph, 1);   /* B */
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, 9000, 500, 100, tw, tw, &pos));
    ASSERT_EQ_U32(pos.paragraph, 0);   /* Alpha */
    ASSERT_TRUE(wp_viewport_hit_test(&vp, &doc, 9000, 500, 700, tw, tw, &pos));
    ASSERT_EQ_U32(pos.paragraph, 3);   /* "after", below the row */
    wp_document_free(&doc);
}

/* Enter in a cell keeps both halves in it, the cell's end on the second;
   undo joins them back. Backspace at a cell's start joins nothing. */
void test_table_enter_and_backspace_keep_cells(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_pos at, after;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_row, (u32)strlen(g_row), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    at.section = 0; at.paragraph = 0; at.run = 0; at.offset = 2;   /* "Al|pha" */
    ASSERT_TRUE(wp_edit_split_paragraph(&doc, &journal, at, &after) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 5);
    ASSERT_TRUE(doc.sections[0].paras[0].trow != 0 && doc.sections[0].paras[1].trow != 0);
    ASSERT_EQ_U32(doc.sections[0].paras[0].tflags, 0);
    ASSERT_EQ_U32(doc.sections[0].paras[1].tflags, WP_TCELL_END);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);
    ASSERT_EQ_U32(doc.sections[0].paras[0].tflags, WP_TCELL_END);
    /* "B" starts the second cell: joining it to "Alpha" is refused. */
    ASSERT_TRUE(wp_document_merge_paragraph_raw(&doc, 0, 0, &after) != WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* ---- Editing tables (table.h) ---------------------------------------- */

static void flags_are(wp_document *doc, u32 from, const u8 *want, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) {
        ASSERT_EQ_U32(doc->sections[0].paras[from + i].tflags, want[i]);
        ASSERT_TRUE(doc->sections[0].paras[from + i].trow != 0);
    }
}

/* Insert a 2x3 table after "Hello": six empty cells, each a cell end,
   each row's last a row end; Tab and Shift+Tab walk them, across rows,
   and stop at the table's edges. */
void test_table_insert_and_move(void)
{
    static const u8 two_rows[6] = { 1, 1, 3, 1, 1, 3 };
    wp_document doc;
    wp_undo_journal journal;
    wp_pos at, cell, next;
    const wp_table_row *row;

    ASSERT_TRUE(format_rtf_import((const u8 *)"{\\rtf1 Hello\\par}", 17, &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    at = cell_start_pos(0);
    ASSERT_TRUE(wp_edit_table_insert(&doc, &journal, at, 2, 3, 6000, &cell) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 7);
    flags_are(&doc, 1, two_rows, 6);
    ASSERT_EQ_U32(cell.paragraph, 1);
    row = wp_document_trow_get(&doc, doc.sections[0].paras[1].trow);
    ASSERT_TRUE(row != NULL && row->cell_count == 3 && row->cell_right[2] == 6000);
    ASSERT_TRUE(wp_table_cell_move(&doc, &cell, WP_FALSE, &next) && next.paragraph == 2);
    cell.paragraph = 3;   /* row 1's last cell: Tab goes to row 2 */
    ASSERT_TRUE(wp_table_cell_move(&doc, &cell, WP_FALSE, &next) && next.paragraph == 4);
    ASSERT_TRUE(wp_table_cell_move(&doc, &next, WP_TRUE, &cell) && cell.paragraph == 3);
    cell.paragraph = 6;   /* the table's last cell */
    ASSERT_TRUE(!wp_table_cell_move(&doc, &cell, WP_FALSE, &next));
    cell.paragraph = 1;
    ASSERT_TRUE(!wp_table_cell_move(&doc, &cell, WP_TRUE, &next));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 1);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Rows and columns on that table, each undone: a row after the first
   (three more cells); a column after the first cell, in every row,
   taking the second half of its width; the middle column deleted, the
   cells after it moving left; a row with text in it deleted, and the
   text back on undo. */
void test_table_rows_and_columns(void)
{
    static const u8 four[4] = { 1, 1, 1, 3 };
    static const u8 two[2] = { 1, 3 };
    wp_document doc;
    wp_undo_journal journal;
    wp_pos cell, out;
    const wp_table_row *row;
    wp_pos in;

    ASSERT_TRUE(format_rtf_import((const u8 *)"{\\rtf1 Hello\\par}", 17, &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_edit_table_insert(&doc, &journal, cell_start_pos(0), 2, 3, 6000, &cell) == WP_OK);

    ASSERT_TRUE(wp_edit_table_insert_row(&doc, &journal, cell, &out) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 10);
    ASSERT_EQ_U32(out.paragraph, 4);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 7);

    ASSERT_TRUE(wp_edit_table_insert_column(&doc, &journal, cell, &out) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 9);
    flags_are(&doc, 1, four, 4);
    flags_are(&doc, 5, four, 4);
    row = wp_document_trow_get(&doc, doc.sections[0].paras[1].trow);
    ASSERT_TRUE(row != NULL && row->cell_count == 4);
    if (row != NULL) {
        ASSERT_EQ_U32((u32)row->cell_right[0], 1000);
        ASSERT_EQ_U32((u32)row->cell_right[1], 2000);
        ASSERT_EQ_U32((u32)row->cell_right[3], 6000);
    }
    ASSERT_EQ_U32(doc.sections[0].paras[5].trow, doc.sections[0].paras[1].trow);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 7);

    cell.paragraph = 2;   /* the middle column */
    ASSERT_TRUE(wp_edit_table_delete_column(&doc, &journal, cell, &out) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 5);
    flags_are(&doc, 1, two, 2);
    flags_are(&doc, 3, two, 2);
    row = wp_document_trow_get(&doc, doc.sections[0].paras[1].trow);
    ASSERT_TRUE(row != NULL && row->cell_count == 2 && row->cell_right[1] == 4000);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 7);

    in = cell_start_pos(5);
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, in, (const u8 *)"Strength", 8, &out) == WP_OK);
    ASSERT_TRUE(wp_edit_table_delete_row(&doc, &journal, in, &out) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 7);
    ASSERT_EQ_U32(doc.sections[0].paras[5].runs[0].text.length, 8);
    ASSERT_EQ_U32(doc.sections[0].paras[6].tflags, WP_TCELL_END | WP_TROW_END);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* ---- Frames (docs/document-model.md, "Frames") ----------------------- */

/* "Before", a two-paragraph frame 3000 in from the left margin and 600
   below the top margin, 2000 wide, then "After". Host lines are 300. */
static const char g_frame[] =
    "{\\rtf1\\ansi Before\\par\\pard\\phmrg\\pvmrg\\posx3000\\posy600\\absw2000 Frame one\\par"
    " Frame two\\par\\pard After\\par}";

/* The frame's lines sit at its place, 600 and 900 below the top margin,
   between 3000 and 5000 across, and take no room: "After" follows
   "Before" directly. */
void test_frame_layout(void)
{
    wp_document doc;
    wp_line *before, *f1, *f2, *after;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_frame, (u32)strlen(g_frame), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);
    ASSERT_TRUE(doc.sections[0].paras[1].frame != 0);
    ASSERT_EQ_U32(doc.sections[0].paras[2].frame, doc.sections[0].paras[1].frame);
    ASSERT_EQ_U32(doc.sections[0].paras[3].frame, 0);
    (void)wp_pageview_relayout(&doc, 0);
    before = line0(&doc, 0); f1 = line0(&doc, 1); f2 = line0(&doc, 2); after = line0(&doc, 3);
    ASSERT_TRUE(before != NULL && f1 != NULL && f2 != NULL && after != NULL);
    if (before == NULL || f1 == NULL || f2 == NULL || after == NULL) { wp_document_free(&doc); return; }
    ASSERT_EQ_U32(f1->flags, WP_LINE_FRAME | WP_LINE_FRAME_FIRST);
    ASSERT_EQ_U32(f2->flags, WP_LINE_FRAME);
    ASSERT_EQ_U32((u32)f1->cell_x0, 3000);
    ASSERT_EQ_U32((u32)f1->cell_x1, 5000);
    ASSERT_EQ_U32((u32)f1->page_y_twips, 600);
    ASSERT_EQ_U32((u32)f2->page_y_twips, 900);
    ASSERT_EQ_U32((u32)after->page_y_twips, 300);
    ASSERT_EQ_U32((u32)f1->advance_twips, 0);
    ASSERT_EQ_U32((u32)((wp_paragraph_layout *)doc.sections[0].paras[1].layout)->wrap_twips, 2000);
    wp_document_free(&doc);
}

/* Printed (pixels = twips, default inch margins): the frame's first word
   at 1440 + 3000 across and 1440 + 600 + 240 down, its second line a
   line lower; "After" right under "Before". And out and back in RTF. */
void test_frame_pageprint_and_rtf(void)
{
    wp_document doc, back;
    wp_page_device dev;
    u32 pages = 0, n, i;
    i32 x, y;
    u8 out[2048];
    u32 len = 0;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_frame, (u32)strlen(g_frame), &doc) == WP_OK);
    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = tw;
    dev.y_px = tw;
    dev.end_page = no_end;
    draw_host_reset_text();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    n = draw_host_text_count();
    ASSERT_EQ_U32(n, 6);   /* Before, Frame, one, Frame, two, After */
    for (i = 0; i < n; i++) {
        const char *t = draw_host_text_at(i, &x, &y);
        if (i == 0) { ASSERT_EQ_STR(t, "Before"); ASSERT_EQ_U32((u32)y, 1680); }
        if (i == 1) { ASSERT_EQ_STR(t, "Frame"); ASSERT_EQ_U32((u32)x, 4440); ASSERT_EQ_U32((u32)y, 2280); }
        if (i == 3) { ASSERT_EQ_STR(t, "Frame"); ASSERT_EQ_U32((u32)y, 2580); }
        if (i == 5) { ASSERT_EQ_STR(t, "After"); ASSERT_EQ_U32((u32)x, 1440); ASSERT_EQ_U32((u32)y, 1980); }
    }
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *)out, "\\phmrg\\pvmrg\\posx3000\\posy600\\absw2000") != NULL);
    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    ASSERT_TRUE(back.sections[0].paras[1].frame != 0 && back.sections[0].paras[3].frame == 0);
    wp_document_free(&back);
    wp_document_free(&doc);
}

/* Two boxed frames side by side, two paragraphs each: each frame is its
   own box -- the first frame's last paragraph keeps its bottom rule, the
   second's first paragraph its top. */
void test_frame_boxes_stay_apart(void)
{
    static const char rtf[] =
        "{\\rtf1\\ansi\\pard\\phmrg\\posx0\\absw4000\\box\\brdrs A\\par"
        "\\pard\\phmrg\\posx0\\absw4000\\box\\brdrs B\\par"
        "\\pard\\phmrg\\posx4800\\absw4000\\box\\brdrs C\\par"
        "\\pard\\phmrg\\posx4800\\absw4000\\box\\brdrs D\\par}";
    static const u8 want[4] = {
        WP_BORDER_TOP | WP_BORDER_LEFT | WP_BORDER_RIGHT,
        WP_BORDER_BOTTOM | WP_BORDER_LEFT | WP_BORDER_RIGHT,
        WP_BORDER_TOP | WP_BORDER_LEFT | WP_BORDER_RIGHT,
        WP_BORDER_BOTTOM | WP_BORDER_LEFT | WP_BORDER_RIGHT
    };
    wp_document doc;
    u32 i;

    ASSERT_TRUE(format_rtf_import((const u8 *)rtf, (u32)strlen(rtf), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);
    for (i = 0; i < 4 && i < doc.sections[0].para_count; i++) {
        const wp_para_style *ps = wp_document_pstyle_get(&doc, doc.sections[0].paras[i].style);
        ASSERT_TRUE(ps != NULL);
        if (ps != NULL) ASSERT_EQ_U32(ps->border_sides, want[i]);
    }
    wp_document_free(&doc);
}

/* Scrolled so "After" (300 below the top margin) is the view's top line
   at y 1000: the page's top margin is at 700, and the frame -- its own
   lines above the view -- is placed from there, its first line at 1300,
   its second 300 lower. */
void test_frame_walk_scrolled(void)
{
    wp_document doc;
    wp_line_walk w;
    u32 page = 7;

    ASSERT_TRUE(format_rtf_import((const u8 *)g_frame, (u32)strlen(g_frame), &doc) == WP_OK);
    (void)wp_pageview_relayout(&doc, 0);
    wp_line_walk_start_view(&w, 1000, &doc, 3, tw, &page);
    ASSERT_EQ_U32(page, 0);
    ASSERT_EQ_U32((u32)w.page_top, 700);
    ASSERT_EQ_U32((u32)wp_line_walk_next(&w, line0(&doc, 1), tw), 1300);
    ASSERT_EQ_U32((u32)wp_line_walk_next(&w, line0(&doc, 2), tw), 1600);
    ASSERT_EQ_U32((u32)wp_line_walk_next(&w, line0(&doc, 3), tw), 1000);
    wp_document_free(&doc);
}
