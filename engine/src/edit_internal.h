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

/* Splits the run at `index` so that `offset` bytes stay where they are
   and the remainder becomes a new run immediately after it, carrying the
   same style. A no-op (WP_OK) when `offset` is 0 or the run's whole
   length -- i.e. when the boundary already falls between runs -- so a
   caller can ask for a split at any byte without checking first.

   This is what makes styling a PART of a run possible: a character style
   belongs to a run, so "bold from here to there" means "make that stretch
   a run of its own first". */
wp_status run_split_at(wp_paragraph *para, u16 index, u32 offset);

/* The run containing byte `byte` of the paragraph's flattened text, and
   how far into that run it falls. `byte` == the paragraph's total length
   answers (run_count, 0) -- one past the end, which run_split_at then
   treats as the no-op it is. */
void run_at_byte(const wp_paragraph *para, u32 byte, u16 *out_index, u32 *out_offset);

/* The byte offset of `pos` within its own paragraph -- the inverse of
   pos_from_byte_in_paragraph below. A range walk needs this because
   splitting runs invalidates every run INDEX in the paragraph while
   leaving every byte offset exactly where it was. */
u32 para_byte_of_pos(const wp_paragraph *para, wp_pos pos);

/* Total bytes of a paragraph's flattened text. */
u32 para_total_bytes(const wp_paragraph *para);

/* The slice of `para` (identified by its own section/paragraph indices)
   that [start, end) covers, as a byte range. WP_FALSE when the paragraph
   lies outside the range entirely, or when the overlap is empty. `start`
   must not be after `end` -- callers normalize first. */
wp_bool range_in_paragraph(const wp_paragraph *para, u32 section, u32 paragraph,
                            wp_pos start, wp_pos end, u32 *out_from, u32 *out_to);

/* The (section, paragraph, run, offset) position exactly byte_offset
   bytes into a paragraph's flattened text (the inverse of summing run
   lengths) -- clamps to paragraph_end_run_offset's own result if
   byte_offset runs past the paragraph's content. Promoted (M11) from
   vimode.c's own file-local helper of the same name/shape, once
   search.c's wp_document_find_text/wp_document_match_selection needed
   the exact same "convert a byte offset found via a flattened-text scan
   back into a real wp_pos" conversion wp_vimode_search's paragraph scan
   already did. */
wp_pos pos_from_byte_in_paragraph(u32 section, u32 paragraph, const wp_paragraph *para,
                                   u32 byte_offset);

#endif /* WP_EDIT_INTERNAL_H */
