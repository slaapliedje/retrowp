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
                                              current_page_used_height > 0 &&
                                              !(line->flags & WP_LINE_FRAME));   /* as page marking */

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
    const wp_frame_desc *fr = row == NULL ? wp_document_frame_get(doc, para->frame) : NULL;
    i32 left, right, w;

    if (fr != NULL) return fr->w > 144 ? fr->w : column_twips;
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
            const wp_frame_desc *fr = wp_document_frame_get(doc, para->frame);
            /* A frame: its text edges, each line's offset down the frame
               (counting the frame's paragraphs before), no advance. */
            i32 fx0 = 0, fx1 = 0, fdy = 0;
            wp_bool frame_first = WP_FALSE;

            if (fr != NULL) {
                u32 q = pi;
                fx0 = fr->x - (fr->xref == WP_FRAME_X_PAGE ? sec->margin_left : 0);
                fx1 = fx0 + wp_paragraph_wrap_twips(doc, sec, pi, wp_section_column_twips(sec));
                while (q > 0 && sec->paras[q - 1].frame == para->frame && sec->paras[q - 1].trow == 0) {
                    const wp_paragraph_layout *pl = (const wp_paragraph_layout *)sec->paras[q - 1].layout;
                    if (pl != NULL) for (j = 0; j < pl->line_count; j++) fdy += pl->lines[j].height_twips;
                    q--;
                }
                frame_first = (wp_bool)(q == pi);
            }
            if (fr == NULL && para->note != 0) {
                /* A footnote's paragraph: placed like a frame -- no room
                   in the text, the column's width -- at its page's foot
                   (wp_document_mark_pages). */
                fx0 = 0;
                fx1 = wp_section_column_twips(sec);
                frame_first = WP_TRUE;
            }
            if (l != NULL) {
                for (j = 0; j < l->line_count; j++) {
                    wp_line *ln = &l->lines[j];
                    ln->flags = 0;
                    ln->advance_twips = ln->height_twips;
                    ln->cell_x0 = ln->cell_x1 = 0;
                    ln->row_dy_twips = ln->row_h_twips = 0;
                    if (fr != NULL || para->note != 0) {
                        ln->flags = (u8)(WP_LINE_FRAME | ((frame_first && j == 0) ? WP_LINE_FRAME_FIRST : 0));
                        ln->advance_twips = 0;
                        ln->cell_x0 = fx0;
                        ln->cell_x1 = fx1 > fx0 ? fx1 : fx0 + 1;
                        ln->row_dy_twips = fdy;
                        fdy += ln->height_twips;
                    }
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
        /* A row given a height (\trrh): at least it, or exactly it -- a
           sheet of labels keeps its pitch whatever each label holds. */
        if (row->height > 0 && row_h < row->height) row_h = row->height;
        else if (row->height < 0) row_h = -row->height;
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

#define MARK_NOTES_MAX 64

/* A page's footnotes, stacked up from its foot in the order they were
   met: each paragraph's lines a frame whose top is the notes' block top
   plus the notes before it. */
static void place_notes(wp_section *sec, const u32 *note_para, u32 notes, i32 notes_h, i32 content_height)
{
    u32 k, j;
    i32 top = content_height - notes_h;

    if (top < 0) top = 0;
    for (k = 0; k < notes; k++) {
        wp_paragraph_layout *l = (wp_paragraph_layout *)sec->paras[note_para[k]].layout;
        if (l == NULL) continue;
        for (j = 0; j < l->line_count; j++) {
            l->lines[j].frame_top_twips = top;
            l->lines[j].page_y_twips = top + l->lines[j].row_dy_twips;
        }
        for (j = 0; j < l->line_count; j++) top += l->lines[j].height_twips;
    }
}

u32 wp_document_mark_pages(wp_document *doc)
{
    u32 s_idx;
    u32 page = 0;
    wp_bool have_page = WP_FALSE;
    i32 used = 0, row_top = 0, frame_top = 0;
    /* The footnotes met on the page being filled: placed at its foot
       when it closes (place_notes), their height kept free of text. */
    u32 note_para[MARK_NOTES_MAX];
    u32 notes = 0;
    i32 notes_h = 0;

    for (s_idx = 0; s_idx < doc->section_count; s_idx++) {
        wp_section *sec = &doc->sections[s_idx];
        i32 content_height = sec->page_height - sec->margin_top - sec->margin_bottom;
        u32 p_idx;

        place_table_lines(doc, sec);
        /* A new section starts a new page, as in wp_document_paginate. */
        if (have_page && s_idx > 0) {
            place_notes(&doc->sections[s_idx - 1], note_para, notes, notes_h,
                        doc->sections[s_idx - 1].page_height - doc->sections[s_idx - 1].margin_top -
                        doc->sections[s_idx - 1].margin_bottom);
            notes = 0; notes_h = 0;
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

                /* A footnote's line: on the page of the text it follows,
                   collected to go at that page's foot. */
                if ((line->flags & WP_LINE_FRAME) && sec->paras[p_idx].note != 0) {
                    line->page = page;
                    line->page_start = WP_FALSE;
                    if (l_idx == 0) {
                        u32 k2;
                        if (notes < MARK_NOTES_MAX) note_para[notes++] = p_idx;
                        for (k2 = 0; k2 < layout->line_count; k2++) notes_h += layout->lines[k2].height_twips;
                    }
                    continue;
                }
                /* A frame line takes no room: it goes on the page of the
                   text it follows, at its frame's place on that page. */
                if (line->flags & WP_LINE_FRAME) {
                    if (line->flags & WP_LINE_FRAME_FIRST) {
                        const wp_frame_desc *fr = wp_document_frame_get(doc, sec->paras[p_idx].frame);
                        frame_top = fr == NULL ? used :
                                    fr->yref == WP_FRAME_Y_PAGE ? fr->y - sec->margin_top :
                                    fr->yref == WP_FRAME_Y_MARGIN ? fr->y : used + fr->y;
                    }
                    line->page = page;   /* a frame never starts a page */
                    line->page_start = WP_FALSE;
                    line->frame_top_twips = frame_top;
                    line->page_y_twips = frame_top + line->row_dy_twips;
                    continue;
                }

                if (!have_page) {
                    have_page = WP_TRUE;
                    starts = WP_TRUE;
                } else if (used > 0 &&
                           (used + need > content_height - notes_h ||
                            (l_idx == 0 && para_breaks_page(doc, &sec->paras[p_idx])))) {
                    place_notes(sec, note_para, notes, notes_h, content_height);
                    notes = 0; notes_h = 0;
                    page++;
                    used = 0;
                    starts = WP_TRUE;
                }
                line->page = page;
                line->page_start = starts;
                if (line->flags & WP_LINE_ROW_FIRST) row_top = used;
                line->page_y_twips = line->cell_x1 != 0 ? row_top + line->row_dy_twips : used;
                used += line->advance_twips;
            }
        }
    }
    if (doc->section_count > 0) {
        wp_section *last = &doc->sections[doc->section_count - 1];
        place_notes(last, note_para, notes, notes_h,
                    last->page_height - last->margin_top - last->margin_bottom);
    }
    return page + 1;
}
