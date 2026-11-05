/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "docmodel.h"
#include "platform.h"
#include "growth_internal.h"

static wp_status section_table_grow(wp_document *doc, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= doc->section_capacity) return WP_OK;

    new_cap = wp_grow_capacity(doc->section_capacity, needed_count, 2);

    new_mem = mem_realloc(doc->sections,
                           doc->section_capacity * (u32)sizeof(wp_section),
                           new_cap * (u32)sizeof(wp_section));
    if (new_mem == NULL) return WP_NOMEM;

    doc->sections = (wp_section *)new_mem;
    doc->section_capacity = new_cap;
    return WP_OK;
}

wp_status wp_document_split_section_raw(wp_document *doc, u32 section, u32 paragraph)
{
    wp_section *sec, *ns;
    wp_paragraph *moved;
    u32 n, i;
    wp_status st;

    if (section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[section];
    if (paragraph < 1 || paragraph >= sec->para_count) return WP_RANGE;
    n = sec->para_count - paragraph;

    moved = (wp_paragraph *)mem_alloc(n * (u32)sizeof(wp_paragraph));
    if (moved == NULL) return WP_NOMEM;
    st = section_table_grow(doc, doc->section_count + 1);
    if (st != WP_OK) { mem_free(moved); return st; }

    sec = &doc->sections[section];          /* the table may have moved */
    for (i = doc->section_count; i > section + 1; i--) doc->sections[i] = doc->sections[i - 1];
    doc->section_count++;

    ns = &doc->sections[section + 1];
    *ns = *sec;                              /* page setup, header, footer */
    memcpy(moved, sec->paras + paragraph, n * (u32)sizeof(wp_paragraph));
    ns->paras = moved;
    ns->para_count = n;
    ns->para_capacity = n;
    sec->para_count = paragraph;
    for (i = 0; i < n; i++) ns->paras[i].layout_dirty = WP_TRUE;
    return WP_OK;
}

wp_status wp_document_merge_section_raw(wp_document *doc, u32 section)
{
    wp_section *sec, *next;
    u32 need, i;

    if (section + 1 >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[section];
    next = &doc->sections[section + 1];
    need = sec->para_count + next->para_count;
    if (need > sec->para_capacity) {
        void *grown = mem_realloc(sec->paras, sec->para_capacity * (u32)sizeof(wp_paragraph),
                                  need * (u32)sizeof(wp_paragraph));
        if (grown == NULL) return WP_NOMEM;
        sec->paras = (wp_paragraph *)grown;
        sec->para_capacity = need;
    }
    memcpy(sec->paras + sec->para_count, next->paras, next->para_count * (u32)sizeof(wp_paragraph));
    for (i = sec->para_count; i < need; i++) sec->paras[i].layout_dirty = WP_TRUE;
    sec->para_count = need;
    mem_free(next->paras);
    for (i = section + 1; i + 1 < doc->section_count; i++) doc->sections[i] = doc->sections[i + 1];
    doc->section_count--;
    return WP_OK;
}

wp_status wp_document_section_add(wp_document *doc, u32 *out_index)
{
    wp_status st;
    wp_section *sec;

    st = section_table_grow(doc, doc->section_count + 1);
    if (st != WP_OK) return st;

    sec = &doc->sections[doc->section_count];
    memset(sec, 0, sizeof(*sec));
    /* Reasonable defaults (US Letter, 1in margins); format importers and
       other callers override these as needed. */
    sec->page_width = 12240;
    sec->page_height = 15840;
    sec->margin_left = 1440;
    sec->margin_right = 1440;
    sec->margin_top = 1440;
    sec->margin_bottom = 1440;
    sec->header.offset = 0; sec->header.length = 0;
    sec->footer.offset = 0; sec->footer.length = 0;

    *out_index = doc->section_count;
    doc->section_count++;
    return WP_OK;
}

wp_section *wp_document_section_get(wp_document *doc, u32 index)
{
    if (index >= doc->section_count) return NULL;
    return &doc->sections[index];
}

static wp_status para_table_grow(wp_section *section, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= section->para_capacity) return WP_OK;

    new_cap = wp_grow_capacity(section->para_capacity, needed_count, 4);

    new_mem = mem_realloc(section->paras,
                           section->para_capacity * (u32)sizeof(wp_paragraph),
                           new_cap * (u32)sizeof(wp_paragraph));
    if (new_mem == NULL) return WP_NOMEM;

    section->paras = (wp_paragraph *)new_mem;
    section->para_capacity = new_cap;
    return WP_OK;
}

wp_status wp_section_paragraph_add(wp_section *section, u32 *out_index)
{
    wp_status st;
    wp_paragraph *para;

    st = para_table_grow(section, section->para_count + 1);
    if (st != WP_OK) return st;

    para = &section->paras[section->para_count];
    memset(para, 0, sizeof(*para));

    *out_index = section->para_count;
    section->para_count++;
    return WP_OK;
}

wp_paragraph *wp_section_paragraph_get(wp_section *section, u32 index)
{
    if (index >= section->para_count) return NULL;
    return &section->paras[index];
}

static wp_status run_table_grow(wp_paragraph *para, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= para->run_capacity) return WP_OK;
    if (needed_count > 0xFFFFu) return WP_RANGE;

    new_cap = wp_grow_capacity(para->run_capacity, needed_count, 2);
    if (new_cap > 0xFFFFu) new_cap = 0xFFFFu;

    new_mem = mem_realloc(para->runs,
                           (u32)para->run_capacity * (u32)sizeof(wp_run),
                           new_cap * (u32)sizeof(wp_run));
    if (new_mem == NULL) return WP_NOMEM;

    para->runs = (wp_run *)new_mem;
    para->run_capacity = (u16)new_cap;
    return WP_OK;
}

wp_status wp_paragraph_run_add(wp_paragraph *para, wp_span text, wp_cstyle style,
                                u16 *out_index)
{
    wp_status st;

    if (para->run_count == 0xFFFFu) return WP_RANGE;

    st = run_table_grow(para, (u32)para->run_count + 1);
    if (st != WP_OK) return st;

    para->runs[para->run_count].text = text;
    para->runs[para->run_count].style = style;

    *out_index = para->run_count;
    para->run_count++;
    para->layout_dirty = WP_TRUE;
    return WP_OK;
}

wp_run *wp_paragraph_run_get(wp_paragraph *para, u16 index)
{
    if (index >= para->run_count) return NULL;
    return &para->runs[index];
}

void wp_paragraph_pstyle_set(wp_paragraph *para, wp_pstyle style)
{
    para->style = style;
    para->layout_dirty = WP_TRUE;
}

void wp_run_cstyle_set(wp_paragraph *para, u16 index, wp_cstyle style)
{
    if (index >= para->run_count) return;
    para->runs[index].style = style;
    para->layout_dirty = WP_TRUE; /* size/flags change measured width -- see docmodel.h */
}
