/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "paraborder.h"

/* A rule's pixels: at least one, so a hairline never vanishes. */
static i32 at_least_one(i32 px)
{
    return px < 1 ? 1 : px;
}

/* One side's rules across [a, b) along the side, starting `at` and
   growing inward by `dir` (+1 or -1): a single or thick rule `thick`
   pixels deep; a double, two rules of `rule` with a rule's gap. */
static void draw_rules(plat_gc *gc, wp_bool horizontal, i32 at, i32 dir, i32 a, i32 b,
                       i32 thick, i32 rule, wp_bool dbl)
{
    i32 k, n = dbl ? 2 : 1;

    for (k = 0; k < n; k++) {
        i32 depth = dbl ? rule : thick;
        i32 start = at + dir * (k * 2 * rule);
        i32 lo = dir > 0 ? start : start - depth;

        if (horizontal) (void)plat_fill_rect(gc, a, lo, b - a, depth, 0, 0, 0);
        else (void)plat_fill_rect(gc, lo, a, depth, b - a, 0, 0, 0);
    }
}

void wp_border_draw_slice(plat_gc *gc, const wp_border_slice *s,
                          i32 col_x_px, i32 line_top_px,
                          wp_border_px_fn x_px, wp_border_px_fn y_px,
                          i32 min_x_px, i32 max_x_px)
{
    i32 x0 = col_x_px + x_px(s->x0), x1 = col_x_px + x_px(s->x1);
    i32 y0 = line_top_px + y_px(s->top), y1 = line_top_px + y_px(s->bottom);
    wp_bool dbl = (wp_bool)(s->kind == WP_BORDER_DOUBLE);
    i32 rx = at_least_one(x_px(s->rule)), ry = at_least_one(y_px(s->rule));
    i32 tx = dbl ? 3 * rx : at_least_one(x_px(s->thickness));
    i32 ty = dbl ? 3 * ry : at_least_one(y_px(s->thickness));

    if (x0 < min_x_px) x0 = min_x_px;
    if (x1 > max_x_px) x1 = max_x_px;
    if (x1 <= x0 || y1 <= y0) return;
    if (s->shading > 0) (void)plat_fill_shade(gc, x0, y0, x1 - x0, y1 - y0, s->shading);
    if (s->sides & WP_BORDER_TOP)    draw_rules(gc, WP_TRUE, y0, 1, x0, x1, ty, ry, dbl);
    if (s->sides & WP_BORDER_BOTTOM) draw_rules(gc, WP_TRUE, y1, -1, x0, x1, ty, ry, dbl);
    if (s->sides & WP_BORDER_LEFT)   draw_rules(gc, WP_FALSE, x0, 1, y0, y1, tx, rx, dbl);
    if (s->sides & WP_BORDER_RIGHT)  draw_rules(gc, WP_FALSE, x1, -1, y0, y1, tx, rx, dbl);
}
