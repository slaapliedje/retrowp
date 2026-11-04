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
    i32 colors;             /* the screen's colours: from 16 up the bar is a
                               grey panel of raised buttons (a pressed one
                               sunken) with the text column white on the
                               ruler; below, black line art on white, which
                               is all two or four colours can show (ST
                               Medium's four are white, red, green, black) */
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

/* Dragging on the ruler. A press picks up the marker under it -- the
   first-line indent (the upper triangle), the left or right indent (the
   lower ones), or a tab stop -- or, on bare ruler, a new tab stop. The
   shell then feeds every mouse position to _move while the button is
   down, repainting the ruler strip with wp_rulerbar_paint_scale (which
   draws the marker where it is being dragged to), and calls _end on
   release:
     - an indent marker sets that indent, snapped to an eighth of an
       inch; the first line keeps its place when the left indent moves,
       as on every ruler since Word 1.0;
     - a tab stop moves, keeping its kind, or is removed if it was
       dragged off the ruler, up or down;
     - a press that never moved is a click: it sets or clears a tab stop
       there, as a click always has.
   On DOS's one-row ruler a first-line and left indent in the same cell
   are picked up together and move together. */
typedef enum {
    WP_RB_DRAG_NONE = 0,
    WP_RB_DRAG_FIRST,       /* first-line indent */
    WP_RB_DRAG_LEFT,        /* left indent; the first line stays put */
    WP_RB_DRAG_BOTH,        /* both together (DOS, when they share a cell) */
    WP_RB_DRAG_RIGHT,       /* right indent */
    WP_RB_DRAG_TAB,         /* an existing tab stop */
    WP_RB_DRAG_NEWTAB       /* bare ruler: a new stop, or a click */
} wp_rulerbar_drag_kind;

typedef struct {
    u8      kind;           /* wp_rulerbar_drag_kind */
    wp_bool moved;          /* left the press position */
    wp_bool off;            /* pulled off the ruler (a tab: removed) */
    i32     press_twips;    /* where the press was, unsnapped */
    i32     from_twips;     /* the marker's own position when picked up */
    i32     twips;          /* where it is now, snapped */
    i32     wrap;           /* the column width the ruler shows */
    i32     press_x, press_y;
} wp_rulerbar_drag;

/* WP_TRUE when (x, y) is on the ruler strip; *d is then set up. */
wp_bool wp_rulerbar_drag_begin(const wp_rulerbar *rb, struct wp_app_ctx *ctx,
                               i32 x, i32 y, wp_rulerbar_drag *d);

/* The mouse is at (x, y) with the button still down. WP_TRUE when what
   the ruler shows changed: repaint the strip. */
wp_bool wp_rulerbar_drag_move(const wp_rulerbar *rb, wp_rulerbar_drag *d, i32 x, i32 y);

/* The button is up: apply the drag (or the click). */
wp_status wp_rulerbar_drag_end(struct wp_app_ctx *ctx, wp_rulerbar_drag *d);

/* The ruler strip alone -- the scale under the buttons -- cleared and
   repainted, with `d`'s marker where it is being dragged to (d may be
   NULL). A drag repaints this on every move, never the whole bar: on an
   8 MHz ST the buttons are hundreds of fills. */
void wp_rulerbar_paint_scale(plat_gc *gc, plat_font *font, i32 ascent,
                             const wp_rulerbar *rb, struct wp_app_ctx *ctx,
                             const wp_rulerbar_drag *d);

/* The strip's rectangle, for a shell's clipping: top y and height. */
void wp_rulerbar_scale_rect(const wp_rulerbar *rb, i32 *y, i32 *h);

/* WP_TRUE if the bar has a button for `id` -- an action reachable this
   way needs no menu or shortcut to be usable. */
wp_bool wp_rulerbar_has_action(wp_action_id id);

#endif /* WP_RULERBAR_H */
