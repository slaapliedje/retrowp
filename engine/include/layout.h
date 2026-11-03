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

/* One laid-out line, with everything a painter needs to place it -- so
   that every backend draws the same geometry instead of re-deriving it
   (each used to: none drew indents, and all four measured alignment and
   justify against the full column even when an indent had narrowed the
   line). Coordinates are twips: x from the left edge of the text column,
   y from the top of the line's own box.

     +---------------------------------------- box top (y advances by height_twips)
     |  text_top_twips   (space_before, first line only)
     +--- text top ---------------------------
     |  text_height_twips (the line's own ascent+descent+leading;
     |                     caret and selection span this)
     +----------------------------------------
     |  line spacing's extra, then space_after on the last line
     +---------------------------------------- box bottom

   x_twips already includes the paragraph's indent (left, plus first-line
   on line 0) and its alignment within avail_twips; avail_twips is the
   width the line was broken to, which is also justify's target. */
typedef struct {
    u32 start_run;    /* line's content starts here (inclusive) */
    u32 start_offset;
    u32 end_run;       /* line's content ends here (exclusive) */
    u32 end_offset;
    i32 width_twips;   /* measured width of this line's content, tabs included */
    i32 height_twips;  /* the whole box: spacing above and below included */
    i32 x_twips;            /* where the content starts, from the column's left edge */
    i32 avail_twips;        /* width available to this line (justify's target) */
    i32 text_top_twips;     /* box top to text top */
    i32 text_height_twips;  /* the text's own height */
    u32 first_tab;          /* index into the layout's tab_widths of this line's first tab */
    u32 tab_count;          /* tabs on this line */
    u32 page;               /* 0-based page the line falls on (wp_document_mark_pages) */
    wp_bool page_start;     /* first line of its page */
} wp_line;

typedef struct {
    wp_line *lines;
    u32      line_count;
    u32      line_capacity;
    i32     *tab_widths;    /* the advance of every tab in the paragraph, in text order */
    u32      tab_count;
    u32      tab_capacity;
    i32      wrap_twips;    /* the column width this layout was broken at */
} wp_paragraph_layout;

/* The tab stop a tab starting at x_twips (from the column's left edge)
   advances to, and its kind (wp_tab_kind): the first explicit stop past
   x, else the next multiple of WP_DEFAULT_TAB_TWIPS. pstyle may be NULL
   (default stops only). Shared with the ruler, which draws the same
   stops the layout uses. */
i32 wp_tab_next_stop(const wp_para_style *pstyle, i32 x_twips, u8 *kind_out);

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

/* The same page-filling rule as wp_document_paginate, but nothing
   allocated: it writes each line's page and page_start in place, for a
   screen that draws page breaks and a status line that says which page
   the caret is on. Uses the layouts as they are (lay them out first);
   a paragraph with no layout contributes no lines. Returns the page
   count (at least 1). */
u32 wp_document_mark_pages(wp_document *doc);

/* The text column of a section's page: page width less both margins
   (never less than an inch, so a nonsense page setup still lays out). */
i32 wp_section_column_twips(const wp_section *sec);


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

/* A tab is a fragment of its own (is_tab): never drawn, never measured
   by the painter -- it advances x by tab_twips, which the layout already
   worked out from the stops. A painter that measured "\t" as a glyph
   would draw it at whatever width the font gives a control character. */
typedef struct {
    u32     run_index;
    u32     byte_start;
    u32     byte_end;
    wp_bool is_space;
    wp_bool is_tab;
    i32     tab_twips;   /* meaningful only when is_tab */
} wp_line_fragment;

typedef struct {
    u32     run_index;
    u32     byte_offset;
    wp_bool finished;
    u32     tab_index;   /* next tab's index into the layout's tab_widths */
} wp_line_fragment_iter;

void wp_line_fragment_iter_init(wp_line_fragment_iter *iter, const wp_line *line);

wp_bool wp_line_next_fragment(wp_document *doc, wp_paragraph *para, const wp_line *line,
                               wp_line_fragment_iter *iter, wp_line_fragment *out);

/* 0 unless align is WP_ALIGN_JUSTIFY, the line is not its paragraph's
   last (matching PDF export's own left-align-last-line convention),
   it has no tab (stretching gaps would pull tabbed columns out of their
   stops), and it has at least one interior gap; otherwise
   (target_width_twips - line->width_twips) / gap_count. Pass
   line->avail_twips as the target. */
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

/* The inverse, for a mouse click: the (run, offset) on `line` nearest
   to x_twips measured from the line's own drawing origin (the same
   origin wp_line_cursor_x_twips is relative to), with the same
   space_extra_twips. A click left of the line gives its start, right of
   it its end, inside a glyph the nearer of its two edges. Measures
   through plat_measure_text like its counterpart. Always sets both outs;
   returns WP_TRUE (the result is never undefined). */
wp_bool wp_line_pos_at_x_twips(wp_document *doc, wp_paragraph *para, const wp_line *line,
                               i32 x_twips, i32 space_extra_twips,
                               u32 *run_out, u32 *offset_out);

#endif /* WP_LAYOUT_H */
