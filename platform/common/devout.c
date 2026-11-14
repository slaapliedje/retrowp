/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "devout.h"
#include "drawing.h"
#include "utf8.h"

static plat_gc *g_bound;
static const wp_dev_ops *g_ops;
static void *g_user;

void wp_dev_bind(plat_gc *gc, const wp_dev_ops *ops, void *user)
{
    g_bound = ops != NULL ? gc : NULL;
    g_ops = ops;
    g_user = user;
}

#define OURS(gc) ((gc) != NULL && (gc) == g_bound && g_ops != NULL)

wp_status wp_dev_text(plat_gc *gc, plat_font *f, const wp_document *doc, const wp_char_style *cs,
                      i32 x, i32 y, i32 w, i32 h, const u8 *utf8, u32 len, u8 r, u8 g, u8 b)
{
    if (OURS(gc)) {
        if (g_ops->text != NULL) g_ops->text(g_user, doc, cs, x, y, w, h, utf8, len);
        return WP_OK;
    }
    return plat_draw_text(gc, f, x, y, utf8, len, r, g, b);
}

wp_status wp_dev_fill_rect(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, u8 r, u8 g, u8 b)
{
    if (OURS(gc)) {
        if (g_ops->rect != NULL) g_ops->rect(g_user, x, y, w, h, r, g, b);
        return WP_OK;
    }
    return plat_fill_rect(gc, x, y, w, h, r, g, b);
}

wp_status wp_dev_fill_shade(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, u8 percent)
{
    if (OURS(gc)) {
        if (g_ops->shade != NULL) g_ops->shade(g_user, x, y, w, h, percent);
        return WP_OK;
    }
    return plat_fill_shade(gc, x, y, w, h, percent);
}

wp_status wp_dev_fill_polygon(plat_gc *gc, const i32 *xy, u16 n, u8 r, u8 g, u8 b)
{
    if (OURS(gc)) {
        if (g_ops->polygon != NULL) g_ops->polygon(g_user, xy, n, r, g, b);
        return WP_OK;
    }
    return plat_fill_polygon(gc, xy, n, r, g, b);
}

wp_status wp_dev_draw_polyline(plat_gc *gc, const i32 *xy, u16 n, i32 width, u8 r, u8 g, u8 b)
{
    if (OURS(gc)) {
        if (g_ops->polyline != NULL) g_ops->polyline(g_user, xy, n, width, r, g, b);
        return WP_OK;
    }
    return plat_draw_polyline(gc, xy, n, width, r, g, b);
}

wp_status wp_dev_draw_mono(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, const u16 *rows, u32 row_words,
                           u8 r, u8 g, u8 b)
{
    if (OURS(gc)) {
        if (g_ops->mono != NULL) g_ops->mono(g_user, x, y, w, h, rows, row_words, r, g, b);
        return WP_OK;
    }
    return plat_draw_mono(gc, x, y, w, h, rows, row_words, r, g, b);
}

wp_status wp_dev_draw_text(plat_gc *gc, i32 x, i32 y, i32 height, i32 size_twips, u16 angle,
                           u8 align, u8 style, u8 family, const u8 *rgb, const u8 *utf8, u16 len)
{
    plat_font *f = NULL;
    plat_font_metrics fm;
    i32 asc, desc, a = (i32)(angle % 3600u);
    wp_bool paper;
    u16 flags = (u16)((style & (PLAT_STYLE_BOLD | PLAT_STYLE_ITALIC | PLAT_STYLE_UNDERLINE)) |
                      ((u16)(family & 7u) << PLAT_FAMILY_SHIFT));

    if (OURS(gc)) {
        if (g_ops->dtext != NULL) {
            g_ops->dtext(g_user, x, y, height, angle, align, style, family, rgb[0], rgb[1], rgb[2],
                         utf8, len);
        }
        return WP_OK;
    }
    if (height <= 0 || size_twips <= 0 || len == 0) return WP_OK;
    /* A drawing's text is placed by the face that draws it: paper metrics
       (papermetrics.h) are for laying a document out for print, and a
       label centred by paper widths but drawn in a screen face sits off
       its mark -- two words of a heading run into each other or apart. */
    paper = plat_metrics_paper();
    plat_metrics_set_paper(WP_FALSE);
    if (plat_font_open((const u8 *)"", 0, size_twips, flags, &f) != WP_OK) { plat_metrics_set_paper(paper); return WP_ERR; }
    if (plat_font_metrics_get(f, &fm) != WP_OK) { plat_font_close(f); plat_metrics_set_paper(paper); return WP_ERR; }
    asc = (fm.ascent * height + size_twips / 2) / size_twips;      /* device units */
    desc = (fm.descent * height + size_twips / 2) / size_twips;
    if ((a > 450 && a < 1350) || (a > 2250 && a < 3150)) {
        /* Turned a quarter: the letters upright, one under another --
           up the page from (x, y) at 90 degrees, down it at 270. */
        i32 step = asc + desc, total = 0, at, dir = a < 1800 ? -1 : 1;
        u32 pos = 0, cp, k;
        while (pos < len) { k = utf8_decode(utf8 + pos, len - pos, &cp); pos += k ? k : 1; total += step; }
        at = (align & WP_DRAW_TEXT_CENTER) ? -total / 2 : (align & WP_DRAW_TEXT_RIGHT) ? -total : 0;
        for (pos = 0; pos < len; pos += k) {
            i32 w = 0, cy;
            k = utf8_decode(utf8 + pos, len - pos, &cp);
            if (k == 0) k = 1;
            (void)plat_measure_text(f, utf8 + pos, k, &w);
            w = (w * height + size_twips / 2) / size_twips;
            cy = dir < 0 ? y - at - step + asc : y + at + asc;
            (void)plat_draw_text(gc, f, x - w / 2, cy, utf8 + pos, k, rgb[0], rgb[1], rgb[2]);
            at += step;
        }
    } else {
        i32 w = 0;
        (void)plat_measure_text(f, utf8, len, &w);
        w = (w * height + size_twips / 2) / size_twips;
        if (align & WP_DRAW_TEXT_CENTER) x -= w / 2;
        else if (align & WP_DRAW_TEXT_RIGHT) x -= w;
        if (align & WP_DRAW_TEXT_TOP) y += asc;
        else if (align & WP_DRAW_TEXT_BOTTOM) y -= desc;
        (void)plat_draw_text(gc, f, x, y, utf8, len, rgb[0], rgb[1], rgb[2]);
    }
    plat_font_close(f);
    plat_metrics_set_paper(paper);
    return WP_OK;
}
