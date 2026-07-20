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

/* Pure (section, paragraph, run, offset) lexicographic comparison -- a
   wp_pos's fields already form a total order by construction, no
   document lookup needed. <0 if a precedes b, 0 if equal, >0 if a
   follows b. Foundational for wp_selection: normalizing an (anchor,
   caret) pair into an ordered (start, end) range needs this. */
int wp_pos_compare(wp_pos a, wp_pos b);

/* Extracts the UTF-8 text of the tree range between sel.anchor and
   sel.caret (order-independent) into out (out_cap bytes), concatenating
   across run boundaries and joining paragraph boundaries with exactly
   one '\n' -- the same separator convention format_txt_export already
   uses, so a yanked range round-trips through a later insert correctly.
   Same two-call sizing convention as format_txt_export: *out_len is
   always set to the exact required size; WP_RANGE and nothing written
   if out_cap is too small. Read-only. Single-section only this pass
   (matches wp_document_split/merge_paragraph_raw's own scope) --
   WP_RANGE if anchor and caret are in different sections. */
wp_status wp_document_extract_text(const wp_document *doc, wp_selection sel,
                                    u8 *out, u32 out_cap, u32 *out_len);


/* ------------------------------------------------------------------ */
/* Word/line/paragraph motions (M8.5) -- one step per call, same        */
/* single-step-and-let-the-caller-loop convention as wp_pos_next/prev.  */
/* Operate on LOGICAL PARAGRAPHS, not visually-wrapped screen lines --  */
/* see docs/vimode.md for why. Vi-specific semantics (which motions are */
/* inclusive/exclusive/linewise, the dw-doesn't-eat-the-newline rule)   */
/* live in the vi-mode layer, not here -- these stay honest, mechanical */
/* "one motion" primitives. Clamp at document start/end like wp_pos_    */
/* next/prev; never go out of range.                                    */
/* ------------------------------------------------------------------ */

typedef enum {
    WP_WORDCLASS_BLANK = 0, WP_WORDCLASS_WORD = 1, WP_WORDCLASS_PUNCT = 2
} wp_wordclass;

/* ASCII alnum + underscore = WORD, whitespace = BLANK, everything else
   (punctuation) = PUNCT. Codepoints >= 128 are classified WORD (treated
   as ordinary text characters) -- a deliberate simplification, not a
   claim of correct locale-aware word-breaking; see docs/vimode.md. */
wp_wordclass wp_wordclass_of(u32 codepoint);

/* vim's w/b/e (big=WP_FALSE, three-way WORD/PUNCT/BLANK classes) or
   W/B/E (big=WP_TRUE, PUNCT merges into WORD -- blank-delimited only).
   Landing position uses this engine's own insertion-point caret
   convention throughout (see docs/vimode.md's Fork C note), not vim's
   on-character caret -- wp_pos_word_end lands immediately AFTER a
   word's last character, not on it. */
wp_pos wp_pos_word_forward(const wp_document *doc, wp_pos pos, wp_bool big);
wp_pos wp_pos_word_backward(const wp_document *doc, wp_pos pos, wp_bool big);
wp_pos wp_pos_word_end(const wp_document *doc, wp_pos pos, wp_bool big);

wp_pos wp_pos_line_start(const wp_document *doc, wp_pos pos);          /* 0 */
wp_pos wp_pos_line_first_nonblank(const wp_document *doc, wp_pos pos); /* ^ */
wp_pos wp_pos_line_end(const wp_document *doc, wp_pos pos);            /* $ */

/* j/k, reinterpreted per the logical-paragraph decision above -- NOT
   column-preserving (real vim remembers a "desired column" across
   ragged paragraph lengths; deferred, see docs/vimode.md). Lands at the
   target paragraph's start; clamps at the first/last paragraph. */
wp_pos wp_pos_paragraph_down(const wp_document *doc, wp_pos pos);
wp_pos wp_pos_paragraph_up(const wp_document *doc, wp_pos pos);

/* vim's { / }: jump to the next/previous EMPTY paragraph, or document
   start/end if none. Single-section scope like the rest of this pass --
   see docs/vimode.md. */
wp_pos wp_pos_para_block_forward(const wp_document *doc, wp_pos pos);  /* } */
wp_pos wp_pos_para_block_backward(const wp_document *doc, wp_pos pos); /* { */
/* gg/G need no new primitive here: wp_pos_doc_start/end above, plus
   direct wp_section_paragraph_get indexing (already public), cover
   both. */


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

/* Splits the paragraph at pos into two paragraphs: every run at/after
   pos moves into a NEW paragraph inserted immediately after the
   original (same section, index pos.paragraph + 1); the original
   paragraph keeps every run/fragment strictly before pos. A pos landing
   strictly mid-run splits that run exactly like insert's mid-run case.
   The new paragraph's style is copied from the original (both share
   style after a split, matching Enter-key behavior in most editors).
   pos must be a valid position as produced by wp_pos_next/prev/
   wp_pos_doc_start/end or an insert's out_pos -- WP_RANGE otherwise (see
   docs/editing.md for the exact accepted shapes). *out_pos, if non-NULL,
   is set to (section, paragraph + 1, 0, 0) -- where a cursor lands after
   Enter. */
wp_status wp_document_split_paragraph_raw(wp_document *doc, wp_pos pos,
                                           wp_pos *out_pos);

