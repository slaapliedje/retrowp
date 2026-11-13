/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "layout.h"
#include "pageview.h"
#include "pagecanvas.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "notes.h"
#include "editing.h"
#include "draw_host.h"

/* Newspaper columns (M19; wp_section.columns). */

static void doc_lines(wp_document *doc, u32 lines)
{
    static u8 src[30000];
    u32 n = 0, k;
    for (k = 0; k < lines && n + 30 < sizeof(src); k++) {
        memcpy(src + n, "Column text that fills.\n", 24);
        n += 24;
    }
    ASSERT_TRUE(format_txt_import(src, n, doc) == WP_OK);
}

static i32 px_ident(i32 t) { return t; }

static const wp_line *line_of(wp_document *doc, u32 pi)
{
    return &((const wp_paragraph_layout *)doc->sections[0].paras[pi].layout)->lines[0];
}

/* Two columns: each half the text less the gap; a full column goes on
   in the next, from its top, before a new page; half the pages. */
void test_columns_flow(void)
{
    wp_document doc;
    wp_section *sec;
    u32 pages1, pages2, pi, first_c1 = 0;
    i32 colw;

    doc_lines(&doc, 200);
    pages1 = wp_pageview_relayout(&doc, 0);
    sec = wp_document_section_get(&doc, 0);
    sec->columns = 2;
    sec->col_gap = 720;
    colw = wp_section_column_twips(sec);
    ASSERT_EQ_U32((u32)colw, (u32)((sec->page_width - sec->margin_left - sec->margin_right - 720) / 2));
    for (pi = 0; pi < sec->para_count; pi++) sec->paras[pi].layout_dirty = WP_TRUE;
    pages2 = wp_pageview_relayout(&doc, 0);
    ASSERT_TRUE(pages2 * 2 <= pages1 + 1);
    ASSERT_EQ_U32(((const wp_paragraph_layout *)sec->paras[0].layout)->wrap_twips, (u32)colw);
    for (pi = 0; pi < sec->para_count; pi++) {
        if (line_of(&doc, pi)->page == 0 && line_of(&doc, pi)->column == 1) { first_c1 = pi; break; }
    }
    ASSERT_TRUE(first_c1 > 0);
    ASSERT_EQ_U32((u32)line_of(&doc, first_c1)->page_y_twips, 0);
    ASSERT_EQ_U32((u32)line_of(&doc, first_c1)->col_x_twips, (u32)(colw + 720));
    ASSERT_TRUE(!line_of(&doc, first_c1)->page_start);
    ASSERT_EQ_U32((u32)line_of(&doc, first_c1 - 1)->column, 0);
    /* Column two's lines end before the next page's first line. */
    for (pi = first_c1; pi < sec->para_count && line_of(&doc, pi)->page == 0; pi++) {
        ASSERT_EQ_U32(line_of(&doc, pi)->column, 1);
    }
    ASSERT_TRUE(pi < sec->para_count && line_of(&doc, pi)->page_start && line_of(&doc, pi)->column == 0);

    /* A printing walk starts column two at the top margin. */
    {
        wp_line_walk w;
        u32 k;
        i32 top = 0;
        wp_line_walk_start(&w, sec->margin_top);
        w.paper = WP_TRUE;
        for (k = 0; k <= first_c1; k++) top = wp_line_walk_next(&w, line_of(&doc, k), px_ident);
        ASSERT_EQ_U32((u32)top, (u32)sec->margin_top);
    }
    wp_document_free(&doc);
}

static i32 px15(i32 t) { return t / 15; }
static i32 tw15(i32 p) { return p * 15; }

/* In page view the caret in column two is right of the page's middle,
   and a click there comes back as the same place. */
