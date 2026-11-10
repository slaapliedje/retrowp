/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_TABLE_H
#define WP_TABLE_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"

/* Editing tables (docs/document-model.md, "Tables"): moving between
   cells, and the row and column operations. Each edit is one undo step,
   built from the journaled pieces -- text deleted, empty paragraphs put
   in or taken out (wp_edit_insert_empty_paragraph,
   wp_edit_remove_empty_paragraph), table fields set (wp_edit_set_table).
   A "table" is a run of consecutive table paragraphs; its rows end at
   their WP_TROW_END. Positions out are a cell's start. */

/* The next cell's start (backward: the previous cell's), across rows;
   WP_FALSE at the table's edge or outside a table. */
wp_bool wp_table_cell_move(const wp_document *doc, const wp_pos *pos, wp_bool backward,
                           wp_pos *out);

/* A table of rows x cols empty cells after the paragraph at pos (after
   its row, in a table), the cells evenly across column_twips with a
   single rule round each; *out is its first cell. */
wp_status wp_edit_table_insert(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                               u32 rows, u32 cols, i32 column_twips, wp_pos *out);

/* A row after pos's row, shaped like it, each cell taking the style of
   the cell above's first paragraph; *out is its first cell. */
wp_status wp_edit_table_insert_row(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                   wp_pos *out);

/* pos's row deleted, text and all; *out is where the caret goes. */
wp_status wp_edit_table_delete_row(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                   wp_pos *out);

/* In every row of pos's table: a cell after pos's cell, taking the
   second half of its width (insert), or pos's cell deleted, the cells
   after it moving left into its width (delete -- refused where a row has
   one cell). */
wp_status wp_edit_table_insert_column(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                      wp_pos *out);
wp_status wp_edit_table_delete_column(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                      wp_pos *out);

#endif /* WP_TABLE_H */
