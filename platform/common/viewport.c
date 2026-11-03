/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "viewport.h"
#include "layout.h"

/* ------------------------------------------------------------------ */
/* Walking the document's display lines in order                        */
/* ------------------------------------------------------------------ */

typedef struct {
    wp_document *doc;
    u32 si, pi, li;              /* the CURRENT line, once walk_next said WP_TRUE */
    wp_paragraph_layout *layout; /* its paragraph's layout */
    wp_bool started;
} line_walk;

static void walk_init(line_walk *w, wp_document *doc)
{
    w->doc = doc;
    w->si = 0;
    w->pi = 0;
    w->li = 0;
    w->layout = NULL;
    w->started = WP_FALSE;
}

/* Advances to the next display line in document order; WP_FALSE at the
   end. Paragraphs with no layout are stepped over, as the painters do. */
static wp_bool walk_next(line_walk *w)
{
    if (w->started) w->li++;
    w->started = WP_TRUE;

    for (;;) {
        wp_section *sec;
        wp_paragraph *para;

        if (w->si >= w->doc->section_count) return WP_FALSE;
        sec = wp_document_section_get(w->doc, w->si);
        if (sec == NULL || w->pi >= sec->para_count) {
            w->si++;
            w->pi = 0;
            w->li = 0;
            continue;
        }
        para = wp_section_paragraph_get(sec, w->pi);
        w->layout = (para != NULL) ? (wp_paragraph_layout *)para->layout : NULL;
        if (w->layout == NULL || w->li >= w->layout->line_count) {
            w->pi++;
            w->li = 0;
            continue;
        }
        return WP_TRUE;
    }
}

/* Positions the walk on display line `index`; WP_FALSE past the end. */
static wp_bool walk_to(line_walk *w, wp_document *doc, u32 index)
{
    walk_init(w, doc);
    for (;;) {
        if (!walk_next(w)) return WP_FALSE;
        if (index == 0) return WP_TRUE;
        index--;
    }
}

/* The same half-open test every paint loop has its own copy of: a
   cursor at a line's exclusive end still counts as on it, so a cursor
   between two lines belongs to the first. */
static wp_bool line_contains_pos(const wp_line *line, u32 run, u32 offset)
{
    if (run < line->start_run || run > line->end_run) return WP_FALSE;
    if (run == line->start_run && offset < line->start_offset) return WP_FALSE;
    if (run == line->end_run && offset > line->end_offset) return WP_FALSE;
    return WP_TRUE;
}

static i32 line_height(const wp_viewport *vp, const line_walk *w)
{
    i32 twips = w->layout->lines[w->li].height_twips;

    if (vp->line_px == NULL) return 1;
    return vp->line_px(twips);
}

/* ------------------------------------------------------------------ */

void wp_viewport_init(wp_viewport *vp, wp_viewport_line_px_fn line_px)
{
    vp->top_line = 0;
    vp->view_h_px = 0;
    vp->line_px = line_px;
    vp->manual = WP_FALSE;
}

void wp_viewport_reset(wp_viewport *vp)
{
    vp->top_line = 0;
    vp->manual = WP_FALSE;
}

u32 wp_viewport_cursor_line(wp_document *doc, const wp_pos *cursor, u32 *total_out)
{
    line_walk w;
    u32 index = 0;
    u32 found = 0;
    wp_bool matched = WP_FALSE;

    walk_init(&w, doc);
    while (walk_next(&w)) {
        if (!matched && w.si == cursor->section && w.pi == cursor->paragraph) {
            /* Every line of the cursor's paragraph overwrites `found`
               until one contains the cursor, so a cursor past the last
               line's range ends on the last line. */
            found = index;
            if (line_contains_pos(&w.layout->lines[w.li], cursor->run, cursor->offset)) {
                matched = WP_TRUE;
            }
        }
        index++;
    }
    if (total_out != NULL) *total_out = index;
    return found;
}

