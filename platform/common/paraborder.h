/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PARABORDER_H
#define WP_PARABORDER_H

#include "types.h"
#include "layout.h"
#include "platform.h"

/* A line's share of its paragraph's border box (wp_line_border_slice)
   drawn in pixels: the shading (plat_fill_shade, the device's own
   pattern), then the rules over it in black, each at least a pixel.
   One function for every painter that draws in pixels -- the ST, Amiga
   and Mac screens and the page printer -- so the box is the same box
   on each; DOS draws its own with box-drawing characters.

   col_x_px is the column's left edge and line_top_px the line box's
   top, in device pixels; x_px and y_px turn twips into pixels across
   and down. The box is kept within [min_x_px, max_x_px): on a screen
   whose text starts at the window's edge, a rule standing outside the
   text would otherwise fall off it.

   platform/common rules apply: engine headers and platform.h only. */

typedef i32 (*wp_border_px_fn)(i32 twips);

void wp_border_draw_slice(plat_gc *gc, const wp_border_slice *s,
                          i32 col_x_px, i32 line_top_px,
                          wp_border_px_fn x_px, wp_border_px_fn y_px,
                          i32 min_x_px, i32 max_x_px);

#endif /* WP_PARABORDER_H */
