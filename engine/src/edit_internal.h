#ifndef WP_EDIT_INTERNAL_H
#define WP_EDIT_INTERNAL_H

#include "docmodel.h"

/* Private to engine/src -- not installed under engine/include. Shared
   run-array splice primitives used by both edit.c (raw insert/delete)
   and undo.c (whose inverses are built from the exact same primitives --
   see docs/editing.md). Regular external linkage (not `static`, unlike
   growth_internal.h) since these do real work, not a one-line formula --
   duplicating them per translation unit isn't worth it. */

/* Inserts one new run at `index`, shifting runs at/after it right by
   one. Grows the array via the normal wp_paragraph_run_add growth path
   (append, then rotate into place). */
wp_status run_insert_at(wp_paragraph *para, u16 index, wp_span text, wp_cstyle style);

/* Removes the run at `index`, shifting runs after it left by one.
   Capacity is not shrunk. */
void run_remove_at(wp_paragraph *para, u16 index);

/* The structural core of wp_document_insert_text_raw, parameterized on
   an ALREADY-APPENDED pool span rather than raw bytes -- used directly
   by wp_document_insert_text_raw (after appending) and by
   wp_edit_redo (reusing the original insert's span, so redo never
   re-appends duplicate bytes into the pool). */
wp_status insert_span_at_pos(wp_document *doc, wp_pos pos, wp_span span, wp_pos *out_pos);

/* Finds the run in para whose span exactly matches `span` (pool offsets
   are never reused -- append-only -- so this is unambiguous) and
   returns its index, or para->run_count if not found. Used by
   wp_edit_undo to locate the run an insert created, without needing to
   re-derive its index via insert's state-dependent placement rule
   (which doesn't apply cleanly to the already-mutated, post-insert
   paragraph -- see docs/editing.md). */
u16 find_run_by_span(const wp_paragraph *para, wp_span span);

#endif /* WP_EDIT_INTERNAL_H */
