/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_DEVOUT_H
#define WP_DEVOUT_H

#include "platform.h"
#include "docmodel.h"

/* Where the shared drawing goes -- the page printer (pageprint.c), the
   borders and table rules (paraborder.c), drawings (drawpaint.c). To the
   platform (plat_*) for a screen or a GDOS printer; or, for a gc bound
   here, to a device of RetroWP's own -- the PostScript writer (psdev.h)
   -- which draws the same page in its own language. A bound gc is known
   by its pointer, never by looking inside it. One at a time. */
typedef struct {
    /* utf8 at x, its baseline y, w wide as measured and h from the
       font's ascent to its descent (device units: the line the layout
       made room for), in cs's face and colour. */
    void (*text)(void *user, const wp_document *doc, const wp_char_style *cs,
                 i32 x, i32 y, i32 w, i32 h, const u8 *utf8, u32 len);
    void (*rect)(void *user, i32 x, i32 y, i32 w, i32 h, u8 r, u8 g, u8 b);
    void (*shade)(void *user, i32 x, i32 y, i32 w, i32 h, u8 percent);
    void (*polygon)(void *user, const i32 *xy, u16 n, u8 r, u8 g, u8 b);
    void (*polyline)(void *user, const i32 *xy, u16 n, i32 width, u8 r, u8 g, u8 b);
    void (*mono)(void *user, i32 x, i32 y, i32 w, i32 h, const u16 *rows, u32 row_words,
                 u8 r, u8 g, u8 b);
    /* A drawing's text (drawing.h's TEXT): utf8 at x, y, height device
       units tall, turned `angle` tenths of a degree, placed by `align`
       (WP_DRAW_TEXT_*), in the style and family's standard face. */
    void (*dtext)(void *user, i32 x, i32 y, i32 height, u16 angle, u8 align, u8 style,
                  u8 family, u8 r, u8 g, u8 b, const u8 *utf8, u16 len);
} wp_dev_ops;

/* gc's drawing goes to ops from now on (NULL ops: back to the platform). */
void wp_dev_bind(plat_gc *gc, const wp_dev_ops *ops, void *user);

/* As plat_draw_text, plat_fill_rect, plat_fill_shade, plat_fill_polygon,
   plat_draw_polyline and plat_draw_mono, for whichever gc is given. Text
   also takes the style its font was opened from, its measured width and
   its font's height, which a device of its own draws by. */
wp_status wp_dev_text(plat_gc *gc, plat_font *f, const wp_document *doc, const wp_char_style *cs,
                      i32 x, i32 y, i32 w, i32 h, const u8 *utf8, u32 len, u8 r, u8 g, u8 b);

/* Text held to its laid-out width on a screen. With paper metrics on
   (papermetrics.h) a word's room is its printed width, and a screen face
   is often wider -- Geneva against Helvetica's widths -- so drawn whole
   it runs into the space after it and words touch. wp_dev_ink_twips is
   the screen face's own width of the text (paper metrics off for the
   measure), or -1 when paper metrics are off and the layout's width is
   already the face's. wp_dev_text_fit draws as wp_dev_text, except that
   on a platform gc a word whose ink_px is wider than its w is drawn in
   pieces, one more than the pixels it overruns by (at most a character
   each), each at its share of w: where the face's own advance up to it
   falls, scaled into w. A word that fits is drawn whole, as is one
   character. */
i32 wp_dev_ink_twips(plat_font *f, const u8 *utf8, u32 len);
wp_status wp_dev_text_fit(plat_gc *gc, plat_font *f, const wp_document *doc, const wp_char_style *cs,
                          i32 x, i32 y, i32 w, i32 h, i32 ink_px, const u8 *utf8, u32 len,
                          u8 r, u8 g, u8 b);
wp_status wp_dev_fill_rect(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, u8 r, u8 g, u8 b);
wp_status wp_dev_fill_shade(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, u8 percent);
wp_status wp_dev_fill_polygon(plat_gc *gc, const i32 *xy, u16 n, u8 r, u8 g, u8 b);
wp_status wp_dev_draw_polyline(plat_gc *gc, const i32 *xy, u16 n, i32 width, u8 r, u8 g, u8 b);
wp_status wp_dev_draw_mono(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, const u16 *rows, u32 row_words,
                           u8 r, u8 g, u8 b);

/* A drawing's text, as the dtext op above; `size_twips` is its height
   as a font size, for the platform's own fonts. The platform draws it
   level, or for a turned line each letter upright along it -- no
   target here can turn a glyph -- where a device of its own (PostScript)
   turns the text itself. */
wp_status wp_dev_draw_text(plat_gc *gc, i32 x, i32 y, i32 height, i32 size_twips, u16 angle,
                           u8 align, u8 style, u8 family, const u8 *rgb, const u8 *utf8, u16 len);

#endif /* WP_DEVOUT_H */
