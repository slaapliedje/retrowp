/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PAGEVIEW_H
#define WP_PAGEVIEW_H

#include "types.h"
#include "docmodel.h"
#include "platform.h"

/* The page, as the screen shows it: what width text wraps at, where the
   pages break, and which page the caret is on. One implementation for
   every shell -- each had its own copy of the relayout loop, all four
   wrapping at the window's width and none knowing the page existed.

   Text wraps at the page's text column (page width less both margins,
   wp_section_column_twips) -- unless the window is narrower, and then at
   the window: Word's draft view. A 6.5-inch column is 624 pixels at 96
   dpi, so it fits the Mac, the Amiga's hires screen, DOS's 80 columns
   and the ST in monochrome; the ST's medium resolution reports 150 dpi
   across, where the column would be 975 pixels, and keeps wrapping at
   the window as it always has. Horizontal scrolling to show the whole
   column there is a later view option.

   platform/common rules apply: engine headers and platform.h only. */

/* The width a section's paragraphs wrap at for a window this wide
   (twips; 0 or less means "no window yet", and gives the column). */
i32 wp_pageview_wrap_twips(const wp_document *doc, u32 section, i32 window_twips);

/* Lays out every paragraph whose layout is missing, dirty, or was
   broken at a different width than its section now wraps at, then
   marks where the pages break (wp_document_mark_pages). The relayout
   every shell runs before it paints. Returns the page count. */
u32 wp_pageview_relayout(wp_document *doc, i32 window_twips);

/* The 1-based page the cursor's line is on, and (page_count_out, may be
   NULL) how many pages the document has. Needs the layouts current and
   the pages marked (wp_pageview_relayout). 1 and 1 for an empty
   document. */
u32 wp_pageview_cursor_page(wp_document *doc, const wp_pos *cursor, u32 *page_count_out);

/* A page break as the screen shows it: a dashed rule across the text
   area, w pixels from x, at y (the top of the line that starts the new
   page; it sits in that line's own top pixel row, so nothing moves).
   For the shells that draw in pixels; DOS shows the page in its status
   line instead. */
void wp_pageview_draw_break(plat_gc *gc, i32 x, i32 y, i32 w);

#endif /* WP_PAGEVIEW_H */
