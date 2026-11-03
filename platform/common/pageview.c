/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "pageview.h"
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

    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        i32 wrap = wp_pageview_wrap_twips(doc, si, window_twips);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            const wp_paragraph_layout *layout = (const wp_paragraph_layout *)para->layout;

            if (layout != NULL && !para->layout_dirty && layout->wrap_twips == wrap) continue;
            (void)wp_paragraph_relayout(doc, para, wrap);
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
