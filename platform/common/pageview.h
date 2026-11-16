/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PAGEVIEW_H
#define WP_PAGEVIEW_H

#include "types.h"
#include "docmodel.h"
#include "platform.h"
#include "layout.h"

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

/* WP_TRUE when the last wp_pageview_relayout could not lay out every
   paragraph -- out of memory, almost always. Such a paragraph keeps no
   lines (or its old ones) and is not drawn, so a shell that does not ask
   shows a blank or stale page and says nothing; it should tell the user
   once, not at every repaint. */
wp_bool wp_pageview_relayout_short(void);

/* WP_TRUE once when a relayout has newly come up short; then WP_FALSE
   until a relayout succeeds and a later one falls short again. A shell
   asks after it has handled an event and repainted -- never from inside
   a repaint, where a prompt would draw over the screen being painted --
   and tells the user WP_PAGEVIEW_SHORT_MSG. */
wp_bool wp_pageview_short_news(void);

/* 68 characters: it fits DOS's prompt row beside "(Enter=OK)". */
#define WP_PAGEVIEW_SHORT_MSG "Out of memory: part of the document can't be shown. Save your work."

/* How layout measures (papermetrics.h), chosen at each relayout.

   WP_METRICS_AS_SET, the default: left as plat_metrics_set_paper has it
   (the host tests, and any caller that sets it itself).

   WP_METRICS_SCREEN: always the screen's own faces.

   WP_METRICS_PAPER: the page's column (window_twips 0: page view and
   printing) as it will print; a window too, unless the screen's body
   face is plainly wider than the paper's -- the ST's 8x16 system font
   is about a fifth wider than Times at 12 point, and words held to
   their printed widths were squeezed together in normal view. There the
   screen's own face reads as an ST editor always has, and only page
   view and the printout keep the paper's line breaks. A narrower face
   (the Amiga's, the ST's in medium resolution) or a matching one (the
   Mac's) keeps paper metrics in normal view too, and its words are
   closed up (screenline.h).

   A change of mode lays every paragraph out again. */
#define WP_METRICS_AS_SET 0u
#define WP_METRICS_SCREEN 1u
#define WP_METRICS_PAPER  2u
void wp_pageview_metrics(u8 mode);

/* Whether the screen's body face is plainly wider than the paper's (the
   test WP_METRICS_PAPER makes, measured once a mode is set). */
wp_bool wp_pageview_screen_face_wider(void);

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

/* Where each display line goes down a page or screen, for a painter
   walking lines in order (docs/document-model.md, "Tables"): a line
   outside a table at the running y, which then moves past it; a table
   line at its row's top plus its offset in the row, the running y
   holding at the row's top until the row is done and then jumping to
   its bottom. Pixels throughout, through the painter's own conversion,
   and always as a sum of each line's converted height -- never a
   converted total, which rounds differently: DOS's 300-twip line is a
   row, but two of them, 600, round to three. A cell's lines stack from
   the row's top; the row ends at its lowest line. */
typedef i32 (*wp_line_walk_px_fn)(i32 twips);
typedef struct {
    i32     y;            /* where the next line outside a row starts */
    i32     row_top, row_bottom;
    i32     cell_y;       /* where the current cell's next line starts */
    wp_bool in_row;
    i32     page_top;     /* the top margin's line of the page walked last */
    i32     frame_y;      /* where the current frame's next line starts */
    wp_bool paper;        /* a page being printed: footnotes at its foot, not in
                             the flow (WP_LINE_NOTE), and each column from the top
                             (wp_line.column); wp_line_walk_start clears it */
    u8      column;       /* a printing walk's column */
} wp_line_walk;

void wp_line_walk_start(wp_line_walk *w, i32 y);
/* The line's top; moves past it. Call for every line drawn, in order. */
i32  wp_line_walk_next(wp_line_walk *w, const wp_line *line, wp_line_walk_px_fn px);

/* A walk for a view scrolled to display line `top_line`, its first line
   at y: as wp_line_walk_start, and where that line's page begins, so a
   frame whose own lines are scrolled off -- it follows text above the
   view -- is still placed. *page_out gets the page (~0 for none); a
   painter draws a scrolled-off frame line on that page, and only where
   it starts inside the view. */
void wp_line_walk_start_view(wp_line_walk *w, i32 y, wp_document *doc, u32 top_line,
                             wp_line_walk_px_fn px, u32 *page_out);
/* Where the lines end: past the last line, or a row it was in. */
i32  wp_line_walk_end(wp_line_walk *w);

/* A table row's height as the walker will find it: its tallest cell's
   converted line heights, summed. `para_index` is the row's first
   paragraph. For a painter clearing the row, or ruling its cells, at its
   first line. */
i32  wp_table_row_extent(const wp_section *sec, u32 para_index, wp_line_walk_px_fn px);

#endif /* WP_PAGEVIEW_H */
