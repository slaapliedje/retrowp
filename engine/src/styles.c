/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "styles.h"
#include "strpool.h"
#include "platform.h"

/* RetroWP's own styles, as offsets from the document's default
   character format (its face, and its size for Normal): a writer's
   usual set, the sizes Word's own defaults use. */
typedef struct {
    const char *name;
    i32 size_twips;      /* 0: the default's */
    u16 flags;           /* wp_style_flags */
    u8  align;
    i32 indent_left, indent_right;
    i32 space_before, space_after;
} builtin_style;

static const builtin_style g_builtin[] = {
    { "Normal",    0,   0,                                0,                0,   0,   0,   0   },
    { "Title",     520, WP_STYLE_BOLD,                    WP_ALIGN_CENTER,  0,   0,   0,   240 },
    { "Heading 1", 320, WP_STYLE_BOLD,                    WP_ALIGN_LEFT,    0,   0,   240, 120 },
    { "Heading 2", 280, WP_STYLE_BOLD,                    WP_ALIGN_LEFT,    0,   0,   200, 80  },
    { "Heading 3", 0,   WP_STYLE_BOLD | WP_STYLE_ITALIC,  WP_ALIGN_LEFT,    0,   0,   160, 60  },
    { "Quotation", 0,   WP_STYLE_ITALIC,                  WP_ALIGN_LEFT,    720, 720, 120, 120 },
    { "Footnote Text", 200, 0,                            WP_ALIGN_LEFT,    0,   0,   0,   0   }
};
#define BUILTINS ((u32)(sizeof(g_builtin) / sizeof(g_builtin[0])))

u32 wp_style_builtin_count(void) { return BUILTINS; }
const char *wp_style_builtin_name(u32 index) { return index < BUILTINS ? g_builtin[index].name : NULL; }

static wp_bool same_name(const u8 *a, u32 alen, const u8 *b, u32 blen)
{
    u32 i;
    if (alen != blen) return WP_FALSE;
    for (i = 0; i < alen; i++) {
        u8 x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (u8)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (u8)(y + 32);
        if (x != y) return WP_FALSE;
    }
    return WP_TRUE;
}

const u8 *wp_style_name(const wp_document *doc, u16 id, u32 *len)
{
    if (id == 0 || id >= doc->named_count) { if (len) *len = 0; return NULL; }
    if (len) *len = doc->named[id].name.length;
    return wp_strpool_get(&doc->strings, doc->named[id].name);
}

u16 wp_style_find(const wp_document *doc, const u8 *name, u32 len)
{
    u16 i;
    while (len > 0 && name[len - 1] == ' ') len--;
    while (len > 0 && name[0] == ' ') { name++; len--; }
    for (i = 1; i < doc->named_count; i++) {
        u32 n;
        const u8 *nm = wp_style_name(doc, i, &n);
        if (nm != NULL && same_name(nm, n, name, len)) return i;
    }
    return 0;
}

/* A new stylesheet entry (no formats yet), its index in *id_out. */
static wp_status add_entry(wp_document *doc, const u8 *name, u32 len, u16 *id_out)
{
    wp_named_style e;
    wp_status st;

    if (doc->named_count == 0xFFFFu) return WP_RANGE;
    if ((u32)doc->named_count + 2 > doc->named_capacity) {
        u32 cap = doc->named_capacity < 8u ? 8u : (u32)doc->named_capacity * 2u;
        void *mem;
        if (cap > 0xFFFFu) cap = 0xFFFFu;
        mem = mem_realloc(doc->named, (u32)doc->named_capacity * (u32)sizeof(wp_named_style),
                          cap * (u32)sizeof(wp_named_style));
        if (mem == NULL) return WP_NOMEM;
        doc->named = (wp_named_style *)mem;
        doc->named_capacity = (u16)cap;
    }
    if (doc->named_count == 0) {
        memset(&doc->named[0], 0, sizeof(wp_named_style));
        doc->named_count = 1;
    }
    memset(&e, 0, sizeof(e));
    st = wp_strpool_append(&doc->strings, name, len, &e.name);
    if (st != WP_OK) return st;
    e.para = doc->default_pstyle;
    e.chars = doc->default_cstyle;
    doc->named[doc->named_count] = e;
    *id_out = doc->named_count++;
    return WP_OK;
}

/* Entry `id` given these formats (its paragraph format carrying id). */
static wp_status set_formats(wp_document *doc, u16 id, const wp_para_style *ps, const wp_char_style *cs)
{
    wp_para_style p = *ps;
    wp_pstyle ph;
    wp_cstyle ch;
    wp_status st;

    p.style_id = id;
    st = wp_document_pstyle_find_or_add(doc, &p, &ph);
    if (st == WP_OK) st = wp_document_cstyle_find_or_add(doc, cs, &ch);
    if (st != WP_OK) return st;
    doc->named[id].para = ph;
    doc->named[id].chars = ch;
    return WP_OK;
}

