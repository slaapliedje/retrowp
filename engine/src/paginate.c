/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "layout.h"
#include "platform.h"
#include "growth_internal.h"

/* A paragraph whose style asks to start a new page (and is not already
   at the top of one -- the callers check that). */
static wp_bool para_breaks_page(const wp_document *doc, const wp_paragraph *para)
{
    const wp_para_style *ps = wp_document_pstyle_get(doc, para->style);
    return (wp_bool)(ps != NULL && ps->page_break_before);
}

static wp_status pagination_add_page(wp_pagination *pagination)
{
    wp_page *page;

    if (pagination->page_count >= pagination->page_capacity) {
        u32 new_cap = wp_grow_capacity(pagination->page_capacity,
                                        pagination->page_count + 1, 4);
        void *new_mem = mem_realloc(pagination->pages,
                                     pagination->page_capacity * (u32)sizeof(wp_page),
                                     new_cap * (u32)sizeof(wp_page));
        if (new_mem == NULL) return WP_NOMEM;
        pagination->pages = (wp_page *)new_mem;
        pagination->page_capacity = new_cap;
    }

    page = &pagination->pages[pagination->page_count];
    page->lines = NULL;
    page->line_count = 0;
    page->line_capacity = 0;
    pagination->page_count++;
    return WP_OK;
}

static wp_status pagination_page_add_line(wp_pagination *pagination, u32 section,
                                           u32 paragraph, u32 line_index)
{
    wp_page *page = &pagination->pages[pagination->page_count - 1];
    wp_page_line_ref *ref;

    if (page->line_count >= page->line_capacity) {
        u32 new_cap = wp_grow_capacity(page->line_capacity, page->line_count + 1, 8);
        void *new_mem = mem_realloc(page->lines,
                                     page->line_capacity * (u32)sizeof(wp_page_line_ref),
                                     new_cap * (u32)sizeof(wp_page_line_ref));
        if (new_mem == NULL) return WP_NOMEM;
        page->lines = (wp_page_line_ref *)new_mem;
        page->line_capacity = new_cap;
    }

    ref = &page->lines[page->line_count];
    ref->section = section;
    ref->paragraph = paragraph;
    ref->line_index = line_index;
    page->line_count++;
    return WP_OK;
}

static void place_table_lines(wp_document *doc, wp_section *sec);

wp_status wp_document_paginate(wp_document *doc, wp_pagination *out)
{
    u32 s_idx;
    wp_status st;
    wp_bool have_current_page = WP_FALSE;
    i32 current_page_used_height = 0;

    out->pages = NULL;
    out->page_count = 0;
    out->page_capacity = 0;

    for (s_idx = 0; s_idx < doc->section_count; s_idx++) {
        wp_section *sec = &doc->sections[s_idx];
        i32 content_width = sec->page_width - sec->margin_left - sec->margin_right;
        i32 content_height = sec->page_height - sec->margin_top - sec->margin_bottom;
        u32 p_idx;

        /* A new section always starts a new page, even if the previous
           page has room left (docs/layout.md). */
        have_current_page = WP_FALSE;

        for (p_idx = 0; p_idx < sec->para_count; p_idx++) {
            wp_paragraph *para = &sec->paras[p_idx];

            /* Minimal recompute (T3.3): only paragraphs that are dirty
               (or have never been laid out) pay for line breaking --
               every other paragraph's existing line list is reused
               untouched. Page-filling below always runs in full over
               every paragraph's line list regardless; that's cheap
               arithmetic, not a font-measurement hot path -- see
               docs/layout.md for why that asymmetry is deliberate. */
            if (para->layout == NULL || para->layout_dirty) {
                st = wp_paragraph_relayout(doc, para, wp_paragraph_wrap_twips(doc, sec, p_idx, content_width));
                if (st != WP_OK) { wp_pagination_free(out); return st; }
            }
        }
        place_table_lines(doc, sec);

        for (p_idx = 0; p_idx < sec->para_count; p_idx++) {
            wp_paragraph *para = &sec->paras[p_idx];
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;
            u32 l_idx;

            for (l_idx = 0; l_idx < layout->line_count; l_idx++) {
                wp_line *line = &layout->lines[l_idx];
                i32 need = (line->flags & WP_LINE_ROW_FIRST) ? line->row_h_twips :
                           (line->cell_x1 != 0 ? 0 : line->height_twips);
                wp_bool overflow = (wp_bool)((current_page_used_height + need >
                                               content_height ||
                                               (l_idx == 0 && para_breaks_page(doc, para))) &&
                                              current_page_used_height > 0);

                if (!have_current_page || overflow) {
                    st = pagination_add_page(out);
                    if (st != WP_OK) { wp_pagination_free(out); return st; }
                    have_current_page = WP_TRUE;
                    current_page_used_height = 0;
                }

                st = pagination_page_add_line(out, s_idx, p_idx, l_idx);
                if (st != WP_OK) { wp_pagination_free(out); return st; }
                current_page_used_height += line->advance_twips;
            }
        }
    }

    return WP_OK;
}