/* Merges paragraph (section, paragraph + 1) INTO paragraph (section,
   paragraph): the second paragraph's runs are concatenated onto the
   first (never coalesced into fewer runs, matching insert's own
   never-coalesce rule), the second paragraph's slot is removed (its
   run array and layout cache freed), and every later paragraph in the
   section shifts down by one index. The surviving (first) paragraph's
   style wins. WP_RANGE if paragraph + 1 doesn't exist in that section
   -- single-section only, no cross-section merge (see docs/editing.md).
   *out_pos, if non-NULL, is set to the join point: the position
   immediately after the first paragraph's ORIGINAL last codepoint (or
   (paragraph, 0, 0) if the first paragraph was empty) -- the exact
   inverse of what wp_document_split_paragraph_raw needs to undo this
   merge. */
wp_status wp_document_merge_paragraph_raw(wp_document *doc, u32 section,
                                           u32 paragraph, wp_pos *out_pos);


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

/* Brackets a run of subsequent wp_edit_* calls so wp_edit_undo/redo
   treats the whole run as one user-facing step. Every call between a
   begin/end pair still pushes its own wp_op exactly as it would
   standalone (each remains individually well-formed) -- the pair just
   adds two marker nodes wp_edit_undo/redo walk through instead of
   stopping at. Nests correctly (a group_begin/end pair fully inside
   another one composes into a single outer step). Callers that want an
   OOM during the bracketed run to still produce a well-formed pair
   (rather than a dangling group_begin with no matching end) must call
   wp_undo_group_end even on an early-out failure path -- see
   wp_edit_delete_selection's implementation for the pattern. */
void wp_undo_group_begin(wp_undo_journal *journal);
void wp_undo_group_end(wp_undo_journal *journal);

/* Journaled edit operations: apply the same structural edit as the raw
   functions above, and additionally push an undo record. Any pending
   redo history is discarded (a new edit invalidates it). */
wp_status wp_edit_insert_text(wp_document *doc, wp_undo_journal *journal,
                               wp_pos pos, const u8 *utf8, u32 len,
                               wp_pos *out_pos);
wp_status wp_edit_delete_range(wp_document *doc, wp_undo_journal *journal,
                                u32 section, u32 paragraph, u16 run_index,
                                u32 start_offset, u32 end_offset);
wp_status wp_edit_split_paragraph(wp_document *doc, wp_undo_journal *journal,
                                   wp_pos pos, wp_pos *out_pos);
wp_status wp_edit_merge_paragraph(wp_document *doc, wp_undo_journal *journal,
                                   u32 section, u32 paragraph, wp_pos *out_pos);

/* Deletes the tree range between sel.anchor and sel.caret (order-
   independent). Decomposes into a sequence of calls to
   wp_edit_delete_range (per touched run, including partial first/last
   runs) and wp_edit_merge_paragraph (per paragraph boundary crossed) --
   no new structural logic, no new wp_op encoding beyond the group
   markers below. The whole sequence is wrapped in one undo group (see
   wp_undo_group_begin/end) so it is exactly ONE user-facing undo step
   regardless of how many paragraphs/runs it touched. *out_pos, if
   non-NULL, is set to the normalized start position -- delete collapses
   the selection to its start, standard editor behavior. A no-op
   (WP_OK, *out_pos = anchor==caret) if the selection is already empty.
   Single-section only this pass, same scope as wp_document_extract_text. */
wp_status wp_edit_delete_selection(wp_document *doc, wp_undo_journal *journal,
                                    wp_selection sel, wp_pos *out_pos);

/* Inserts utf8[0..len) at pos, splitting a new paragraph at each
   embedded '\n' instead of inserting a literal '\n' byte into a run's
   text (which the document model never does -- paragraph breaks are
   structural, not embedded characters). This is the write-side
   counterpart to wp_document_extract_text's own '\n'-per-paragraph
   convention, so a range extracted by that function round-trips through
   this one correctly -- the shape every "yank and put elsewhere" or
   "copy/move a block" feature needs (M8.5's vi-mode put commands and
   M10's WordStar block copy/move both call this exact function; a
   future scheme's own paste/yank is expected to as well, rather than
   re-deriving this same splitting logic per scheme). Not wrapped in its
   own undo group (M10: verified this preserves vi-mode's own pre-
   existing, already-ungrouped multi-paragraph-put behavior exactly --
   see docs/vimode.md); a caller that wants the whole paste to undo as
   one step must bracket its own call with wp_undo_group_begin/end.
   *out_pos, if non-NULL, is always set to the position reached so far,
   even on an early WP_NOMEM exit from one of the underlying
   wp_edit_insert_text/wp_edit_split_paragraph calls. */
wp_status wp_edit_paste_text(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                              const u8 *utf8, u32 len, wp_pos *out_pos);

/* Undoes/redoes the most recent entry on the respective stack. WP_ERR if
   that stack is empty. A group (a run of ops bracketed by
   wp_undo_group_begin/end -- currently only wp_edit_delete_selection
   produces one) undoes/redoes as a single call: every op inside it is
   applied, in the correct order, before this function returns. Nested
   groups compose correctly (depth-counted, not "stop at the first
   marker") -- needed because a future Insert-mode session grouping a
   leading wp_edit_delete_selection inside its own outer group is exactly
   this shape. See docs/editing.md's "Undo Grouping" section. */
wp_status wp_edit_undo(wp_document *doc, wp_undo_journal *journal);
wp_status wp_edit_redo(wp_document *doc, wp_undo_journal *journal);

#endif /* WP_EDITING_H */
