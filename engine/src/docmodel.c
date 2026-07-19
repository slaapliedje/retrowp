#include <string.h>
#include "docmodel.h"
#include "platform.h"
#include "growth_internal.h"
#include "layout.h"

static wp_status cstyle_table_grow(wp_document *doc, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= doc->cstyle_capacity) return WP_OK;
    if (needed_count > 0xFFFFu) return WP_RANGE;

    new_cap = wp_grow_capacity(doc->cstyle_capacity, needed_count, 4);
    if (new_cap > 0xFFFFu) new_cap = 0xFFFFu;

    new_mem = mem_realloc(doc->cstyles,
                           (u32)doc->cstyle_capacity * (u32)sizeof(wp_char_style),
                           new_cap * (u32)sizeof(wp_char_style));
    if (new_mem == NULL) return WP_NOMEM;

    doc->cstyles = (wp_char_style *)new_mem;
    doc->cstyle_capacity = (u16)new_cap;
    return WP_OK;
}

static wp_status pstyle_table_grow(wp_document *doc, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= doc->pstyle_capacity) return WP_OK;
    if (needed_count > 0xFFFFu) return WP_RANGE;

    new_cap = wp_grow_capacity(doc->pstyle_capacity, needed_count, 4);
    if (new_cap > 0xFFFFu) new_cap = 0xFFFFu;

    new_mem = mem_realloc(doc->pstyles,
                           (u32)doc->pstyle_capacity * (u32)sizeof(wp_para_style),
                           new_cap * (u32)sizeof(wp_para_style));
    if (new_mem == NULL) return WP_NOMEM;

    doc->pstyles = (wp_para_style *)new_mem;
    doc->pstyle_capacity = (u16)new_cap;
    return WP_OK;
}

wp_status wp_document_cstyle_add(wp_document *doc, const wp_char_style *style,
                                  wp_cstyle *out)
{
    wp_status st;

    if (doc->cstyle_count == 0) {
        st = cstyle_table_grow(doc, 1);
        if (st != WP_OK) return st;
        memset(&doc->cstyles[0], 0, sizeof(wp_char_style));
        doc->cstyle_count = 1;
    }

    if (doc->cstyle_count == 0xFFFFu) return WP_RANGE;

    st = cstyle_table_grow(doc, (u32)doc->cstyle_count + 1);
    if (st != WP_OK) return st;

    doc->cstyles[doc->cstyle_count] = *style;
    *out = (wp_cstyle)doc->cstyle_count;
    doc->cstyle_count++;
    return WP_OK;
}

wp_status wp_document_pstyle_add(wp_document *doc, const wp_para_style *style,
                                  wp_pstyle *out)
{
    wp_status st;

    if (doc->pstyle_count == 0) {
        st = pstyle_table_grow(doc, 1);
        if (st != WP_OK) return st;
        memset(&doc->pstyles[0], 0, sizeof(wp_para_style));
        doc->pstyle_count = 1;
    }

    if (doc->pstyle_count == 0xFFFFu) return WP_RANGE;

    st = pstyle_table_grow(doc, (u32)doc->pstyle_count + 1);
    if (st != WP_OK) return st;

    doc->pstyles[doc->pstyle_count] = *style;
    *out = (wp_pstyle)doc->pstyle_count;
    doc->pstyle_count++;
    return WP_OK;
}

const wp_char_style *wp_document_cstyle_get(const wp_document *doc, wp_cstyle handle)
{
    if (handle == 0 || handle >= doc->cstyle_count) return NULL;
    return &doc->cstyles[handle];
}

const wp_para_style *wp_document_pstyle_get(const wp_document *doc, wp_pstyle handle)
{
    if (handle == 0 || handle >= doc->pstyle_count) return NULL;
    return &doc->pstyles[handle];
}

wp_status wp_document_init(wp_document *doc)
{
    wp_status st;
    wp_char_style default_cs;
    wp_para_style default_ps;

    doc->cstyles = NULL;
    doc->cstyle_count = 0;
    doc->cstyle_capacity = 0;
    doc->pstyles = NULL;
    doc->pstyle_count = 0;
    doc->pstyle_capacity = 0;
    doc->sections = NULL;
    doc->section_count = 0;
    doc->section_capacity = 0;
    doc->title = 0;
    doc->author = 0;

    st = wp_strpool_init(&doc->strings);
    if (st != WP_OK) return st;

    memset(&default_cs, 0, sizeof(default_cs));
    default_cs.size_twips = 240; /* 12pt */

    memset(&default_ps, 0, sizeof(default_ps));
    default_ps.align = WP_ALIGN_LEFT;

    st = wp_document_cstyle_add(doc, &default_cs, &doc->default_cstyle);
    if (st != WP_OK) {
        wp_strpool_free(&doc->strings);
        return st;
    }

    st = wp_document_pstyle_add(doc, &default_ps, &doc->default_pstyle);
    if (st != WP_OK) {
        wp_strpool_free(&doc->strings);
        mem_free(doc->cstyles);
        return st;
    }

    return WP_OK;
}

void wp_document_free(wp_document *doc)
{
    u32 i;

    for (i = 0; i < doc->section_count; i++) {
        wp_section *sec = &doc->sections[i];
        u32 j;

        for (j = 0; j < sec->para_count; j++) {
            wp_paragraph_layout_free(&sec->paras[j]);
            mem_free(sec->paras[j].runs);
        }
        mem_free(sec->paras);
    }
    mem_free(doc->sections);
    mem_free(doc->cstyles);
    mem_free(doc->pstyles);
    wp_strpool_free(&doc->strings);
}
