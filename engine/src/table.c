/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "table.h"
#include "layout.h"

/* ---- Where things are ------------------------------------------------ */

/* The row paragraph pi is in: [*first, *last]. */
static wp_bool row_bounds(const wp_section *sec, u32 pi, u32 *first, u32 *last)
{
    u32 f = pi, l = pi;

    if (pi >= sec->para_count || sec->paras[pi].trow == 0) return WP_FALSE;
    while (f > 0 && sec->paras[f - 1].trow != 0 && !(sec->paras[f - 1].tflags & WP_TROW_END)) f--;
    while (l + 1 < sec->para_count && !(sec->paras[l].tflags & WP_TROW_END) &&
           sec->paras[l + 1].trow != 0) l++;
    *first = f;
    *last = l;
    return WP_TRUE;
}

/* The table paragraph pi is in: its first row's first paragraph. */
static u32 table_first(const wp_section *sec, u32 pi)
{
    while (pi > 0 && sec->paras[pi - 1].trow != 0) pi--;
    return pi;
}

/* Cell k of the row [first, last]: its paragraphs [*c0, *c1]. */
static wp_bool cell_bounds(const wp_section *sec, u32 first, u32 last, u32 k, u32 *c0, u32 *c1)
{
    u32 pi = first, cell = 0;

    while (pi <= last && cell < k) {
        if (sec->paras[pi].tflags & WP_TCELL_END) cell++;
        pi++;
    }
    if (pi > last) return WP_FALSE;
    *c0 = pi;
    while (pi < last && !(sec->paras[pi].tflags & WP_TCELL_END)) pi++;
    *c1 = pi;
    return WP_TRUE;
}

static u32 cell_index(const wp_section *sec, u32 first, u32 pi)
{
    u32 k = 0;

    for (; first < pi; first++) if (sec->paras[first].tflags & WP_TCELL_END) k++;
    return k;
}

static u32 cell_count(const wp_section *sec, u32 first, u32 last)
{
    return cell_index(sec, first, last) + 1u;
}

static wp_pos cell_start(u32 si, u32 pi)
{
    wp_pos p;

    p.section = si;
    p.paragraph = pi;
    p.run = 0;
    p.offset = 0;
    return p;
}

/* ---- Moving between cells -------------------------------------------- */

wp_bool wp_table_cell_move(const wp_document *doc, const wp_pos *pos, wp_bool backward,
                           wp_pos *out)
{
    const wp_section *sec;
    u32 pi;

    if (pos->section >= doc->section_count) return WP_FALSE;
    sec = &doc->sections[pos->section];
    pi = pos->paragraph;
    if (pi >= sec->para_count || sec->paras[pi].trow == 0) return WP_FALSE;

    if (!backward) {
        while (pi < sec->para_count && !(sec->paras[pi].tflags & WP_TCELL_END)) pi++;
        pi++;   /* past this cell's end */
        if (pi >= sec->para_count || sec->paras[pi].trow == 0) return WP_FALSE;
        *out = cell_start(pos->section, pi);
        return WP_TRUE;
    }
    /* Back to this cell's start, then to the previous cell's. */
    while (pi > 0 && sec->paras[pi - 1].trow != 0 && !(sec->paras[pi - 1].tflags & WP_TCELL_END)) pi--;
    if (pi == 0 || sec->paras[pi - 1].trow == 0) return WP_FALSE;
    pi--;
    while (pi > 0 && sec->paras[pi - 1].trow != 0 && !(sec->paras[pi - 1].tflags & WP_TCELL_END)) pi--;
    *out = cell_start(pos->section, pi);
    return WP_TRUE;
}

/* ---- Pieces ------------------------------------------------------------ */

