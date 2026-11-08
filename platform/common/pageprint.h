/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PAGEPRINT_H
#define WP_PAGEPRINT_H

#include "types.h"
#include "docmodel.h"
#include "platform.h"

/* The document drawn page by page onto a printer's own drawing surface
   -- the ST's GDOS printer workstation, and any other machine whose
   printing is drawing (a Mac's printer port, an Amiga's graphics
   printing) -- with the fonts, sizes and styles the screen shows, where
   format_prn can only send a character printer its fixed-pitch grid.

   It lays the document out again first, at the full text column, so the
   caller must already have pointed font measuring at the printer: line
   breaks measured with the screen's fonts would be the wrong length on
   paper (the ST sizes its screen fonts to GEM's own system font, not to
   points). Every layout is marked dirty again afterwards, so the next
   paint lays the document out for the screen once the caller has pointed
   measuring back at it.

   Each page: the section's header and footer in the middle of its top
   and bottom margins ('|' parts left, centre and right, '#' the page
   number, as format_prn and the PDF print them), then its lines where
   the layout put them -- alignment, indents, tabs, justified gaps --
   in black. A picture prints as its frame.

   platform/common rules apply: engine headers and platform.h only. */

typedef i32 (*wp_page_px_fn)(i32 twips);

typedef struct {
    plat_gc *gc;
    wp_page_px_fn x_px;         /* twips across the paper -> device pixels */
    wp_page_px_fn y_px;         /* twips down the paper -> device pixels */
    /* The printable area in device pixels, 0 for the whole paper. A
       printer cannot reach its paper's edges and its pixel 0 is the
       printable area's corner, so the paper is centred on it: a margin
       measured from the paper's edge lands there on the page. */
    i32 device_w_px;
    i32 device_h_px;
    /* Prints the page drawn so far and starts a clean one. */
    wp_status (*end_page)(void *user);
    void *user;
} wp_page_device;

/* Draws every page of doc onto dev. *pages_out (may be NULL) is the
   number printed -- 0 for a document with no lines, which draws nothing
   and ends no page. A failing end_page stops the printout and is
   returned. */
wp_status wp_pageprint(wp_document *doc, const wp_page_device *dev, u32 *pages_out);

#endif /* WP_PAGEPRINT_H */
