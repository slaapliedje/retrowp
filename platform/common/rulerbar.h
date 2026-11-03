/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_RULERBAR_H
#define WP_RULERBAR_H

#include "types.h"
#include "platform.h"
#include "actions.h"

/* The ruler bar: a row of icon buttons over a measured ruler, the way
   Atari Works puts its formatting controls on the ruler. One model for
   every shell, drawn with the two primitives every one of them already
   has (plat_fill_rect, plat_draw_text), so a shell only reserves the
   strip, calls wp_rulerbar_paint, and routes a click through
   wp_rulerbar_hit_test.

     [new][open][save] [cut][copy][paste][undo] [B][I][U]
     [left][centre][right][justify] [1][1.5][2] [<-][->]
     |....:....1....:....2....:....3....:....4....:....5....:....6....:..|
      v first-line indent   ^ left indent   L tab   ^ right indent      ]

   The buttons show state: bold/italic/underline as the input style has
   them, the caret paragraph's alignment and line spacing. The ruler
   shows that paragraph's indents and tab stops against the text column
   it was laid out in, from the column's left edge (where every line's
   x_twips is measured from) to its wrap width.

   Pixel shells draw 16x16 icons (or 16x8 on a screen whose pixels are
   twice as tall as wide -- the ST's medium resolution -- by folding row
   pairs); DOS draws three-cell text buttons ("[B]") and a text ruler,
   with x and y in cells. Which one is chosen by icon_rows below.

   platform/common rules apply: engine headers and platform.h only. */

typedef i32 (*wp_rulerbar_conv_fn)(i32 v);

typedef struct {
    i32 x, y, w;            /* where the bar goes (pixels; cells on DOS) */
    i32 button_w, button_h; /* one button's cell */
    i32 icon_rows;          /* 16, 8 (rows folded in pairs), or 0 = text buttons */
    i32 scale_h;            /* the ruler strip under the buttons */
    i32 origin_x;           /* screen x of the text column's left edge */
    wp_rulerbar_conv_fn twips_to_x;   /* the shell's own x-axis conversions */
    wp_rulerbar_conv_fn x_to_twips;
} wp_rulerbar;

/* The bar's whole height: the button row, a gap, and the ruler. */
i32 wp_rulerbar_height(const wp_rulerbar *rb);

/* Paints the whole bar. font/ascent are for the ruler's inch numbers
   and DOS's labels (ascent in the bar's own vertical unit: pixels, or
   0 on DOS). */
void wp_rulerbar_paint(plat_gc *gc, plat_font *font, i32 ascent,
                       const wp_rulerbar *rb, struct wp_app_ctx *ctx);

typedef enum {
    WP_RULERBAR_MISS = 0,
    WP_RULERBAR_BUTTON,     /* .action: invoke it the way a menu pick is */
    WP_RULERBAR_RULER       /* .twips: from the column's left edge */
} wp_rulerbar_hit_kind;

typedef struct {
    u8           kind;      /* wp_rulerbar_hit_kind */
    wp_action_id action;
    i32          twips;
} wp_rulerbar_hit;

/* What is at (x, y): a button, a place on the ruler, or nothing. */
wp_bool wp_rulerbar_hit_test(const wp_rulerbar *rb, i32 x, i32 y, wp_rulerbar_hit *out);

/* A fingerprint of everything the bar shows: the input style, the
   caret paragraph's alignment, spacing, indents and tabs, the width it
   wraps at, and the bar's own geometry. Painting the bar is hundreds of
   rectangle fills -- a visible pause on an 8 MHz machine if it happened
   on every keystroke -- so a shell repaints it only when this changes,
   or when the window system says the area was damaged. */
u32 wp_rulerbar_signature(const wp_rulerbar *rb, struct wp_app_ctx *ctx);

/* WP_TRUE if the bar has a button for `id` -- an action reachable this
   way needs no menu or shortcut to be usable. */
wp_bool wp_rulerbar_has_action(wp_action_id id);

#endif /* WP_RULERBAR_H */
