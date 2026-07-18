#ifndef WP_EDITING_H
#define WP_EDITING_H

#include "types.h"
#include "docmodel.h"

/* Cursor movement, insert/delete, and undo/redo. Full design rationale
   (style inheritance, single-run delete scope, why undo doesn't need to
   restore the exact original run layout, the wp_op field-reuse scheme)
   is in docs/editing.md -- read it before changing this file. */


/* ------------------------------------------------------------------ */
/* Cursor / position navigation (T2.1)                                 */
/* ------------------------------------------------------------------ */

wp_pos wp_pos_doc_start(const wp_document *doc);
wp_pos wp_pos_doc_end(const wp_document *doc);

/* Steps pos forward/backward by exactly one codepoint, crossing run,
   paragraph, and section boundaries as needed. Clamps at the start/end
   of the document -- repeated calls at a boundary keep returning the
   same position, never go out of range. Precondition: pos is a valid
   position in doc (as produced by these functions or wp_pos_doc_start/
   end); behavior is only defined for valid input. */
wp_pos wp_pos_next(const wp_document *doc, wp_pos pos);
wp_pos wp_pos_prev(const wp_document *doc, wp_pos pos);


/* ------------------------------------------------------------------ */
/* Raw insert/delete (T2.2) -- structural only, no undo journal        */
/* ------------------------------------------------------------------ */

/* Inserts utf8[0..len) at pos (always a point, never a range). Always
   creates exactly one new run for the inserted text (see
   docs/editing.md -- no run-coalescing). Style is inherited per the
   documented rule. *out_pos, if non-NULL, is set to the position
   immediately after the inserted text. */
wp_status wp_document_insert_text_raw(wp_document *doc, wp_pos pos,
                                       const u8 *utf8, u32 len,
                                       wp_pos *out_pos);

/* Deletes the byte range [start_offset, end_offset) within the single
   run identified by (section, paragraph, run_index). Returns WP_RANGE if
   start_offset/end_offset are not a valid, non-empty sub-range of that
   run. Deleting a selection spanning multiple runs or paragraphs is out
   of scope for this milestone (see docs/editing.md) -- callers needing
   that must loop this per-run themselves; there is no cross-run atomic
   delete here. */
wp_status wp_document_delete_range_raw(wp_document *doc, u32 section,
                                        u32 paragraph, u16 run_index,
                                        u32 start_offset, u32 end_offset);


/* ------------------------------------------------------------------ */
/* Undo/redo journal (T2.3)                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    wp_op *undo_top;  /* most recently applied op, NULL if none */
    wp_op *redo_top;  /* most recently undone op, NULL if none */
} wp_undo_journal;

void wp_undo_journal_init(wp_undo_journal *journal);
/* Frees every node on both stacks. Does not touch doc. */
void wp_undo_journal_free(wp_undo_journal *journal);

wp_bool wp_undo_can_undo(const wp_undo_journal *journal);
wp_bool wp_undo_can_redo(const wp_undo_journal *journal);

/* Journaled edit operations: apply the same structural edit as the raw
   functions above, and additionally push an undo record. Any pending
   redo history is discarded (a new edit invalidates it). */
wp_status wp_edit_insert_text(wp_document *doc, wp_undo_journal *journal,
                               wp_pos pos, const u8 *utf8, u32 len,
                               wp_pos *out_pos);
wp_status wp_edit_delete_range(wp_document *doc, wp_undo_journal *journal,
                                u32 section, u32 paragraph, u16 run_index,
                                u32 start_offset, u32 end_offset);

/* Undoes/redoes the most recent entry on the respective stack. WP_ERR if
   that stack is empty. */
wp_status wp_edit_undo(wp_document *doc, wp_undo_journal *journal);
wp_status wp_edit_redo(wp_document *doc, wp_undo_journal *journal);

#endif /* WP_EDITING_H */
