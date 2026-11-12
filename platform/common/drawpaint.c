/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "devout.h"
#include "drawpaint.h"
#include "drawing.h"

static void paint_line(void *user, const i32 *xy, u16 n, i32 width, const u8 *rgb)
{
    (void)wp_dev_draw_polyline((plat_gc *)user, xy, n, width, rgb[0], rgb[1], rgb[2]);
}

static void paint_fill(void *user, const i32 *xy, u16 n, const u8 *rgb)
{
    (void)wp_dev_fill_polygon((plat_gc *)user, xy, n, rgb[0], rgb[1], rgb[2]);
}

wp_bool wp_paint_drawing(plat_gc *gc, const wp_document *doc, const wp_image_desc *desc,
                         i32 x, i32 y, i32 w, i32 h)
{
    wp_drawing_sink sink;

    if (desc == NULL || !WP_IMAGE_IS_DRAWING(desc)) return WP_FALSE;
    sink.user = gc;
    sink.line = paint_line;
    sink.fill = paint_fill;
    (void)wp_drawing_render_image(doc, desc, x, y, w, h, &sink);
    return WP_TRUE;
}