wp_bool wp_viewport_line_at(wp_document *doc, u32 index,
                            u32 *section_out, u32 *paragraph_out, u32 *line_out)
{
    line_walk w;

    if (!walk_to(&w, doc, index)) return WP_FALSE;
    if (section_out != NULL) *section_out = w.si;
    if (paragraph_out != NULL) *paragraph_out = w.pi;
    if (line_out != NULL) *line_out = w.li;
    return WP_TRUE;
}

wp_bool wp_viewport_follow_cursor(wp_viewport *vp, wp_document *doc, const wp_pos *cursor)
{
    u32 total, cur;
    u32 old = vp->top_line;

    if (vp->manual) return WP_FALSE;

    cur = wp_viewport_cursor_line(doc, cursor, &total);
    if (total == 0) {
        vp->top_line = 0;
        return (wp_bool)(old != 0);
    }
    if (vp->top_line >= total) vp->top_line = total - 1;

    if (cur < vp->top_line) {
        vp->top_line = cur;
    } else if (vp->view_h_px > 0) {
        /* The height of top_line..cur inclusive, then shed lines from the
           top until that fits -- one walk for the sum, one for the
           shedding, rather than a walk per line. */
        line_walk w;
        i32 h = 0;
        u32 i;

        if (walk_to(&w, doc, vp->top_line)) {
            h = line_height(vp, &w);
            for (i = vp->top_line; i < cur && walk_next(&w); i++) h += line_height(vp, &w);
        }
        if (h > vp->view_h_px && walk_to(&w, doc, vp->top_line)) {
            while (h > vp->view_h_px && vp->top_line < cur) {
                h -= line_height(vp, &w);
                vp->top_line++;
                if (!walk_next(&w)) break;
            }
        }
    }
    return (wp_bool)(vp->top_line != old);
}

u32 wp_viewport_page_lines(const wp_viewport *vp, wp_document *doc)
{
    line_walk w;
    i32 h = 0;
    u32 n = 0;

    if (vp->view_h_px > 0 && walk_to(&w, doc, vp->top_line)) {
        do {
            h += line_height(vp, &w);
            if (h > vp->view_h_px) break;
            n++;
        } while (walk_next(&w));
    }
    return (n > 0) ? n : 1;
}

u32 wp_viewport_max_top(const wp_viewport *vp, wp_document *doc)
{
    line_walk front, back;
    u32 total = 0;
    u32 b = 0;
    i32 h = 0;

    /* The answer is the first line of the longest run of LAST lines
       that fits the view. Two walks slide a window over the document:
       the front adds each line, the back sheds lines while the window
       is too tall. The walk only goes forward, so this is one pass with
       no per-line storage, which a 1MB machine appreciates. */
    if (vp->view_h_px <= 0) return 0;
    walk_init(&front, doc);
    walk_init(&back, doc);
    if (!walk_next(&back)) return 0;

    while (walk_next(&front)) {
        total++;
        h += line_height(vp, &front);
        while (h > vp->view_h_px && b + 1 < total) {
            h -= line_height(vp, &back);
            walk_next(&back);
            b++;
        }
    }
    return b;
}

void wp_viewport_page(wp_viewport *vp, wp_document *doc, wp_pos *cursor, wp_bool down)
{
    u32 total, cur, page, target_cur;
    u32 si, pi, li;

    vp->manual = WP_FALSE;
    cur = wp_viewport_cursor_line(doc, cursor, &total);
    if (total == 0) return;
    page = wp_viewport_page_lines(vp, doc);

    if (down) {
        u32 max_top = wp_viewport_max_top(vp, doc);

        vp->top_line = (vp->top_line + page < max_top) ? vp->top_line + page : max_top;
        target_cur = (cur + page < total - 1) ? cur + page : total - 1;
    } else {
        vp->top_line = (vp->top_line > page) ? vp->top_line - page : 0;
        target_cur = (cur > page) ? cur - page : 0;
    }

    if (wp_viewport_line_at(doc, target_cur, &si, &pi, &li)) {
        wp_section *sec = wp_document_section_get(doc, si);
        wp_paragraph *para = wp_section_paragraph_get(sec, pi);
        const wp_line *line = &((wp_paragraph_layout *)para->layout)->lines[li];

        cursor->section = si;
        cursor->paragraph = pi;
        cursor->run = line->start_run;
        cursor->offset = line->start_offset;
    }
}

