/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "layout.h"
#include "format_rtf.h"
#include "editing.h"
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
