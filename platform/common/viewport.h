/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_VIEWPORT_H
#define WP_VIEWPORT_H

#include "types.h"
#include "docmodel.h"

/* The scrolled view of a document: which display line is painted at the
   top, and enough about the backend's geometry to keep the caret in view
   and to move by a page. One lives in wp_app_ctx; every shell's paint
   loop starts from its top_line instead of from the document's first
   line, which is all "scrolling" is on a backend that repaints its lines
   from scratch anyway.

   Lines are DISPLAY lines -- the wp_line entries of every paragraph's
   layout, in document order, an image paragraph's single line included
   -- flattened into one index space. A paragraph without a layout
   contributes none. That is exactly the walk every paint loop makes, so
   "line N" means the same thing to the viewport and to the painter.

   The position is a line index rather than a pixel offset on purpose:
   the painter advances y by one converted line height at a time, so a
   pixel offset would have to reproduce its rounding exactly to stay in
   step with it. Heights go through the backend's own line_px (the same
   conversion its paint loop advances y with), so a view of view_h_px
   holds exactly the lines the painter fits into it. On DOS a line is one
   row, line_px returns 1 and view_h_px is a row count.

   Two things move the view. Following the caret -- after every keystroke
   and every action, the painter asks for the least scroll that brings
   the caret's line fully into view. Scrolling by hand -- a slider, an
   arrow, a wheel -- sets `manual`, which holds the view where the writer
   put it even though the caret may now be off-screen; the next keystroke
   clears it (wp_keymap_feed_key does that) and the view snaps back to
   the caret. Both are the behaviour of every editor the writer has used.

   platform/common rules apply: engine headers and platform.h only, never
   a native header. */

typedef i32 (*wp_viewport_line_px_fn)(i32 height_twips);

typedef struct {
    u32 top_line;                 /* first display line in view */
    i32 view_h_px;                /* height of the document area, in the backend's
                                     vertical unit; recorded by the painter, since
                                     only it knows (0 until the first paint) */
    wp_viewport_line_px_fn line_px;
    wp_bool manual;               /* scrolled by hand: hold until the next keystroke */
} wp_viewport;

/* line_px may be NULL until the backend has one; the view then follows
   by line index alone (as if every line were one row). */
void wp_viewport_init(wp_viewport *vp, wp_viewport_line_px_fn line_px);

/* A new document: back to the top, no manual hold. Keeps line_px. */
void wp_viewport_reset(wp_viewport *vp);

/* Display-line index of the line the cursor sits on, choosing the
   paragraph's LAST line when the cursor is past every line's range
   (where it lands after typing at the paragraph's end) -- the same rule
   each paint loop uses to place the caret. *total_out (may be NULL) gets
   the document's display-line count. 0 when the cursor's paragraph has
   no layout. */
u32 wp_viewport_cursor_line(wp_document *doc, const wp_pos *cursor, u32 *total_out);

/* Where display line `index` is. WP_FALSE past the end. */
wp_bool wp_viewport_line_at(wp_document *doc, u32 index,
                            u32 *section_out, u32 *paragraph_out, u32 *line_out);

/* Moves top_line the least that brings the cursor's line fully into
   view, and keeps it inside the document (a document that just got
   shorter). Returns WP_TRUE if it changed, i.e. the whole view needs
   repainting, not just the caret's line. Needs the layouts current.
   Leaves a manual hold alone. */
wp_bool wp_viewport_follow_cursor(wp_viewport *vp, wp_document *doc, const wp_pos *cursor);

/* Whole display lines that fit from top_line down; at least 1. */
u32 wp_viewport_page_lines(const wp_viewport *vp, wp_document *doc);

/* The largest top_line that still fills the view, so the writer cannot
   scroll past the end into blank space. 0 for a document that fits. */
u32 wp_viewport_max_top(const wp_viewport *vp, wp_document *doc);

/* Page Up/Down: moves the view AND the cursor by a page, so the writer
   sees the next screenful with the caret where it was in the old one.
   The cursor lands at the start of its new line. Clears manual. */
void wp_viewport_page(wp_viewport *vp, wp_document *doc, wp_pos *cursor, wp_bool down);

/* Scrolling by hand: by delta lines (negative is up), or to a top line.
   Both clamp to [0, max_top] and set manual. */
void wp_viewport_scroll_by(wp_viewport *vp, wp_document *doc, i32 delta_lines);
void wp_viewport_scroll_to(wp_viewport *vp, wp_document *doc, u32 top_line);

#endif /* WP_VIEWPORT_H */
