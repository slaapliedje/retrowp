/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_NOTES_H
#define WP_NOTES_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"

/* Footnotes (M19). A reference is a run whose character format names a
   note (wp_char_style.note) -- superscript, its text the note's number.
   A note's text is one or more paragraphs marked with that note
   (wp_paragraph.note), kept after the paragraph that refers to it and
   starting with the same number; layout puts them at the foot of that
   paragraph's page (paginate.c), the way it places a frame, so every
   painter and the printer draw them with no code of their own. Notes
   are numbered by where their references stand (wp_notes_renumber); a
   note whose reference is gone shows "*". See docs/document-model.md,
   "Footnotes". */

/* Every reference and every note's own number set from the order the
   references stand in. Run before each layout (wp_pageview_relayout);
   only text that changes is touched. */
wp_status wp_notes_renumber(wp_document *doc);

/* A new footnote at `pos`: a reference there, and an empty note
   paragraph -- in the Footnote Text style -- after the paragraph (and
   any notes already following it), its number and a space typed;
   *note_pos the place after them, to type the note at. Made of
   journaled edits, so Undo takes it back. */
wp_status wp_notes_insert(wp_document *doc, wp_undo_journal *journal, wp_pos pos, wp_pos *note_pos);

/* A note id no reference uses yet. */
u16 wp_notes_new_id(const wp_document *doc);

#endif /* WP_NOTES_H */
