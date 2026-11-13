/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PAGECANVAS_H
#define WP_PAGECANVAS_H

#include "types.h"
#include "docmodel.h"
#include "platform.h"
#include "pageprint.h"

/* Page view (M19): the document as its pages, each whole at its size,
   stacked down a scrolling canvas with a gap between, the way it will
   print -- because it is drawn by the page printer itself
   (wp_pageprint_page), the screen one more device for the code that
   draws paper. NeXT's WYSIWYG came from drawing the screen with the
   printer's imaging model; this is the same idea without a PostScript
   interpreter. See docs/document-model.md, "Page view".

   Everything here is in CANVAS pixels: (0,0) the canvas's top-left, a
   page's corner at wp_pagecanvas_page_rect. A shell draws the canvas
   with its window's content corner at (win_x, win_y) and scrolled by
   (scroll_x, scroll_y): a canvas point lands at
   win_x + cx - scroll_x, win_y + cy - scroll_y.

   The layouts must be current at the page's column -- wp_pageview_relayout
   (doc, 0) -- before wp_pagecanvas_update, which reads where the pages
   fall. platform/common rules apply: engine headers and platform.h only. */

#define WP_ZOOM_FIT_WIDTH 0   /* the widest page fills the window's width */
#define WP_ZOOM_FIXED     1   /* zoom as set */
#define WP_ZOOM_FIT_PAGE  2   /* a whole page fits the window */

#define WP_PAGECANVAS_GAP_PX 8   /* desk showing around and between pages */

typedef struct {
    u32 si, pi, li;   /* the page's first line */
    u32 section;
    i32 top;          /* canvas pixels */
    i32 w, h;         /* the paper, canvas pixels */
} wp_pagecanvas_page;

typedef struct {
    wp_page_px_fn x_px, y_px;   /* the screen's twips -> pixels at 100% */
    wp_page_px_fn x_tw, y_tw;   /* and back */
    u8  fit;                    /* WP_ZOOM_* */
    i32 zoom;                   /* percent in force (a fit sets it) */
    i32 set_zoom;               /* percent for WP_ZOOM_FIXED */
    i32 view_w, view_h;         /* the window's content, pixels */
    i32 scroll_x, scroll_y;     /* canvas pixels at the view's corner */
    i32 canvas_w, canvas_h;
    wp_pagecanvas_page *pages;
    u32 page_count, page_cap;
    /* The layout grid (the user, 2026-11-12: DTP belongs in the word
       processor): drawn on the page from its margins' corner, and what
       a snapped position rounds to. */
    wp_bool grid_on;
    wp_bool snap_on;
    i32 grid_twips;
} wp_pagecanvas;

void wp_pagecanvas_init(wp_pagecanvas *c, wp_page_px_fn x_px, wp_page_px_fn y_px,
                        wp_page_px_fn x_tw, wp_page_px_fn y_tw);
void wp_pagecanvas_free(wp_pagecanvas *c);

/* Where the pages fall, the zoom a fit asks for, the canvas's size, the
   scroll kept inside it. After every layout and every window resize. */
wp_status wp_pagecanvas_update(wp_pagecanvas *c, wp_document *doc, i32 view_w, i32 view_h);

/* Twips on the paper -> canvas pixels at the zoom, and back. */
i32 wp_pagecanvas_zx(const wp_pagecanvas *c, i32 twips);
i32 wp_pagecanvas_zy(const wp_pagecanvas *c, i32 twips);
i32 wp_pagecanvas_tx(const wp_pagecanvas *c, i32 px);
i32 wp_pagecanvas_ty(const wp_pagecanvas *c, i32 px);

/* Page `page`'s paper on the canvas; WP_FALSE past the last page. */
wp_bool wp_pagecanvas_page_rect(const wp_pagecanvas *c, u32 page, i32 *x, i32 *y, i32 *w, i32 *h);

/* The canvas drawn into the window rectangle (clip_*, window pixels):
   the desk around the pages, each page's paper, its margins' guide, the
   grid when it is on, then the page as it prints. The caret and the
   selection are the shell's to draw over it (wp_pagecanvas_caret,
   wp_pagecanvas_selection), in its own way of inverting. */
void wp_pagecanvas_paint(const wp_pagecanvas *c, wp_document *doc, plat_gc *gc,
                         i32 win_x, i32 win_y, i32 clip_x, i32 clip_y, i32 clip_w, i32 clip_h);

/* A click at canvas (cx, cy) as a document position: the page under it
   (or nearest), the line on it nearest the click, the place on that
   line nearest its x. WP_FALSE for a document with no lines. */
wp_bool wp_pagecanvas_hit(const wp_pagecanvas *c, wp_document *doc, i32 cx, i32 cy, wp_pos *out);

/* Where the caret for `pos` goes: canvas x, the top and height of its
   line's text. WP_FALSE when pos is on no line. */
wp_bool wp_pagecanvas_caret(const wp_pagecanvas *c, wp_document *doc, const wp_pos *pos,
                            i32 *cx, i32 *cy, i32 *ch);

/* Each line's selected strip, as a canvas rectangle, handed to `fn`. */
typedef void (*wp_pagecanvas_rect_fn)(void *user, i32 cx, i32 cy, i32 w, i32 h);
void wp_pagecanvas_selection(const wp_pagecanvas *c, wp_document *doc, wp_selection sel,
                             wp_pagecanvas_rect_fn fn, void *user);

/* Scrolls the least that brings the caret for `pos` into view; WP_TRUE
   when it moved (the view needs drawing whole). */
wp_bool wp_pagecanvas_follow(wp_pagecanvas *c, wp_document *doc, const wp_pos *pos);

/* Scrolls by (dx, dy) pixels, or to (x, y), kept inside the canvas. */
void wp_pagecanvas_scroll_by(wp_pagecanvas *c, i32 dx, i32 dy);
void wp_pagecanvas_scroll_to(wp_pagecanvas *c, i32 x, i32 y);

/* A length from the margins' corner rounded to the grid, when the grid
   and snapping are on; unchanged otherwise. */
i32 wp_pagecanvas_snap(const wp_pagecanvas *c, i32 twips);

#endif /* WP_PAGECANVAS_H */
