/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_stw.h"
#include "codepage.h"
#include "utf8.h"
#include "platform.h"

/* Atari Works .STW. docs/formats/atariworks.md is the map; this file
   follows it in order: the header's page, the paragraph records (each
   one's tab and merge-field counts carried in the record before it),
   the placed objects, the body, the font table. Everything is
   big-endian. */

#define STW_INFO        0x13Eu   /* the info block */
#define STW_LINES       0x170u   /* the line table, 16 bytes a line */
#define STW_REC_FIXED   30u      /* a record's part after its tabs and fields */
#define STW_FIELD       54u      /* a merge-field descriptor */
#define STW_OBJECT      176u     /* a placed object's header */
#define STW_FONT_ENTRY  36u      /* 00, id, name */
#define STW_STYLE       8u       /* 00, font, size, effects, 00 */
#define STW_MAX_FIELDS  64u

static u16 rd16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* 1/91.4 inch -- 640 span seven inches -- to twips (15.75 each). */
static i32 units_twips(u32 v) { return (i32)((v * 63u + 2u) / 4u); }

/* An IEEE real as twips: sign, an exponent, and the mantissa's top 20
   bits with the leading 1, times 1440, without floating point. Version
   11 stores doubles; version 10 singles of four times the inches. */
static i32 real_twips(const u8 *p, wp_bool single)
{
    i32 e;
    u32 m, v;

    if (single) {
        e = (i32)(((p[0] & 0x7Fu) << 1) | (p[1] >> 7));
        if (e == 0) return 0;
        m = 0x100000uL | ((((u32)p[1] & 0x7Fu) << 13) | ((u32)p[2] << 5) | ((u32)p[3] >> 3));
        e -= 127 + 2;   /* four times the inches */
    } else {
        e = (i32)(((p[0] & 0x7Fu) << 4) | (p[1] >> 4));
        if (e == 0) return 0;
        m = 0x100000uL | (((u32)p[1] & 0x0Fu) << 16) | ((u32)p[2] << 8) | (u32)p[3];
        e -= 1023;
    }
    if (e > 4 || e < -20) return 0;   /* not a page's size in inches */
    v = m * 1440u;   /* the top 20 bits are plenty: 1/20 twip */
    v = (v + (1uL << (19 - e))) >> (20 - e);
    return (p[0] & 0x80u) ? -(i32)v : (i32)v;
}

typedef struct {
    const u8 *b;
    u32 len;
    u32 records, lines, objects;
    u32 rec_at;      /* the first paragraph record */
    u32 body;        /* the body's first byte */
    u32 end;         /* the lone 00 after it, where the font table starts */
    u16 fields_seen; /* merge-field descriptors, in order */
    u32 field_at[STW_MAX_FIELDS];
} stw_map;

/* The walk from the records to the body; WP_FALSE where the file is cut
   short or does not add up. Fills map->body and the field list. */
static wp_bool stw_walk(stw_map *m)
{
    u32 p, r, k, nt = 0, nf = 0;

    if (m->len < STW_LINES || m->b[0] != '1' || (m->b[1] != '0' && m->b[1] != '1')) return WP_FALSE;
    m->records = rd16(m->b + STW_INFO + 8);
    m->lines = rd16(m->b + STW_INFO + 10);
    m->objects = rd16(m->b + STW_INFO + 12);
    m->fields_seen = 0;
    p = STW_LINES + 16u * m->lines;
    m->rec_at = p;
    for (r = 0; r <= m->records; r++) {
        /* records, then the paragraph-count word with the last record's
           carried tabs and fields after it */
        u32 at = p + 2u + 2u * nt;
        if (at + STW_FIELD * nf > m->len) return WP_FALSE;
        for (k = 0; k < nf; k++) {
            if (m->fields_seen < STW_MAX_FIELDS) m->field_at[m->fields_seen++] = at + STW_FIELD * k;
        }
        at += STW_FIELD * nf;
        if (r == m->records) { p = at; break; }
        if (at + STW_REC_FIXED > m->len) return WP_FALSE;
        nt = rd16(m->b + at + 6);
        nf = rd16(m->b + at + 22);
        if (nt > 64 || nf > 64) return WP_FALSE;
        p = at + STW_REC_FIXED;
    }
    for (r = 0; r < m->objects; r++) {
        u8 kind;
        if (p + STW_OBJECT > m->len) return WP_FALSE;
        kind = (u8)(m->b[p + 2] & 0x7Fu);
        if (kind == 5 || kind == 6) {   /* a GEM metafile or IMG: a length, then it */
            u32 n;
            if (p + STW_OBJECT + 4 > m->len) return WP_FALSE;
            n = rd32(m->b + p + STW_OBJECT);
            if (n > m->len - p - STW_OBJECT - 4) return WP_FALSE;
            p += STW_OBJECT + 4 + n;
        } else {
            p += STW_OBJECT;
        }
    }
    m->body = p;
    /* The end: a lone 00, then the font table's first entry -- 00, an
       id, a printable name (a style change has a size there). */
    while (p < m->len) {
        if (m->b[p] == 0) {
            if (p + 4 < m->len && m->b[p + 1] == 0 && m->b[p + 4] > 0x20 && m->b[p + 4] < 0x7F) break;
            p += STW_STYLE;
            continue;
        }
        p++;
    }
    if (p >= m->len) return WP_FALSE;   /* no end marker: cut short */
    m->end = p;
    return WP_TRUE;
}

