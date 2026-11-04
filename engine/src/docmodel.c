/* SPDX-License-Identifier: GPL-2.0-or-later */
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

wp_status wp_document_cstyle_find_or_add(wp_document *doc, const wp_char_style *style,
                                          wp_cstyle *out)
{
    u16 i;

    for (i = 1; i < doc->cstyle_count; i++) {
        const wp_char_style *existing = &doc->cstyles[i];
        if (existing->font == style->font &&
            existing->size_twips == style->size_twips &&
            existing->flags == style->flags &&
            existing->color_r == style->color_r &&
            existing->color_g == style->color_g &&
            existing->color_b == style->color_b) {
            *out = (wp_cstyle)i;
            return WP_OK;
        }
    }
    return wp_document_cstyle_add(doc, style, out);
}

wp_status wp_document_pstyle_find_or_add(wp_document *doc, const wp_para_style *style,
                                          wp_pstyle *out)
{
    u16 i;

    for (i = 1; i < doc->pstyle_count; i++) {
        const wp_para_style *existing = &doc->pstyles[i];
        if (existing->align == style->align &&
            existing->indent_left == style->indent_left &&
            existing->indent_right == style->indent_right &&
            existing->indent_first == style->indent_first &&
            existing->space_before == style->space_before &&
            existing->space_after == style->space_after &&
            existing->line_spacing == style->line_spacing &&
            existing->style_name == style->style_name &&
            existing->tab_count == style->tab_count &&
            existing->page_break_before == style->page_break_before &&
            (style->tab_count == 0 ||
             (memcmp(existing->tab_pos, style->tab_pos, (size_t)style->tab_count * sizeof(i32)) == 0 &&
              memcmp(existing->tab_kind, style->tab_kind, (size_t)style->tab_count) == 0))) {
            *out = (wp_pstyle)i;
            return WP_OK;
        }
    }
    return wp_document_pstyle_add(doc, style, out);
}

static wp_status image_table_grow(wp_document *doc, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= doc->image_capacity) return WP_OK;
    if (needed_count > 0xFFFFu) return WP_RANGE;

    new_cap = wp_grow_capacity(doc->image_capacity, needed_count, 4);
    if (new_cap > 0xFFFFu) new_cap = 0xFFFFu;

    new_mem = mem_realloc(doc->images,
                           (u32)doc->image_capacity * (u32)sizeof(wp_image_desc),
                           new_cap * (u32)sizeof(wp_image_desc));
    if (new_mem == NULL) return WP_NOMEM;

    doc->images = (wp_image_desc *)new_mem;
    doc->image_capacity = (u16)new_cap;
    return WP_OK;
}

wp_status wp_document_image_add(wp_document *doc, const wp_image_desc *desc, wp_image *out)
{
    wp_status st;

    if (doc->image_count == 0) {
        st = image_table_grow(doc, 1);
        if (st != WP_OK) return st;
        memset(&doc->images[0], 0, sizeof(wp_image_desc));
        doc->image_count = 1;
    }

    if (doc->image_count == 0xFFFFu) return WP_RANGE;

    st = image_table_grow(doc, (u32)doc->image_count + 1);
    if (st != WP_OK) return st;

    doc->images[doc->image_count] = *desc;
    *out = (wp_image)doc->image_count;
    doc->image_count++;
    return WP_OK;
}

const wp_image_desc *wp_document_image_get(const wp_document *doc, wp_image handle)
{
    if (handle == 0 || handle >= doc->image_count) return NULL;
    return &doc->images[handle];
}

static u8 ascii_lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c; }

static wp_bool name_equal(const u8 *a, u32 alen, const u8 *b, u32 blen)
{
    u32 i;

    if (alen != blen) return WP_FALSE;
    for (i = 0; i < alen; i++) if (ascii_lower(a[i]) != ascii_lower(b[i])) return WP_FALSE;
    return WP_TRUE;
}