/* A paragraph's text deleted, journaled. */
static wp_status clear_text(wp_document *doc, wp_undo_journal *journal, u32 si, u32 pi)
{
    const wp_paragraph *para = &doc->sections[si].paras[pi];
    wp_selection sel;
    wp_pos dummy;
    u32 total = 0;
    u16 i;

    for (i = 0; i < para->run_count; i++) total += para->runs[i].text.length;
    if (total == 0) return WP_OK;
    sel.anchor = cell_start(si, pi);
    sel.caret = sel.anchor;
    sel.caret.run = (u32)(para->run_count - 1);
    sel.caret.offset = para->runs[para->run_count - 1].text.length;
    return wp_edit_delete_selection(doc, journal, sel, &dummy);
}

/* A paragraph taken out, its text first; a section's last paragraph is
   kept, made plain. */
static wp_status remove_paragraph(wp_document *doc, wp_undo_journal *journal, u32 si, u32 pi)
{
    wp_status st = clear_text(doc, journal, si, pi);

    if (st != WP_OK) return st;
    if (doc->sections[si].para_count < 2 ||
        wp_edit_remove_empty_paragraph(doc, journal, si, pi) != WP_OK) {
        return wp_edit_set_table(doc, journal, si, pi, 0, 0);
    }
    return WP_OK;
}

/* Every paragraph of a row moved to another shape, its flags kept. */
static wp_status reshape_row(wp_document *doc, wp_undo_journal *journal, u32 si,
                             u32 first, u32 last, wp_trow shape)
{
    u32 pi;
    wp_status st = WP_OK;

    for (pi = first; pi <= last && st == WP_OK; pi++) {
        st = wp_edit_set_table(doc, journal, si, pi, shape, doc->sections[si].paras[pi].tflags);
    }
    return st;
}

/* ---- Tables, rows, columns ------------------------------------------- */

wp_status wp_edit_table_insert(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                               u32 rows, u32 cols, i32 column_twips, wp_pos *out)
{
    wp_table_row shape;
    wp_trow handle;
    wp_section *sec;
    u32 at, r, c, first, last;
    i32 w;
    wp_status st = WP_OK;

    if (pos.section >= doc->section_count || rows == 0 || cols == 0) return WP_RANGE;
    if (cols > WP_MAX_CELLS) cols = WP_MAX_CELLS;
    sec = &doc->sections[pos.section];
    if (pos.paragraph >= sec->para_count) return WP_RANGE;
    memset(&shape, 0, sizeof(shape));
    shape.gap = 108;             /* Word's \trgaph108 */
    shape.cell_count = (u8)cols;
    w = column_twips / (i32)cols;
    if (w < 288) w = 288;
    for (c = 0; c < cols; c++) {
        shape.cell_right[c] = w * (i32)(c + 1);
        shape.cell_borders[c] = WP_BORDER_BOX;
    }
    if (wp_document_trow_find_or_add(doc, &shape, &handle) != WP_OK) return WP_NOMEM;

    at = pos.paragraph + 1;
    if (row_bounds(sec, pos.paragraph, &first, &last)) at = last + 1;
    wp_undo_group_begin(journal);
    for (r = 0; r < rows && st == WP_OK; r++) {
        for (c = 0; c < cols && st == WP_OK; c++) {
            u8 flags = (u8)(WP_TCELL_END | (c + 1 == cols ? WP_TROW_END : 0));
            st = wp_edit_insert_empty_paragraph(doc, journal, pos.section, at + r * cols + c,
                                                doc->default_pstyle, handle, flags);
        }
    }
    wp_undo_group_end(journal);
    if (st == WP_OK && out != NULL) *out = cell_start(pos.section, at);
    return st;
}