void wp_viewport_scroll_to(wp_viewport *vp, wp_document *doc, u32 top_line)
{
    u32 max_top = wp_viewport_max_top(vp, doc);

    vp->top_line = (top_line < max_top) ? top_line : max_top;
    vp->manual = WP_TRUE;
}

void wp_viewport_scroll_by(wp_viewport *vp, wp_document *doc, i32 delta_lines)
{
    u32 target;

    if (delta_lines < 0) {
        u32 up = (u32)(-delta_lines);
        target = (vp->top_line > up) ? vp->top_line - up : 0;
    } else {
        target = vp->top_line + (u32)delta_lines;
    }
    wp_viewport_scroll_to(vp, doc, target);
}

wp_bool wp_viewport_line_at_y(const wp_viewport *vp, wp_document *doc, i32 y_px,
                              u32 *section_out, u32 *paragraph_out, u32 *line_out,
                              i32 *y_top_out)
{
    line_walk w;
    i32 top = 0;
    wp_bool have = WP_FALSE;

    if (!walk_to(&w, doc, vp->top_line)) {
        /* top_line past the end (a document that shrank): the last line. */
        if (!walk_to(&w, doc, 0)) return WP_FALSE;
    }
    do {
        i32 h = line_height(vp, &w);

        have = WP_TRUE;
        *section_out = w.si;
        *paragraph_out = w.pi;
        *line_out = w.li;
        if (y_top_out != NULL) *y_top_out = top;
        if (y_px < top + h) break;      /* on this line (or above the first) */
        top += h;
    } while (walk_next(&w));
    return have;
}

wp_bool wp_viewport_hit_test(const wp_viewport *vp, wp_document *doc, i32 content_width_twips,
                             i32 x_px, i32 y_px,
                             wp_viewport_x_conv_fn px_to_twips_x, wp_viewport_x_conv_fn twips_to_px_x,
                             wp_pos *out)
{
    u32 si, pi, li;
    wp_section *sec;
    wp_paragraph *para;
    wp_paragraph_layout *layout;
    const wp_line *line;
    const wp_para_style *pstyle;
    u8 align;
    i32 origin_px, x_twips, extra_twips;
    u32 run = 0, offset = 0;

    if (!wp_viewport_line_at_y(vp, doc, y_px, &si, &pi, &li, NULL)) return WP_FALSE;
    sec = wp_document_section_get(doc, si);
    para = wp_section_paragraph_get(sec, pi);
    layout = (wp_paragraph_layout *)para->layout;
    line = &layout->lines[li];
    pstyle = wp_document_pstyle_get(doc, para->style);
    align = (pstyle != NULL) ? pstyle->align : WP_ALIGN_LEFT;

    /* The line's own origin (indent and alignment, from the layout) --
       the same x every painter starts the line at. */
    (void)content_width_twips;
    origin_px = twips_to_px_x(line->x_twips);
    x_twips = (x_px > origin_px) ? px_to_twips_x(x_px - origin_px) : 0;
    extra_twips = wp_justify_space_extra_twips(doc, para, line, align,
                                               (wp_bool)(li == layout->line_count - 1),
                                               line->avail_twips);
    (void)wp_line_pos_at_x_twips(doc, para, line, x_twips, extra_twips, &run, &offset);

    out->section = si;
    out->paragraph = pi;
    out->run = run;
    out->offset = offset;
    return WP_TRUE;
}
