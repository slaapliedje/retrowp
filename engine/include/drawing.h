/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_DRAWING_H
#define WP_DRAWING_H

#include "types.h"
#include "docmodel.h"

/* Vector drawings (docs/document-model.md, "Drawings"). A drawing is a
   picture paragraph whose descriptor holds, instead of a pixel cache, a
   list of primitives in the drawing's own coordinates -- 0..width_px
   across and 0..height_px down -- scaled to the display box wherever it
   is drawn, so it is as sharp as the device it lands on. The list lives
   in the string pool (`wp_image_desc.drawing`), big-endian, each record
   an op byte and its fields:

     PEN     1  style u8 (0 none, 1 solid), width u16, r g b
     BRUSH   2  style u8 (0 none, 1 solid), r g b
     LINE    3  n u16, n x (x i16, y i16)    an open polyline
     POLYGON 4  n u16, n x (x i16, y i16)    closed
     RECT    5  left top right bottom (i16)
     RRECT   6  left top right bottom, corner ellipse w h
     ELLIPSE 7  left top right bottom
     ARC     8  left top right bottom, xs ys xe ye: the ellipse's arc
                counter-clockwise (as it looks) from the ray through
                (xs, ys) to the one through (xe, ye), as a metafile's

   Shapes are filled with the brush, then outlined with the pen; a line
   and an arc take only the pen. Until a PEN or BRUSH says otherwise the
   pen is solid black and one pixel wide and the brush solid white -- a
   Windows metafile's defaults, so one round-trips unchanged. */

enum {
    WP_DRAW_PEN = 1, WP_DRAW_BRUSH, WP_DRAW_LINE, WP_DRAW_POLYGON,
    WP_DRAW_RECT, WP_DRAW_RRECT, WP_DRAW_ELLIPSE, WP_DRAW_ARC
};

#define WP_IMAGE_IS_DRAWING(d) ((d)->drawing.length > 0)

/* Building a list: each call appends a record; the first failure sticks
   in `error` and later calls do nothing. */
typedef struct {
    u8       *bytes;
    u32       length, capacity;
    wp_status error;
} wp_drawing_builder;

void wp_drawing_builder_init(wp_drawing_builder *b);
void wp_drawing_builder_free(wp_drawing_builder *b);
void wp_drawing_pen(wp_drawing_builder *b, u8 style, u16 width, u8 r, u8 g, u8 bl);
void wp_drawing_brush(wp_drawing_builder *b, u8 style, u8 r, u8 g, u8 bl);
/* LINE or POLYGON: n points, xy[2n]. */
void wp_drawing_poly(wp_drawing_builder *b, u8 op, const i16 *xy, u16 n);
/* RECT, ELLIPSE (4 values), RRECT (6) or ARC (8), in the order above. */
void wp_drawing_shape(wp_drawing_builder *b, u8 op, const i16 *v);

/* A drawing registered and its descriptor filled in: the list copied
   into the pool, its extent, its display box in twips; *out is its
   picture handle, for a WP_PARA_IMAGE paragraph. `name` (may be NULL)
   is kept as the source name. */
wp_status wp_document_drawing_add(wp_document *doc, const u8 *ops, u32 len,
                                  i32 ext_w, i32 ext_h, i32 display_w, i32 display_h,
                                  const char *name, wp_image *out);

/* Drawing it: the primitives flattened -- curves into enough straight
   segments to look round at the size drawn -- and handed to the sink in
   device pixels. `line` gets an open polyline and its pen (width in
   pixels, at least 1), `fill` a closed polygon and its brush colour. */
typedef struct {
    void *user;
    void (*line)(void *user, const i32 *xy, u16 n, i32 width, const u8 *rgb);
    void (*fill)(void *user, const i32 *xy, u16 n, const u8 *rgb);
} wp_drawing_sink;

/* The list scaled from its extent to the box at x, y, w by h pixels.
   WP_BADFMT where a record runs past the end (what came before it is
   drawn); WP_NOMEM where a long primitive's points would not fit in
   memory (it is skipped, the rest drawn). */
wp_status wp_drawing_render(const u8 *ops, u32 len, i32 ext_w, i32 ext_h,
                            i32 x, i32 y, i32 w, i32 h, const wp_drawing_sink *sink);

/* The same, from a picture's descriptor. */
wp_status wp_drawing_render_image(const wp_document *doc, const wp_image_desc *desc,
                                  i32 x, i32 y, i32 w, i32 h, const wp_drawing_sink *sink);

#endif /* WP_DRAWING_H */