wp_bool format_stw_looks_like(const u8 *bytes, u32 len)
{
    stw_map m;

    if (bytes == NULL) return WP_FALSE;
    m.b = bytes;
    m.len = len;
    return stw_walk(&m);
}

/* ---- building ------------------------------------------------------ */

typedef struct {
    wp_document *doc;
    wp_section *sec;
    wp_paragraph *para;
    const stw_map *m;
    u8 *scratch;
    u32 scratch_len;
    u16 font_id, size, effects;   /* the style the next text takes */
    u16 base_font, base_size, base_effects;   /* the governing record's: each paragraph starts here */
    u32 field_next;
} stw_ctx;

/* The font table's name for `id`; len 0 when it has none. */
static const u8 *font_name(const stw_map *m, u16 id, u32 *len)
{
    u32 q;

    for (q = m->end + 1; q + 4 <= m->len; q += STW_FONT_ENTRY) {
        if (rd16(m->b + q + 1) == id) {
            const u8 *nm = m->b + q + 3;
            u32 room = m->len - q - 3, n = 0;
            if (room > STW_FONT_ENTRY - 3) room = STW_FONT_ENTRY - 3;
            while (n < room && nm[n] != 0) n++;
            *len = n;
            return nm;
        }
    }
    *len = 0;
    return NULL;
}

static wp_bool ends_with(const u8 *s, u32 n, const char *suffix, u32 *cut)
{
    u32 k = (u32)strlen(suffix);
    if (n < k || memcmp(s + n - k, suffix, k) != 0) return WP_FALSE;
    *cut = n - k;
    return WP_TRUE;
}

/* The run style: the face named for font_id less a weight or slant in
   its name ("Humanist 521 Bold Italic" is Humanist 521, bold, italic),
   the size in points, GEM effects 1 bold and 8 underline. */
static wp_status stw_style(stw_ctx *c, wp_cstyle *out)
{
    const wp_char_style *base = wp_document_cstyle_get(c->doc, c->doc->default_cstyle);
    wp_char_style cs;
    const u8 *nm;
    u32 n = 0, cut;
    wp_status st;

    if (base == NULL) return WP_ERR;
    cs = *base;
    cs.flags = 0;
    if (c->effects & 1u) cs.flags |= WP_STYLE_BOLD;
    if (c->effects & 8u) cs.flags |= WP_STYLE_UNDERLINE;
    if (c->size >= 4 && c->size <= 144) cs.size_twips = (i32)c->size * 20;
    nm = font_name(c->m, c->font_id, &n);
    if (nm != NULL && n > 0) {
        if (ends_with(nm, n, " Italic", &cut) || ends_with(nm, n, " Oblique", &cut)) {
            cs.flags |= WP_STYLE_ITALIC;
            n = cut;
        }
        if (ends_with(nm, n, " Bold", &cut) || ends_with(nm, n, " Black", &cut)) {
            cs.flags |= WP_STYLE_BOLD;
            n = cut;
        }
        st = wp_document_font_find_or_add(c->doc, nm, n, wp_font_family_guess(nm, n), &cs.font);
        if (st != WP_OK) return st;
    }
    return wp_document_cstyle_find_or_add(c->doc, &cs, out);
}