wp_status wp_edit_table_insert_row(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                   wp_pos *out)
{
    wp_section *sec;
    u32 first, last, n, k, c0, c1;
    wp_trow shape;
    wp_status st = WP_OK;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[pos.section];
    if (!row_bounds(sec, pos.paragraph, &first, &last)) return WP_RANGE;
    n = cell_count(sec, first, last);
    shape = sec->paras[first].trow;
    wp_undo_group_begin(journal);
    for (k = 0; k < n && st == WP_OK; k++) {
        wp_pstyle style = doc->default_pstyle;
        u8 flags = (u8)(WP_TCELL_END | (k + 1 == n ? WP_TROW_END : 0));

        sec = &doc->sections[pos.section];   /* may move as paragraphs go in */
        if (cell_bounds(sec, first, last, k, &c0, &c1)) style = sec->paras[c0].style;
        st = wp_edit_insert_empty_paragraph(doc, journal, pos.section, last + 1 + k, style, shape, flags);
    }
    wp_undo_group_end(journal);
    if (st == WP_OK && out != NULL) *out = cell_start(pos.section, last + 1);
    return st;
}

wp_status wp_edit_table_delete_row(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                   wp_pos *out)
{
    wp_section *sec;
    u32 first, last, pi;
    wp_status st = WP_OK;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[pos.section];
    if (!row_bounds(sec, pos.paragraph, &first, &last)) return WP_RANGE;
    wp_undo_group_begin(journal);
    for (pi = last + 1; pi > first && st == WP_OK; pi--) {
        st = remove_paragraph(doc, journal, pos.section, pi - 1);
    }
    wp_undo_group_end(journal);
    sec = &doc->sections[pos.section];
    if (out != NULL) *out = cell_start(pos.section, first < sec->para_count ? first : sec->para_count - 1);
    return st;
}

/* The column operations walk the table's rows from its last to its
   first, so the paragraphs put in or taken out of one row never move
   the rows still to do. */
wp_status wp_edit_table_insert_column(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                      wp_pos *out)
{
    wp_section *sec;
    u32 first, last, k, top, pi;
    wp_status st = WP_OK;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[pos.section];
    if (!row_bounds(sec, pos.paragraph, &first, &last)) return WP_RANGE;
    k = cell_index(sec, first, pos.paragraph);
    top = table_first(sec, pos.paragraph);
    while (last + 1 < sec->para_count && sec->paras[last + 1].trow != 0) {
        u32 f2, l2;
        if (!row_bounds(sec, last + 1, &f2, &l2)) break;
        last = l2;
    }

    wp_undo_group_begin(journal);
    pi = last;
    while (st == WP_OK) {
        u32 rf, rl, c0, c1, j;
        const wp_table_row *old;
        wp_table_row shape;
        wp_trow handle;
        i32 left, right, mid;
        wp_bool was_last;

        sec = &doc->sections[pos.section];
        if (!row_bounds(sec, pi, &rf, &rl)) break;
        old = wp_document_trow_get(doc, sec->paras[rf].trow);
        if (old != NULL && cell_bounds(sec, rf, rl, k, &c0, &c1) && old->cell_count < WP_MAX_CELLS) {
            shape = *old;
            wp_table_cell_edges(old, k, &left, &right);
            mid = left + (right - left) / 2;
            for (j = shape.cell_count; j > k + 1; j--) {
                shape.cell_right[j] = shape.cell_right[j - 1];
                shape.cell_borders[j] = shape.cell_borders[j - 1];
                shape.cell_shading[j] = shape.cell_shading[j - 1];
            }
            if (k < shape.cell_count) {
                shape.cell_right[k] = mid;
                shape.cell_right[k + 1] = right;
                shape.cell_borders[k + 1] = shape.cell_borders[k];
                shape.cell_shading[k + 1] = shape.cell_shading[k];
                shape.cell_count++;
            }
            if (wp_document_trow_find_or_add(doc, &shape, &handle) != WP_OK) { st = WP_NOMEM; break; }
            sec = &doc->sections[pos.section];
            was_last = (wp_bool)((sec->paras[c1].tflags & WP_TROW_END) != 0);
            st = wp_edit_insert_empty_paragraph(doc, journal, pos.section, c1 + 1,
                                                sec->paras[c1].style, handle,
                                                (u8)(WP_TCELL_END | (was_last ? WP_TROW_END : 0)));
            if (st == WP_OK && was_last) {
                st = wp_edit_set_table(doc, journal, pos.section, c1, handle, WP_TCELL_END);
            }
            if (st == WP_OK) st = reshape_row(doc, journal, pos.section, rf, rl + 1, handle);
        }
        if (rf == 0 || rf <= top) break;
        pi = rf - 1;
    }
    wp_undo_group_end(journal);
    if (st == WP_OK && out != NULL) {
        u32 f3, l3, c0, c1;
        sec = &doc->sections[pos.section];
        *out = pos;
        if (row_bounds(sec, pos.paragraph, &f3, &l3) && cell_bounds(sec, f3, l3, k + 1, &c0, &c1)) {
            *out = cell_start(pos.section, c0);
        }
    }
    return st;
}