wp_status wp_style_ensure(wp_document *doc, const u8 *name, u32 len, u16 *id_out)
{
    u32 b;
    u16 id = wp_style_find(doc, name, len);

    if (id != 0) { *id_out = id; return WP_OK; }
    for (b = 0; b < BUILTINS; b++) {
        const builtin_style *bs = &g_builtin[b];
        if (same_name((const u8 *)bs->name, (u32)strlen(bs->name), name, len)) {
            const wp_para_style *dps = wp_document_pstyle_get(doc, doc->default_pstyle);
            const wp_char_style *dcs = wp_document_cstyle_get(doc, doc->default_cstyle);
            wp_para_style ps;
            wp_char_style cs;
            wp_status st;
            if (dps == NULL || dcs == NULL) return WP_ERR;
            ps = *dps;
            cs = *dcs;
            ps.align = bs->align;
            ps.indent_left = bs->indent_left;
            ps.indent_right = bs->indent_right;
            ps.space_before = bs->space_before;
            ps.space_after = bs->space_after;
            if (bs->size_twips != 0) cs.size_twips = bs->size_twips;
            cs.flags = bs->flags;
            st = add_entry(doc, (const u8 *)bs->name, (u32)strlen(bs->name), &id);
            if (st == WP_OK) st = set_formats(doc, id, &ps, &cs);
            if (st == WP_OK) *id_out = id;
            return st;
        }
    }
    return WP_RANGE;
}

wp_status wp_style_define(wp_document *doc, const u8 *name, u32 len, const wp_para_style *ps,
                          const wp_char_style *cs, u16 *id_out)
{
    u16 id;
    wp_status st;

    while (len > 0 && name[len - 1] == ' ') len--;
    while (len > 0 && name[0] == ' ') { name++; len--; }
    if (len == 0 || ps == NULL || cs == NULL) return WP_ERR;
    id = wp_style_find(doc, name, len);
    if (id == 0) {
        st = add_entry(doc, name, len, &id);
        if (st != WP_OK) return st;
    }
    st = set_formats(doc, id, ps, cs);
    if (st == WP_OK) st = wp_style_restyle(doc, id);
    if (st == WP_OK && id_out != NULL) *id_out = id;
    return st;
}

wp_status wp_style_apply(wp_document *doc, u32 si, u32 pi, u16 id)
{
    wp_section *sec;
    wp_paragraph *para;
    u16 ri;

    if (id == 0 || id >= doc->named_count || si >= doc->section_count) return WP_RANGE;
    sec = wp_document_section_get(doc, si);
    if (pi >= sec->para_count) return WP_RANGE;
    para = wp_section_paragraph_get(sec, pi);
    wp_paragraph_pstyle_set(para, doc->named[id].para);   /* a table cell keeps its row: that is the paragraph's */
    for (ri = 0; ri < para->run_count; ri++) wp_run_cstyle_set(para, ri, doc->named[id].chars);
    para->layout_dirty = WP_TRUE;
    return WP_OK;
}

wp_cstyle wp_style_text_cstyle(const wp_document *doc, const wp_paragraph *para)
{
    const wp_para_style *ps = para != NULL ? wp_document_pstyle_get(doc, para->style) : NULL;
    if (ps != NULL && ps->style_id != 0 && ps->style_id < doc->named_count) return doc->named[ps->style_id].chars;
    return doc->default_cstyle;
}

wp_status wp_style_next_pstyle(wp_document *doc, const wp_paragraph *para, wp_pstyle *out)
{
    const wp_para_style *ps = wp_document_pstyle_get(doc, para->style);
    u32 len = 0;
    const u8 *nm;
    u16 normal;
    wp_status st;

    *out = para->style;
    if (ps == NULL || ps->style_id == 0) return WP_OK;
    nm = wp_style_name(doc, ps->style_id, &len);
    if (nm == NULL || !((len == 5 && same_name(nm, 5, (const u8 *)"Title", 5)) ||
                        (len >= 7 && same_name(nm, 7, (const u8 *)"Heading", 7)))) {
        return WP_OK;
    }
    st = wp_style_ensure(doc, (const u8 *)"Normal", 6, &normal);
    if (st == WP_OK) *out = doc->named[normal].para;
    return st;
}

wp_status wp_style_restyle(wp_document *doc, u16 id)
{
    u32 si, pi;
    wp_status st = WP_OK;

    for (si = 0; si < doc->section_count && st == WP_OK; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        for (pi = 0; pi < sec->para_count && st == WP_OK; pi++) {
            const wp_para_style *ps = wp_document_pstyle_get(doc, wp_section_paragraph_get(sec, pi)->style);
            if (ps != NULL && ps->style_id == id) st = wp_style_apply(doc, si, pi, id);
        }
    }
    return st;
}