void test_columns_page_view(void)
{
    wp_document doc;
    wp_section *sec;
    wp_pagecanvas c;
    wp_pos pos, back;
    u32 pi;
    i32 x, y, w, h, cx, cy, ch;

    doc_lines(&doc, 120);
    sec = wp_document_section_get(&doc, 0);
    sec->columns = 2;
    (void)wp_pageview_relayout(&doc, 0);
    wp_pagecanvas_init(&c, px15, px15, tw15, tw15);
    ASSERT_TRUE(wp_pagecanvas_update(&c, &doc, 640, 400) == WP_OK);
    for (pi = 0; pi < sec->para_count && line_of(&doc, pi)->column != 1; pi++) {}
    ASSERT_TRUE(pi < sec->para_count);
    memset(&pos, 0, sizeof(pos));
    pos.paragraph = pi;
    pos.offset = 4;
    ASSERT_TRUE(wp_pagecanvas_caret(&c, &doc, &pos, &cx, &cy, &ch));
    ASSERT_TRUE(wp_pagecanvas_page_rect(&c, 0, &x, &y, &w, &h));
    ASSERT_TRUE(cx > x + w / 2);
    ASSERT_TRUE(cy < y + h / 4);
    ASSERT_TRUE(wp_pagecanvas_hit(&c, &doc, cx, cy + ch / 2, &back));
    ASSERT_EQ_U32(back.paragraph, pi);
    ASSERT_EQ_U32(back.offset, 4);
    /* Drawn, the words beside it in column two are right of the middle. */
    {
        u32 i, n, right = 0;
        i32 tx, ty;
        draw_host_reset_text();
        /* the band of the caret's line only: the recorder keeps 64 words */
        wp_pagecanvas_paint(&c, &doc, (plat_gc *)&c, 0, 0, 0, cy - 4, 640, ch + 12);
        n = draw_host_text_count();
        for (i = 0; i < n; i++) { (void)draw_host_text_at(i, &tx, &ty); if (tx > x + w / 2) right++; }
        ASSERT_TRUE(right > 0);
    }
    wp_pagecanvas_free(&c);
    wp_document_free(&doc);
}

/* A footnote goes at the foot of the column it is met in; a page break
   in column two starts a new page in column one. */
void test_columns_note_and_break(void)
{
    wp_document doc;
    wp_undo_journal j;
    wp_section *sec;
    wp_pos at, note;
    u32 pi;

    doc_lines(&doc, 120);
    sec = wp_document_section_get(&doc, 0);
    sec->columns = 2;
    (void)wp_pageview_relayout(&doc, 0);
    for (pi = 0; pi < sec->para_count && line_of(&doc, pi)->column != 1; pi++) {}
    ASSERT_TRUE(pi + 2 < sec->para_count);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.paragraph = pi + 1;
    at.offset = 6;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    (void)wp_pageview_relayout(&doc, 0);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(sec->paras[note.paragraph].note != 0);
    ASSERT_EQ_U32(line_of(&doc, note.paragraph)->column, line_of(&doc, at.paragraph)->column);
    ASSERT_EQ_U32((u32)line_of(&doc, note.paragraph)->col_x_twips, (u32)line_of(&doc, at.paragraph)->col_x_twips);
    /* A page break before a paragraph in column two: page two, column one. */
    {
        u32 q = note.paragraph + 2;
        wp_para_style ps = *wp_document_pstyle_get(&doc, sec->paras[q].style);
        wp_pstyle h;
        u32 page_before = line_of(&doc, q)->page;
        ps.page_break_before = 1;
        ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &ps, &h) == WP_OK);
        sec = wp_document_section_get(&doc, 0);
        wp_paragraph_pstyle_set(&sec->paras[q], h);
        sec->paras[q].layout_dirty = WP_TRUE;
        (void)wp_pageview_relayout(&doc, 0);
        ASSERT_EQ_U32(line_of(&doc, q)->page, page_before + 1);
        ASSERT_EQ_U32(line_of(&doc, q)->column, 0);
        ASSERT_TRUE(line_of(&doc, q)->page_start);
    }
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}

/* RTF: \cols and \colsx, both ways. */
void test_columns_rtf(void)
{
    static u8 out[8192];
    static const char rtf[] = "{\\rtf1\\ansi\\cols3\\colsx540 Text.\\par}";
    wp_document doc, back;
    u32 n = 0;

    ASSERT_TRUE(format_rtf_import((const u8 *)rtf, (u32)strlen(rtf), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].columns, 3);
    ASSERT_EQ_U32((u32)doc.sections[0].col_gap, 540);
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &n) == WP_OK);
    out[n] = '\0';
    ASSERT_TRUE(strstr((const char *)out, "\\cols3\\colsx540") != NULL);
    ASSERT_TRUE(format_rtf_import(out, n, &back) == WP_OK);
    ASSERT_EQ_U32(back.sections[0].columns, 3);
    wp_document_free(&back);
    wp_document_free(&doc);
}
