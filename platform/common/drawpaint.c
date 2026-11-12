/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "devout.h"
#include "drawpaint.h"
#include "drawing.h"

typedef struct {
    plat_gc *gc;
    i32 twips, px;   /* the picture's height on paper, and in the pixels drawn */
} paint_ctx;

static void paint_line(void *user, const i32 *xy, u16 n, i32 width, const u8 *rgb)
{
    (void)wp_dev_draw_polyline(((paint_ctx *)user)->gc, xy, n, width, rgb[0], rgb[1], rgb[2]);
}

static void paint_fill(void *user, const i32 *xy, u16 n, const u8 *rgb)
{
    (void)wp_dev_fill_polygon(((paint_ctx *)user)->gc, xy, n, rgb[0], rgb[1], rgb[2]);
}

/* Text at its height in pixels, as a font size in twips: the picture's
   own scale, its display height over the pixels it is drawn in. */
static void paint_text(void *user, i32 x, i32 y, i32 height, u16 angle, u8 align, u8 style,
                       u8 family, const u8 *rgb, const u8 *utf8, u16 len)
{
    paint_ctx *c = (paint_ctx *)user;
    i32 size = c->px > 0 ? (height * c->twips + c->px / 2) / c->px : 0;
    (void)wp_dev_draw_text(c->gc, x, y, height, size, angle, align, style, family, rgb, utf8, len);
}

wp_bool wp_paint_drawing(plat_gc *gc, const wp_document *doc, const wp_image_desc *desc,
                         i32 x, i32 y, i32 w, i32 h)
{
    wp_drawing_sink sink;
    paint_ctx c;

    if (desc == NULL || !WP_IMAGE_IS_DRAWING(desc)) return WP_FALSE;
    c.gc = gc;
    c.twips = desc->display_height_twips;
    c.px = h;
    sink.user = &c;
    sink.line = paint_line;
    sink.fill = paint_fill;
    sink.text = paint_text;
    (void)wp_drawing_render_image(doc, desc, x, y, w, h, &sink);
    return WP_TRUE;
}
