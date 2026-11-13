/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "pageview.h"
#include "notes.h"
#include "layout.h"
#include "viewport.h"
#include "platform.h"

i32 wp_pageview_wrap_twips(const wp_document *doc, u32 section, i32 window_twips)
{
    i32 column;

    if (section >= doc->section_count) return (window_twips > 0) ? window_twips : 9360;
    column = wp_section_column_twips(&doc->sections[section]);
    if (window_twips > 0 && window_twips < column) return window_twips;
    return column;
}

u32 wp_pageview_relayout(wp_document *doc, i32 window_twips)
{
    u32 si, pi;

    /* Footnotes numbered by where their references stand now (notes.h);
       a document without any is not touched. */
    (void)wp_notes_renumber(doc);

    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        i32 wrap = wp_pageview_wrap_twips(doc, si, window_twips);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            const wp_paragraph_layout *layout = (const wp_paragraph_layout *)para->layout;

            i32 w = wp_paragraph_wrap_twips(doc, sec, pi, wrap);   /* a cell's, a frame's, or wrap */

            if (layout != NULL && !para->layout_dirty && layout->wrap_twips == w) continue;
            (void)wp_paragraph_relayout(doc, para, w);
        }
    }
    return wp_document_mark_pages(doc);
}

u32 wp_pageview_cursor_page(wp_document *doc, const wp_pos *cursor, u32 *page_count_out)
{
    u32 total = 0, index, si, pi, li;
    u32 page = 1, count = 1;

    index = wp_viewport_cursor_line(doc, cursor, &total);
    if (total > 0) {
        if (wp_viewport_line_at(doc, index, &si, &pi, &li)) {
            const wp_paragraph_layout *l =
                (const wp_paragraph_layout *)doc->sections[si].paras[pi].layout;
            page = l->lines[li].page + 1;
        }
        if (wp_viewport_line_at(doc, total - 1, &si, &pi, &li)) {
            const wp_paragraph_layout *l =
                (const wp_paragraph_layout *)doc->sections[si].paras[pi].layout;
            count = l->lines[li].page + 1;
        }
    }
    if (page_count_out != NULL) *page_count_out = count;
    return page;
}

void wp_pageview_draw_break(plat_gc *gc, i32 x, i32 y, i32 w)
{
    i32 at;

    /* Dark enough to stay black on a monochrome screen's threshold. */
    for (at = 0; at < w; at += 8) {
        i32 dash = (w - at < 4) ? w - at : 4;
        (void)plat_fill_rect(gc, x + at, y, dash, 1, 0x40, 0x40, 0x40);
    }
}

void wp_line_walk_start(wp_line_walk *w, i32 y)
{
    w->y = y;
    w->row_top = w->row_bottom = w->cell_y = y;
    w->in_row = WP_FALSE;
    w->page_top = w->frame_y = y;
    w->paper = WP_FALSE;
    w->column = 0;
}

i32 wp_line_walk_next(wp_line_walk *w, const wp_line *line, wp_line_walk_px_fn px)
{
    i32 top;

    /* A frame line: at its frame's place on the page the walk is on,
       taking no room. */
    if (line->flags & WP_LINE_FRAME) {
        if (line->flags & WP_LINE_FRAME_FIRST) w->frame_y = w->page_top + px(line->frame_top_twips);
        top = w->frame_y;
        w->frame_y += px(line->height_twips);
        return top;
    }
    if (w->paper && line->column != w->column) {
        /* The next column (M19): from its top, at the line's place there. */
        w->in_row = WP_FALSE;
        w->y = w->page_top + px(line->page_y_twips);
        w->column = line->column;
    }
    if (line->flags & WP_LINE_ROW_FIRST) {
        if (w->in_row) w->y = w->row_bottom;
        w->row_top = w->cell_y = w->y;
        /* The row is at least its height (a row given one, \trrh, may be
           taller than its text -- a sheet of labels); its cells' text
           can only push the bottom further. */
        w->row_bottom = w->y + px(line->row_h_twips);
        w->in_row = WP_TRUE;
        w->page_top = w->y - px(line->page_y_twips);
    }
    if (w->in_row && line->cell_x1 != 0) {
        if (line->row_dy_twips == 0) w->cell_y = w->row_top;   /* a cell's first line */
        top = w->cell_y;
        w->cell_y += px(line->height_twips);
        if (w->cell_y > w->row_bottom) w->row_bottom = w->cell_y;
        return top;
    }
    if (w->in_row) {
        w->y = w->row_bottom;
        w->in_row = WP_FALSE;
    }
    if (line->flags & WP_LINE_NOTE) {
        /* A footnote: on paper at its page's foot, taking no room; on
           screen in the flow, under the paragraph that cites it, where
           it can be read and edited -- a screen page is only as tall as
           its text, so the foot is not a place on it. */
        if (w->paper) return w->page_top + px(line->page_y_twips);
        top = w->y;
        w->y += px(line->height_twips);
        return top;
    }
    top = w->y;
    w->page_top = top - px(line->page_y_twips);
    w->y += px(line->height_twips);
    return top;
}

i32 wp_line_walk_end(wp_line_walk *w)
{
    if (w->in_row) {
        w->y = w->row_bottom;
        w->in_row = WP_FALSE;
    }
    return w->y;
}

i32 wp_table_row_extent(const wp_section *sec, u32 para_index, wp_line_walk_px_fn px)
{
    i32 tallest = 0, cell = 0;
    u32 pi;

    for (pi = para_index; pi < sec->para_count && sec->paras[pi].trow != 0; pi++) {
        const wp_paragraph *para = &sec->paras[pi];
        const wp_paragraph_layout *l = (const wp_paragraph_layout *)para->layout;
        u32 li;

        if (l != NULL) {
            for (li = 0; li < l->line_count; li++) cell += px(l->lines[li].height_twips);
        }
        if (para->tflags & WP_TCELL_END) {
            if (cell > tallest) tallest = cell;
            cell = 0;
        }
        if (para->tflags & WP_TROW_END) break;
    }
    if (cell > tallest) tallest = cell;
    {
        /* A row given a height is at least that tall (its first line
           carries the row's height, wp_document_mark_pages). */
        const wp_paragraph_layout *l = (const wp_paragraph_layout *)sec->paras[para_index].layout;
        if (para_index < sec->para_count && l != NULL && l->line_count > 0) {
            i32 h = px(l->lines[0].row_h_twips);
            if (h > tallest) tallest = h;
        }
    }
    return tallest;
}

void wp_line_walk_start_view(wp_line_walk *w, i32 y, wp_document *doc, u32 top_line,
                             wp_line_walk_px_fn px, u32 *page_out)
{
    u32 i, si, pi, li;

    wp_line_walk_start(w, y);
    *page_out = ~(u32)0;
    if (doc->frame_count <= 1) return;   /* no frames: nothing to place */
    /* The first ordinary line from the top: frame lines say nothing of
       where the text is. */
    for (i = top_line; i < top_line + 256u && wp_viewport_line_at(doc, i, &si, &pi, &li); i++) {
        const wp_paragraph_layout *l =
            (const wp_paragraph_layout *)doc->sections[si].paras[pi].layout;
        const wp_line *ln = &l->lines[li];
        if (ln->flags & WP_LINE_FRAME) continue;
        w->page_top = y - px(ln->page_y_twips);
        *page_out = ln->page;
        return;
    }
}