static wp_status stw_open_para(stw_ctx *c, wp_pstyle ps)
{
    u32 idx;
    wp_status st;

    if (c->para != NULL) return WP_OK;
    st = wp_section_paragraph_add(c->sec, &idx);
    if (st != WP_OK) return st;
    c->para = wp_section_paragraph_get(c->sec, idx);
    wp_paragraph_pstyle_set(c->para, ps);
    return WP_OK;
}

static wp_status stw_flush(stw_ctx *c, wp_pstyle ps)
{
    wp_cstyle handle;
    wp_span span;
    u16 run;
    wp_status st;

    if (c->scratch_len == 0) return WP_OK;
    st = stw_open_para(c, ps);
    if (st == WP_OK) st = stw_style(c, &handle);
    if (st == WP_OK) st = wp_strpool_append(&c->doc->strings, c->scratch, c->scratch_len, &span);
    if (st == WP_OK) st = wp_paragraph_run_add(c->para, span, handle, &run);
    c->scratch_len = 0;
    return st;
}

static void stw_put(stw_ctx *c, u32 cp)
{
    u8 enc[4];
    u32 n = utf8_encode(cp, enc), i;
    for (i = 0; i < n; i++) c->scratch[c->scratch_len++] = enc[i];
}

/* A paragraph record's style: alignment and line spacing from the
   format word, indents (the right edge measured from the left margin),
   tab stops with their kinds in the top two bits. */
static wp_status stw_record_pstyle(stw_ctx *c, const u8 *tabs, u32 nt, const u8 *fx,
                                   wp_pstyle *out)
{
    const wp_para_style *base = wp_document_pstyle_get(c->doc, c->doc->default_pstyle);
    wp_para_style ps;
    u32 fmt = fx[8], k;
    i32 column = c->sec->page_width - c->sec->margin_left - c->sec->margin_right;
    i32 right_edge = units_twips(rd16(fx + 4));

    if (base == NULL) return WP_ERR;
    ps = *base;
    switch (fmt & 0x0Fu) {
    case 2: ps.align = (u8)WP_ALIGN_RIGHT; break;
    case 4: ps.align = (u8)WP_ALIGN_CENTER; break;
    case 8: ps.align = (u8)WP_ALIGN_JUSTIFY; break;
    default: ps.align = (u8)WP_ALIGN_LEFT; break;
    }
    switch (fmt >> 4) {
    case 2: ps.line_spacing = 360; break;   /* one and a half */
    case 4: ps.line_spacing = 480; break;   /* double */
    default: ps.line_spacing = 0; break;
    }
    ps.indent_left = units_twips(rd16(fx));
    ps.indent_first = units_twips(rd16(fx + 2)) - ps.indent_left;
    ps.indent_right = (right_edge > 0 && right_edge < column) ? column - right_edge : 0;
    ps.tab_count = 0;
    for (k = 0; k < nt && ps.tab_count < WP_MAX_TABS; k++) {
        static const u8 kinds[4] = { WP_TAB_LEFT, WP_TAB_RIGHT, WP_TAB_CENTER, WP_TAB_DECIMAL };
        u16 v = rd16(tabs + 2u * k);
        ps.tab_pos[ps.tab_count] = units_twips(v & 0x3FFFu);
        ps.tab_kind[ps.tab_count] = kinds[v >> 14];
        ps.tab_count++;
    }
    return wp_document_pstyle_find_or_add(c->doc, &ps, out);
}

