#include <stddef.h>
#include "layout.h"
#include "platform.h"
#include "growth_internal.h"

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
            wp_paragraph_layout *layout;
            u32 l_idx;

            /* Minimal recompute (T3.3): only paragraphs that are dirty
               (or have never been laid out) pay for line breaking --
               every other paragraph's existing line list is reused
               untouched. Page-filling below always runs in full over
               every paragraph's line list regardless; that's cheap
               arithmetic, not a font-measurement hot path -- see
               docs/layout.md for why that asymmetry is deliberate. */
            if (para->layout == NULL || para->layout_dirty) {
                st = wp_paragraph_relayout(doc, para, content_width);
                if (st != WP_OK) { wp_pagination_free(out); return st; }
            }
            layout = (wp_paragraph_layout *)para->layout;

            for (l_idx = 0; l_idx < layout->line_count; l_idx++) {
                wp_line *line = &layout->lines[l_idx];
                wp_bool overflow = (wp_bool)(current_page_used_height + line->height_twips >
                                              content_height && current_page_used_height > 0);

                if (!have_current_page || overflow) {
                    st = pagination_add_page(out);
                    if (st != WP_OK) { wp_pagination_free(out); return st; }
                    have_current_page = WP_TRUE;
                    current_page_used_height = 0;
                }

                st = pagination_page_add_line(out, s_idx, p_idx, l_idx);
                if (st != WP_OK) { wp_pagination_free(out); return st; }
                current_page_used_height += line->height_twips;
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