void wp_pagination_free(wp_pagination *pagination)
{
    u32 i;

    for (i = 0; i < pagination->page_count; i++) {
        mem_free(pagination->pages[i].lines);
    }
    mem_free(pagination->pages);
    pagination->pages = NULL;
    pagination->page_count = 0;
    pagination->page_capacity = 0;
}

i32 wp_section_column_twips(const wp_section *sec)
{
    i32 w = sec->page_width - sec->margin_left - sec->margin_right;
    return (w < 1440) ? 1440 : w;
}

void wp_table_cell_edges(const wp_table_row *row, u32 k, i32 *left, i32 *right)
{
    u32 n = row->cell_count;

    if (n > WP_MAX_CELLS) n = WP_MAX_CELLS;
    if (k < n) {
        *left = (k == 0) ? row->left : row->cell_right[k - 1];
        *right = row->cell_right[k];
    } else {
        i32 last = (n > 0) ? row->cell_right[n - 1] : row->left;
        *left = last + (i32)(k - n) * 1440;
        *right = *left + 1440;
    }
    if (*right < *left + 1) *right = *left + 1;
}

/* The cell a table paragraph is in: the cell ends between its row's
   start and it. */
static u32 cell_of(const wp_section *sec, u32 pi)
{
    u32 k = 0, i = pi;

    while (i > 0) {
        const wp_paragraph *prev = &sec->paras[i - 1];
        if (prev->trow == 0 || (prev->tflags & WP_TROW_END)) break;
        if (prev->tflags & WP_TCELL_END) k++;
        i--;
    }
    return k;
}

i32 wp_paragraph_wrap_twips(const wp_document *doc, const wp_section *sec, u32 para_index,
                            i32 column_twips)
{
    const wp_paragraph *para = &sec->paras[para_index];
    const wp_table_row *row = wp_document_trow_get(doc, para->trow);
    i32 left, right, w;

    if (row == NULL) return column_twips;
    wp_table_cell_edges(row, cell_of(sec, para_index), &left, &right);
    w = right - left - 2 * row->gap;
    return w < 144 ? 144 : w;
}

/* The table pass (docs/document-model.md, "Tables"): every line's place
   in its row -- the cell's text edges, its offset below the row's top,
   the row's height on its first line, and the advance that walks a
   table-blind walker past the row correctly. Outside tables a line's
   advance is its height. Runs before every page marking, over layouts
   that may have been rebuilt since. */
