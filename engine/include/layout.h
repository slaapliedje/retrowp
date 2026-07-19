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

#endif /* WP_LAYOUT_H */