wp_status wp_edit_table_delete_column(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                                      wp_pos *out)
{
    wp_section *sec;
    u32 first, last, k, top, pi;
    wp_status st = WP_OK;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[pos.section];
    if (!row_bounds(sec, pos.paragraph, &first, &last)) return WP_RANGE;
    k = cell_index(sec, first, pos.paragraph);
    top = table_first(sec, pos.paragraph);
    while (last + 1 < sec->para_count && sec->paras[last + 1].trow != 0) {
        u32 f2, l2;
        if (!row_bounds(sec, last + 1, &f2, &l2)) break;
        last = l2;
    }

    wp_undo_group_begin(journal);
    pi = last;
    while (st == WP_OK) {
        u32 rf, rl, c0, c1, j, q;
        const wp_table_row *old;
        wp_table_row shape;
        wp_trow handle;
        i32 left, right, w;
        wp_bool was_last;

        sec = &doc->sections[pos.section];
        if (!row_bounds(sec, pi, &rf, &rl)) break;
        old = wp_document_trow_get(doc, sec->paras[rf].trow);
        if (old != NULL && cell_count(sec, rf, rl) > 1 && cell_bounds(sec, rf, rl, k, &c0, &c1)) {
            shape = *old;
            if (k < shape.cell_count) {
                wp_table_cell_edges(old, k, &left, &right);
                w = right - left;
                for (j = k; j + 1 < shape.cell_count; j++) {
                    shape.cell_right[j] = shape.cell_right[j + 1] - w;
                    shape.cell_borders[j] = shape.cell_borders[j + 1];
                    shape.cell_shading[j] = shape.cell_shading[j + 1];
                }
                shape.cell_count--;
                shape.cell_right[shape.cell_count] = 0;
                shape.cell_borders[shape.cell_count] = 0;
                shape.cell_shading[shape.cell_count] = 0;
            }
            if (wp_document_trow_find_or_add(doc, &shape, &handle) != WP_OK) { st = WP_NOMEM; break; }
            sec = &doc->sections[pos.section];
            was_last = (wp_bool)((sec->paras[c1].tflags & WP_TROW_END) != 0);
            for (q = c1 + 1; q > c0 && st == WP_OK; q--) st = remove_paragraph(doc, journal, pos.section, q - 1);
            rl -= (c1 - c0 + 1);
            if (st == WP_OK && was_last) {
                st = wp_edit_set_table(doc, journal, pos.section, c0 - 1, handle, WP_TCELL_END | WP_TROW_END);
            }
            if (st == WP_OK) st = reshape_row(doc, journal, pos.section, rf, rl, handle);
        }
        if (rf == 0 || rf <= top) break;
        pi = rf - 1;
    }
    wp_undo_group_end(journal);
    if (out != NULL) {
        u32 f3, l3, c0, c1;
        sec = &doc->sections[pos.section];
        *out = cell_start(pos.section, top < sec->para_count ? top : 0);
        if (pos.paragraph < sec->para_count && row_bounds(sec, pos.paragraph, &f3, &l3) &&
            cell_bounds(sec, f3, l3, k > 0 ? k - 1 : 0, &c0, &c1)) {
            *out = cell_start(pos.section, c0);
        }
    }
    return st;
}