wp_status wp_document_font_find_or_add(wp_document *doc, const u8 *name, u32 len,
                                        u8 family, wp_font *out)
{
    u16 i;
    wp_font_desc desc;
    wp_status st;

    *out = 0;
    while (len > 0 && name[len - 1] == ' ') len--;
    if (len == 0) return WP_OK;

    for (i = 1; i < doc->font_count; i++) {
        wp_font_desc *f = &doc->fonts[i];
        if (name_equal(wp_strpool_get(&doc->strings, f->name), f->name.length, name, len)) {
            if (f->family == WP_FAMILY_NIL) f->family = family;
            *out = (wp_font)i;
            return WP_OK;
        }
    }

    if (doc->font_count == 0xFFFFu) return WP_RANGE;
    if ((u32)doc->font_count + 2 > doc->font_capacity) {
        u32 new_cap = wp_grow_capacity(doc->font_capacity, (u32)doc->font_count + 2, 4);
        void *mem;
        if (new_cap > 0xFFFFu) new_cap = 0xFFFFu;
        mem = mem_realloc(doc->fonts, (u32)doc->font_capacity * (u32)sizeof(wp_font_desc),
                          new_cap * (u32)sizeof(wp_font_desc));
        if (mem == NULL) return WP_NOMEM;
        doc->fonts = (wp_font_desc *)mem;
        doc->font_capacity = (u16)new_cap;
    }
    if (doc->font_count == 0) {
        memset(&doc->fonts[0], 0, sizeof(wp_font_desc));
        doc->font_count = 1;
    }

    memset(&desc, 0, sizeof(desc));
    st = wp_strpool_append(&doc->strings, name, len, &desc.name);
    if (st != WP_OK) return st;
    desc.family = family;
    doc->fonts[doc->font_count] = desc;
    *out = (wp_font)doc->font_count;
    doc->font_count++;
    return WP_OK;
}

const wp_font_desc *wp_document_font_get(const wp_document *doc, wp_font id)
{
    if (id == 0 || id >= doc->font_count) return NULL;
    return &doc->fonts[id];
}

/* Does `name` contain `word`, case ignored? */
static wp_bool name_has(const u8 *name, u32 len, const char *word)
{
    u32 wl = (u32)strlen(word), i, j;

    for (i = 0; i + wl <= len; i++) {
        for (j = 0; j < wl && ascii_lower(name[i + j]) == (u8)word[j]; j++) { }
        if (j == wl) return WP_TRUE;
    }
    return WP_FALSE;
}

u8 wp_font_family_guess(const u8 *name, u32 len)
{
    static const char *const modern[] = { "courier", "mono", "consol", "typewriter", "fixed",
                                          "terminal", "lucida console", "menlo", "monaco", NULL };
    static const char *const roman[] = { "times", "roman", "serif", "georgia", "garamond",
                                         "palatino", "book", "century", "cambria", "dutch",
                                         "new york", "baskerville", "bodoni", NULL };
    static const char *const script[] = { "script", "brush", "chancery", "comic", NULL };
    u32 i;

    if (len == 0) return (u8)WP_FAMILY_NIL;
    for (i = 0; modern[i] != NULL; i++) if (name_has(name, len, modern[i])) return (u8)WP_FAMILY_MODERN;
    /* "sans" before "serif": Microsoft Sans Serif is not a serif. */
    if (name_has(name, len, "sans")) return (u8)WP_FAMILY_SWISS;
    for (i = 0; roman[i] != NULL; i++) if (name_has(name, len, roman[i])) return (u8)WP_FAMILY_ROMAN;
    for (i = 0; script[i] != NULL; i++) if (name_has(name, len, script[i])) return (u8)WP_FAMILY_SCRIPT;
    return (u8)WP_FAMILY_SWISS;
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
    doc->images = NULL;
    doc->image_count = 0;
    doc->image_capacity = 0;
    doc->fonts = NULL;
    doc->font_count = 0;
    doc->font_capacity = 0;
    doc->sections = NULL;
    doc->section_count = 0;
    doc->section_capacity = 0;
    doc->title = 0;
    doc->author = 0;
    doc->modified = 0;

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

wp_bool wp_document_is_modified(const wp_document *doc)
{
    return (doc != NULL && doc->modified != 0) ? WP_TRUE : WP_FALSE;
}

void wp_document_mark_saved(wp_document *doc)
{
    if (doc != NULL) doc->modified = 0;
}

void wp_document_mark_modified(wp_document *doc)
{
    if (doc != NULL) doc->modified = 1;
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
    mem_free(doc->images);
    mem_free(doc->fonts);
    wp_strpool_free(&doc->strings);
}