wp_status format_stw_import(const u8 *bytes, u32 len, wp_document *doc)
{
    stw_map m;
    stw_ctx c;
    wp_status st;
    u32 sec_idx, p, r, para_no = 0, next_rec_para, rec_p, nt = 0, nf = 0;
    wp_bool single, at_para_start = WP_TRUE;
    wp_pstyle cur_ps;

    if (bytes == NULL || doc == NULL) return WP_ERR;
    m.b = bytes;
    m.len = len;
    if (!stw_walk(&m)) return WP_ERR;

    st = wp_document_init(doc);
    if (st != WP_OK) return st;
    st = wp_document_section_add(doc, &sec_idx);
    if (st != WP_OK) { wp_document_free(doc); return st; }
    c.doc = doc;
    c.sec = wp_document_section_get(doc, sec_idx);
    c.para = NULL;
    c.m = &m;
    c.scratch_len = 0;
    c.font_id = c.base_font = 0;
    c.size = c.base_size = 12;
    c.effects = c.base_effects = 0;
    c.field_next = 0;
    /* A run is never longer than the body, and each byte is at most a
       three-byte character -- or a merge field's name, 48 at most. */
    c.scratch = (u8 *)mem_alloc((m.end - m.body) * 3u + STW_MAX_FIELDS * 48u + 4u);
    if (c.scratch == NULL) { wp_document_free(doc); return WP_NOMEM; }

    /* The page: paper and margins, both versions. */
    single = (wp_bool)(bytes[1] == '0');
    {
        i32 v[6];
        for (r = 0; r < 6; r++) v[r] = real_twips(bytes + 0x22u + 8u * r, single);
        if (v[0] > 1440 && v[1] > 1440) {
            c.sec->page_width = v[0];
            c.sec->page_height = v[1];
            c.sec->margin_left = v[2];
            c.sec->margin_right = v[3];
            c.sec->margin_top = v[4];
            c.sec->margin_bottom = v[5];
        }
    }

    /* The body, a paragraph at a time, each in its own record's style
       (a record's first word is its first line's index in the line
       table, not a paragraph number: there is one record a paragraph). */
    cur_ps = doc->default_pstyle;
    rec_p = m.rec_at;
    r = 0;
    next_rec_para = 0;   /* record r is paragraph r's */
    p = m.body;
    st = WP_OK;
    while (st == WP_OK && p < m.end) {
        u8 b = bytes[p];

        while (st == WP_OK && r < m.records && next_rec_para <= para_no && c.para == NULL &&
               c.scratch_len == 0) {
            const u8 *tabs = bytes + rec_p + 2;
            const u8 *fx = tabs + 2u * nt + STW_FIELD * nf;
            st = stw_record_pstyle(&c, tabs, nt, fx, &cur_ps);
            c.base_font = rd16(fx + 10);
            c.base_size = rd16(fx + 12);
            c.base_effects = rd16(fx + 14);
            nt = rd16(fx + 6);
            nf = rd16(fx + 22);
            rec_p = (u32)(fx - bytes) + STW_REC_FIXED;
            r++;
            next_rec_para = r;
        }
        if (st != WP_OK) break;
        if (c.para == NULL && c.scratch_len == 0 && at_para_start) {
            /* A paragraph starts in its record's style; a change inside
               one lasts to its end (Atari Works' own RTF export says
               \plain and the base style at every paragraph). */
            c.font_id = c.base_font;
            c.size = c.base_size;
            c.effects = c.base_effects;
            at_para_start = WP_FALSE;
        }
        if (b == 0) {   /* a style change */
            if (p + STW_STYLE > m.end) break;
            st = stw_flush(&c, cur_ps);
            c.font_id = rd16(bytes + p + 1);
            c.size = rd16(bytes + p + 3);
            c.effects = rd16(bytes + p + 5);
            p += STW_STYLE;
            continue;
        }
        if (b == 0x0A) {   /* a paragraph ends */
            st = stw_flush(&c, cur_ps);
            if (st == WP_OK) st = stw_open_para(&c, cur_ps);
            c.para = NULL;
            para_no++;
            at_para_start = WP_TRUE;
            p++;
            continue;
        }
        if (b == 0x01 && c.field_next < m.fields_seen) {   /* a merge field: its name */
            const u8 *f = bytes + m.field_at[c.field_next++];
            u32 n;
            for (n = 0; n < 14 && f[8 + n] != 0; n++) stw_put(&c, f[8 + n]);
            stw_put(&c, ':');
            for (n = 0; n < 32 && f[22 + n] != 0; n++) stw_put(&c, f[22 + n]);
            p++;
            continue;
        }
        if (b == 0x09 || b >= 0x20) {
            u32 cp = wp_codepage_decode(WP_CODEPAGE_ATARI_ST, b);
            stw_put(&c, cp == 0xFFFDuL ? (u32)'?' : cp);
        }
        p++;
    }
    if (st == WP_OK) st = stw_flush(&c, cur_ps);
    mem_free(c.scratch);
    if (st != WP_OK) {
        wp_document_free(doc);
        return st;
    }
    if (c.sec->para_count == 0) {   /* an empty document still has a paragraph */
        st = stw_open_para(&c, cur_ps);
        if (st != WP_OK) { wp_document_free(doc); return st; }
    }
    return WP_OK;
}