static void place_table_lines(wp_document *doc, wp_section *sec)
{
    u32 pi = 0;

    while (pi < sec->para_count) {
        wp_paragraph *para = &sec->paras[pi];
        const wp_table_row *row = wp_document_trow_get(doc, para->trow);
        u32 end, k, j;
        i32 cell_h[WP_MAX_CELLS + 1];
        i32 row_h = 0;

        if (row == NULL) {
            wp_paragraph_layout *l = (wp_paragraph_layout *)para->layout;
            if (l != NULL) {
                for (j = 0; j < l->line_count; j++) {
                    wp_line *ln = &l->lines[j];
                    ln->flags = 0;
                    ln->advance_twips = ln->height_twips;
                    ln->cell_x0 = ln->cell_x1 = 0;
                    ln->row_dy_twips = ln->row_h_twips = 0;
                }
            }
            pi++;
            continue;
        }
        /* The row: up to its WP_TROW_END, a paragraph outside the table,
           or the section's end. */
        end = pi;
        while (end < sec->para_count && sec->paras[end].trow != 0 &&
               !(sec->paras[end].tflags & WP_TROW_END)) end++;
        if (end >= sec->para_count || sec->paras[end].trow == 0) end--;

        /* Each cell's lines, stacked from the row's top. */
        for (k = 0; k <= WP_MAX_CELLS; k++) cell_h[k] = 0;
        k = 0;
        for (j = pi; j <= end; j++) {
            wp_paragraph_layout *l = (wp_paragraph_layout *)sec->paras[j].layout;
            u32 kk = k < WP_MAX_CELLS ? k : WP_MAX_CELLS;
            i32 left, right;
            u32 li;

            wp_table_cell_edges(row, k, &left, &right);
            if (l != NULL) {
                for (li = 0; li < l->line_count; li++) {
                    wp_line *ln = &l->lines[li];
                    ln->flags = (u8)((j == pi && li == 0) ? WP_LINE_ROW_FIRST : 0);
                    ln->cell_x0 = left + row->gap;
                    ln->cell_x1 = right - row->gap;
                    ln->row_dy_twips = cell_h[kk];
                    ln->advance_twips = ln->height_twips;
                    ln->row_h_twips = 0;
                    cell_h[kk] += ln->height_twips;
                }
            }
            if (cell_h[kk] > row_h) row_h = cell_h[kk];
            if (sec->paras[j].tflags & WP_TCELL_END) k++;
        }
        /* Each cell's last line goes back up to the row's top, the row's
           last line down to its bottom. */
        k = 0;
        for (j = pi; j <= end; j++) {
            wp_paragraph_layout *l = (wp_paragraph_layout *)sec->paras[j].layout;
            wp_bool cell_end = (wp_bool)((sec->paras[j].tflags & WP_TCELL_END) || j == end);
            u32 kk = k < WP_MAX_CELLS ? k : WP_MAX_CELLS;

            if (l != NULL && l->line_count > 0 && cell_end) {
                wp_line *last = &l->lines[l->line_count - 1];
                last->advance_twips = last->height_twips - cell_h[kk] + (j == end ? row_h : 0);
            }
            if (sec->paras[j].tflags & WP_TCELL_END) k++;
        }
        {
            wp_paragraph_layout *l = (wp_paragraph_layout *)sec->paras[pi].layout;
            if (l != NULL && l->line_count > 0) l->lines[0].row_h_twips = row_h;
        }
        pi = end + 1;
    }
}

u32 wp_document_mark_pages(wp_document *doc)
{
    u32 s_idx;
    u32 page = 0;
    wp_bool have_page = WP_FALSE;
    i32 used = 0;

    for (s_idx = 0; s_idx < doc->section_count; s_idx++) {
        wp_section *sec = &doc->sections[s_idx];
        i32 content_height = sec->page_height - sec->margin_top - sec->margin_bottom;
        u32 p_idx;

        place_table_lines(doc, sec);
        /* A new section starts a new page, as in wp_document_paginate. */
        if (have_page && s_idx > 0) {
            page++;
            used = 0;
            have_page = WP_FALSE;
        }
        for (p_idx = 0; p_idx < sec->para_count; p_idx++) {
            wp_paragraph_layout *layout = (wp_paragraph_layout *)sec->paras[p_idx].layout;
            u32 l_idx;

            if (layout == NULL) continue;
            for (l_idx = 0; l_idx < layout->line_count; l_idx++) {
                wp_line *line = &layout->lines[l_idx];
                wp_bool starts = WP_FALSE;

                /* What must fit: a line, or a whole table row from its
                   first line -- a row is kept on one page. */
                i32 need = (line->flags & WP_LINE_ROW_FIRST) ? line->row_h_twips :
                           (line->cell_x1 != 0 ? 0 : line->height_twips);

                if (!have_page) {
                    have_page = WP_TRUE;
                    starts = WP_TRUE;
                } else if (used > 0 &&
                           (used + need > content_height ||
                            (l_idx == 0 && para_breaks_page(doc, &sec->paras[p_idx])))) {
                    page++;
                    used = 0;
                    starts = WP_TRUE;
                }
                line->page = page;
                line->page_start = starts;
                used += line->advance_twips;
            }
        }
    }
    return page + 1;
}
