/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_SELSPAN_H
#define WP_SELSPAN_H

#include "types.h"
#include "docmodel.h"
#include "layout.h"

/* Which part of one display line a selection covers.

   A painter walks display lines; a selection is a pair of document
   positions in either order. This answers, for one line, "from where to
   where on this line is selected", as two positions ON the line, so the
   painter can turn them into pixels with the same wp_line_cursor_x_twips
   it already places the caret with and invert the strip between. The
   same arithmetic for every backend, host-tested once, rather than four
   copies of the boundary cases (a selection that starts mid-line, ends
   mid-line, or sweeps whole lines of the paragraphs in between).

   Returns WP_FALSE when the line has no selected part. A line wholly
   inside the selection comes back as its own start and end; a selection
   that only touches the line's end (or an empty paragraph's one line)
   is nothing to paint and returns WP_FALSE. The positions carry the
   line's own section and paragraph. */
wp_bool wp_selspan_for_line(wp_selection sel, u32 section, u32 paragraph,
                            const wp_line *line, wp_pos *from_out, wp_pos *to_out);

#endif /* WP_SELSPAN_H */
