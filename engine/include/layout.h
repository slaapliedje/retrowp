/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_LAYOUT_H
#define WP_LAYOUT_H

#include "types.h"
#include "docmodel.h"

/* Word wrap and pagination, reached ONLY through platform.h's font
   metrics seam (CLAUDE.md rule 4) -- never a platform font API
   directly. Full design rationale (tokenization across run boundaries,
   first-line indent handling, why "minimal recompute" only applies to
   line breaking and not page-filling) is in docs/layout.md. */

typedef struct {
    u32 start_run;    /* line's content starts here (inclusive) */
    u32 start_offset;
    u32 end_run;       /* line's content ends here (exclusive) */
    u32 end_offset;
    i32 width_twips;   /* measured width of this line's content */
    i32 height_twips;  /* max (ascent+descent+leading) across the line */
} wp_line;

typedef struct {
    wp_line *lines;
    u32      line_count;
    u32      line_capacity;
} wp_paragraph_layout;

/* Recomputes para->layout via greedy word wrap at content_width_twips
   (the section's usable column width; per-line width is further
   narrowed by the paragraph's own indent_left/indent_right/indent_first
   -- see docs/layout.md). Allocates para->layout on first use, reuses
   and regrows it on subsequent calls. Clears para->layout_dirty on
   success. Safe to call unconditionally -- callers wanting the "only
   recompute if needed" behavior check layout_dirty themselves (see
   wp_document_paginate). */
wp_status wp_paragraph_relayout(wp_document *doc, wp_paragraph *para,
                                 i32 content_width_twips);

/* Frees para->layout (the wp_paragraph_layout and its lines array) and
   sets it back to NULL. Safe to call on an already-NULL layout. Called
   by wp_document_free -- not normally needed directly. */
void wp_paragraph_layout_free(wp_paragraph *para);


/* ------------------------------------------------------------------ */
/* Pagination (T3.2)                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 section;
    u32 paragraph;
    u32 line_index;   /* index into that paragraph's wp_paragraph_layout */
} wp_page_line_ref;

typedef struct {
    wp_page_line_ref *lines;
    u32               line_count;
    u32               line_capacity;
} wp_page;

typedef struct {
    wp_page *pages;
    u32      page_count;
    u32      page_capacity;
} wp_pagination;

/* Relayouts every paragraph whose layout_dirty is true (or whose layout
   is NULL), reusing every other paragraph's existing line list
   untouched, then fills lines into pages per section's page size and
   margins. A new section always starts a new page. *out is
   overwritten -- caller owns it and must eventually call
   wp_pagination_free. */
wp_status wp_document_paginate(wp_document *doc, wp_pagination *out);

void wp_pagination_free(wp_pagination *pagination);


/* ------------------------------------------------------------------ */
/* Justify rendering support (T13.2)                                   */
/* ------------------------------------------------------------------ */

/* wp_line stores only a byte range plus one aggregate measured width --
   enough for LEFT/CENTER/RIGHT (a single paint-time x offset), but
   JUSTIFY needs per-word draw positions. Rather than have every
   backend re-derive word boundaries from scratch, this shared,
   host-testable pair does it once: wp_line_next_fragment walks a
   line's run range yielding maximal same-class (space vs non-space)
   byte ranges -- split at both run boundaries (so a backend can switch
   font per style change, fixing the "whole line drawn in the last
   run's font" bug that motivated this) and space/non-space transitions
   (mirroring linebreak.c's tokenize() class-merge rule: a run of
   consecutive spaces is ONE gap, never one gap per raw space byte).
   wp_justify_space_extra_twips uses it internally to count a line's
   interior gaps and compute the per-gap slack; a backend adds this
   amount to its running x position after every space fragment, needing
   no alignment/last-line logic of its own -- the function already
   returns 0 whenever it wouldn't apply. */

typedef struct {
    u32     run_index;
    u32     byte_start;
    u32     byte_end;
    wp_bool is_space;
} wp_line_fragment;

typedef struct {
    u32     run_index;
    u32     byte_offset;
    wp_bool finished;
} wp_line_fragment_iter;

void wp_line_fragment_iter_init(wp_line_fragment_iter *iter, const wp_line *line);

wp_bool wp_line_next_fragment(wp_document *doc, wp_paragraph *para, const wp_line *line,
                               wp_line_fragment_iter *iter, wp_line_fragment *out);

/* 0 unless align is WP_ALIGN_JUSTIFY, the line is not its paragraph's
   last (matching PDF export's own left-align-last-line convention),
   and it has at least one interior gap; otherwise
   (target_width_twips - line->width_twips) / gap_count. */
i32 wp_justify_space_extra_twips(wp_document *doc, wp_paragraph *para, const wp_line *line,
                                  u8 align, wp_bool is_last_line_of_paragraph,
                                  i32 target_width_twips);


/* ------------------------------------------------------------------ */
/* Cursor positioning support (T157)                                   */
/* ------------------------------------------------------------------ */

/* Width in twips of the part of `line` that lies BEFORE (cursor_run,
   cursor_offset) -- i.e. how far right of the line's own drawing origin
   a caret marker belongs. Add it to whatever x the caller would start
   painting that line at (left margin + this line's alignment offset)
   and the caret lands on the glyph boundary the cursor actually sits at.
   space_extra_twips is the same value wp_justify_space_extra_twips
   returned for this line, so a justified line's stretched gaps are
   accounted for; pass 0 if the caller isn't justifying.

   Exists for the same reason wp_justify_space_extra_twips does: every
   backend's paint loop needs it, and re-deriving a fragment walk per
   backend is how three of the four ended up painting the caret at a
   hardcoded left margin regardless of the cursor's real column (Mac and
   Amiga did exactly that; Atari painted no caret at all; only DOS got it
   right, and only because a fixed-cell font makes the column a pure byte
   count with no measuring involved).

   Measures through plat_measure_text (CLAUDE.md rule 4), opening the
   run's font itself rather than borrowing the caller's -- a caller's
   cached font belongs to whatever run it last drew, which is not
   necessarily the cursor's run. Returns twips so the caller applies its
   own twips->px ratio; on a backend whose ratio is not exact this can
   differ by a pixel or so from summing per-fragment pixel widths, which
   is below the width of the caret itself.

   0 for an empty line, for an image paragraph, or for a cursor at the
   line's start; the full line content width for a cursor at or past its
   end (the common case while typing). Callers are expected to have
   already established that the cursor is on THIS line. */
i32 wp_line_cursor_x_twips(wp_document *doc, wp_paragraph *para, const wp_line *line,
                            u32 cursor_run, u32 cursor_offset, i32 space_extra_twips);

#endif /* WP_LAYOUT_H */
