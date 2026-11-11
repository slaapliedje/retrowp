/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_DRAWPAINT_H
#define WP_DRAWPAINT_H

#include "platform.h"
#include "docmodel.h"

/* A drawing (engine/include/drawing.h) on a device: its primitives
   scaled to the box at x, y, w by h pixels and drawn through
   plat_fill_polygon and plat_draw_polyline, so a screen and a printer
   each draw it at their own resolution. WP_FALSE (nothing drawn) when
   desc is not a drawing. */
wp_bool wp_paint_drawing(plat_gc *gc, const wp_document *doc, const wp_image_desc *desc,
                         i32 x, i32 y, i32 w, i32 h);

#endif /* WP_DRAWPAINT_H */
