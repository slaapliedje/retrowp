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

/* Same shape as run_insert_at/run_remove_at, one tree level up. Unlike a
   run (a POD -- span + style handle, owns no allocation), a removed
   paragraph owns its runs array and layout cache, so paragraph_remove_at
   must free both before shifting; a run never needs that. Inserts an
   empty (zeroed) paragraph -- the caller fills in its runs/style
   afterward; this only handles the array splice. */
wp_status paragraph_insert_at(wp_section *sec, u32 index, u32 *out_index);
void      paragraph_remove_at(wp_section *sec, u32 index);

/* The (run, offset) pair for "the very end of this paragraph's existing
   content" -- (run_count - 1, that run's length), or (0, 0) if the
   paragraph has no runs. The same canonical form wp_pos_next/prev's own
   end_of_paragraph (cursor.c) and insert's "end of run" validation
   branch both use -- see docs/editing.md. Shared by
   wp_document_merge_paragraph_raw, wp_document_extract_text, and
   wp_edit_delete_selection, all of which need "where does paragraph X's
   content end" as a plain (run, offset) pair rather than a full wp_pos. */
void paragraph_end_run_offset(const wp_paragraph *para, u32 *out_run, u32 *out_offset);

#endif /* WP_EDIT_INTERNAL_H */
