/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "devout.h"

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
