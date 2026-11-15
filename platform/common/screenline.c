/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "screenline.h"
#include "devout.h"

void wp_screenline_reset(wp_screenline *sl)
{
    sl->n = 0;
    sl->identity = WP_TRUE;
}

void wp_screenline_add(wp_screenline *sl, i32 paper_x, i32 paper_w, i32 ink_w,
                       u16 gap, u8 part, u32 run_index, u32 byte_start)
{
    wp_screen_word *w;

    if (sl->n >= WP_SCREENLINE_MAX) {
        /* Too many to keep: marked full, and place lays the whole line
           out as the layout did, which is never wrong, only gappy. */
        sl->n = WP_SCREENLINE_MAX + 1u;
        return;
    }
    if (paper_w < 0) paper_w = 0;
    w = &sl->w[sl->n++];
    w->ink_full = ink_w < 0 ? paper_w : ink_w;
    if (ink_w < 0 || ink_w > paper_w) ink_w = paper_w;   /* unknown, or held to paper by the fit */
    w->paper_x = paper_x;
    w->paper_w = paper_w;
    w->ink_w = ink_w;
    w->screen_x = paper_x;
    w->gap = gap;
    w->part = part;
    w->run_index = run_index;
    w->byte_start = byte_start;
    if (ink_w != paper_w) sl->identity = WP_FALSE;
}

void wp_screenline_place(wp_screenline *sl, u8 align, wp_bool justified)
{
    u16 i = 0, parts = 0;

    if (sl->n > WP_SCREENLINE_MAX) { sl->n = 0; sl->identity = WP_TRUE; return; }
    if (sl->identity) return;
    if (sl->n > 0) parts = (u16)(sl->w[sl->n - 1].part != sl->w[0].part);
    while (i < sl->n) {
        u16 j, first = i, last = i;
        i32 slack = 0, total = 0, gaps;
        u8 part = sl->w[i].part;

        while (last + 1u < sl->n && sl->w[last + 1u].part == part) last++;
        for (j = first; j <= last; j++) total += sl->w[j].paper_w - sl->w[j].ink_w;
        gaps = (i32)sl->w[last].gap - (i32)sl->w[first].gap;
        for (j = first; j <= last; j++) {
            wp_screen_word *w = &sl->w[j];
            i32 shift = 0;

            /* Ranged by the paragraph only when the line is one part: a
               tabbed line's parts each keep their start at their stop. */
            if (!parts) {
                if (align == WP_ALIGN_RIGHT) shift = total;
                else if (align == WP_ALIGN_CENTER) shift = total / 2;
                else if (align == WP_ALIGN_JUSTIFY && justified && gaps > 0)
                    shift = (i32)(((long)total * (long)((i32)w->gap - (i32)sl->w[first].gap)) / (long)gaps);
            }
            w->screen_x = w->paper_x - slack + shift;
            slack += w->paper_w - w->ink_w;
        }
        i = (u16)(last + 1u);
    }
}

const wp_screen_word *wp_screenline_word(const wp_screenline *sl, u16 *k,
                                         u32 run_index, u32 byte_start)
{
    u16 i;

    for (i = *k; i < sl->n; i++) {
        if (sl->w[i].run_index == run_index && sl->w[i].byte_start == byte_start) {
            *k = (u16)(i + 1u);
            return &sl->w[i];
        }
    }
    return NULL;
}

/* Between two points, a straight line: (a0,b0) to (a1,b1) at a. */
static i32 lerp(i32 a, i32 a0, i32 a1, i32 b0, i32 b1)
{
    if (a1 <= a0) return b0;
    return b0 + (i32)(((long)(a - a0) * (long)(b1 - b0)) / (long)(a1 - a0));
}

i32 wp_screenline_x(const wp_screenline *sl, i32 x)
{
    u16 i;
    i32 pe = 0, se = 0;   /* the end of the word before, on paper and on screen */

    if (sl->identity || sl->n == 0) return x;
    for (i = 0; i < sl->n; i++) {
        const wp_screen_word *w = &sl->w[i];

        if (x < w->paper_x) return lerp(x, pe, w->paper_x, se, w->screen_x);
        if (x <= w->paper_x + w->paper_w)
            return lerp(x, w->paper_x, w->paper_x + w->paper_w, w->screen_x, w->screen_x + w->ink_w);
        pe = w->paper_x + w->paper_w;
        se = w->screen_x + w->ink_w;
    }
    return se + (x - pe);
}

i32 wp_screenline_paper_x(const wp_screenline *sl, i32 x)
{
    u16 i;
    i32 pe = 0, se = 0;

    if (sl->identity || sl->n == 0) return x;
    for (i = 0; i < sl->n; i++) {
        const wp_screen_word *w = &sl->w[i];

        if (x < w->screen_x) return lerp(x, se, w->screen_x, pe, w->paper_x);
        if (x <= w->screen_x + w->ink_w)
            return lerp(x, w->screen_x, w->screen_x + w->ink_w, w->paper_x, w->paper_x + w->paper_w);
        pe = w->paper_x + w->paper_w;
        se = w->screen_x + w->ink_w;
    }
    return pe + (x - se);
}

void wp_screenline_build(wp_screenline *sl, wp_document *doc, wp_paragraph *para,
                         const wp_line *line, u8 align, i32 extra_twips,
                         wp_screenline_measure_fn measure, void *user)
{
    wp_line_fragment_iter iter;
    wp_line_fragment frag;
    i32 x = 0;
    u16 gap = 0;
    u8 part = 0;

    wp_screenline_reset(sl);
    if (!plat_metrics_paper()) return;   /* the screen's widths are the layout's */
    wp_line_fragment_iter_init(&iter, line);
    while (wp_line_next_fragment(doc, para, line, &iter, &frag)) {
        wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);
        u32 len = frag.byte_end - frag.byte_start;
        const u8 *bytes;
        i32 pw = 0, iw = -1;

        if (frag.is_tab) {
            x += frag.tab_twips;
            part++;
            gap = 0;
            continue;
        }
        if (run == NULL) continue;
        bytes = wp_strpool_get(&doc->strings, run->text);
        if (bytes == NULL || len == 0) continue;
        if (!measure(user, doc, run->style, bytes + frag.byte_start, len, &pw, &iw)) continue;
        if (frag.is_space) {
            x += pw + extra_twips;
            gap++;
            continue;
        }
        wp_screenline_add(sl, x, pw, iw, gap, part, frag.run_index, frag.byte_start);
        x += pw;
    }
    wp_screenline_place(sl, align, (wp_bool)(extra_twips > 0));
}

wp_bool wp_screenline_measure_screen(void *user, wp_document *doc, u16 style,
                                     const u8 *utf8, u32 len, i32 *paper_w, i32 *ink_w)
{
    wp_screen_fonts *f = (wp_screen_fonts *)user;

    if (!f->have || f->style != style) {
        const wp_char_style *cs = wp_document_cstyle_get(doc, style);

        if (f->font != NULL) plat_font_close(f->font);
        f->font = NULL;
        f->have = WP_FALSE;
        if (cs == NULL || wp_cstyle_font_open(doc, cs, &f->font) != WP_OK) { f->font = NULL; return WP_FALSE; }
        f->style = style;
        f->have = WP_TRUE;
    }
    *paper_w = 0;
    (void)plat_measure_text(f->font, utf8, len, paper_w);
    *ink_w = wp_dev_ink_twips(f->font, utf8, len);
    return WP_TRUE;
}

void wp_screen_fonts_close(wp_screen_fonts *f)
{
    if (f->font != NULL) plat_font_close(f->font);
    f->font = NULL;
    f->have = WP_FALSE;
}
