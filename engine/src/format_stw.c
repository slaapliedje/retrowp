/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_stw.h"
#include "codepage.h"
#include "drawfile.h"
#include "utf8.h"
#include "strpool.h"
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
#define STW_MAX_OBJECTS 64u

static u16 rd16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }
static u32 rd32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* 1/91.4 inch -- 640 span seven inches -- to twips (15.75 each). */
static i32 units_twips(u32 v) { return (i32)((v * 63u + 2u) / 4u); }
static i32 sunits_twips(i32 v) { return v < 0 ? -units_twips((u32)-v) : units_twips((u32)v); }

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
    u16 objects_seen;  /* placed objects, their headers */
    u32 object_at[STW_MAX_OBJECTS];
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
    m->objects_seen = 0;
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
        if (m->objects_seen < STW_MAX_OBJECTS) m->object_at[m->objects_seen++] = p;
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

/* ---- placed objects -------------------------------------------- */

/* A box, rule or shape Atari Works drew itself, as a drawing w by h
   units: kind 1 a rectangle (a filled bar when 4 units or less either
   way -- a rule), kind 3 an ellipse; outlined in black. */
static void stw_shape(wp_drawing_builder *b, u8 kind, i16 w, i16 h)
{
    i16 v[4];

    v[0] = 0; v[1] = 0; v[2] = w; v[3] = h;
    if (kind == 1 && (w <= 4 || h <= 4)) {
        wp_drawing_pen(b, 0, 1, 0, 0, 0);
        wp_drawing_brush(b, 1, 0, 0, 0);
        wp_drawing_shape(b, WP_DRAW_RECT, v);
        return;
    }
    wp_drawing_pen(b, 1, 1, 0, 0, 0);
    wp_drawing_brush(b, 0, 255, 255, 255);
    wp_drawing_shape(b, kind == 3 ? WP_DRAW_ELLIPSE : WP_DRAW_RECT, v);
}

/* Object `k` as a picture paragraph in a frame, x from the left margin
   and y from its page's top margin. A GEM metafile is played back by
   drawfile.c; an IMG's pixels are in a file of its own and are not
   read (nothing is added for it), nor is anything the reader refuses. */
static wp_status stw_emit_object(stw_ctx *c, u32 k, i32 dy, u8 yref)
{
    const u8 *o = c->m->b + c->m->object_at[k];
    u8 kind = (u8)(o[2] & 0x7Fu);
    i32 x = (i32)(i16)rd16(o + 4);
    i32 w = (i32)rd16(o + 10), h = (i32)rd16(o + 12);
    wp_drawing_builder b;
    wp_drawfile_info info;
    wp_frame_desc fd;
    wp_frame frame;
    wp_image image;
    wp_paragraph *para;
    u32 idx;
    i32 ext_w = w > 0 ? w : 1, ext_h = h > 0 ? h : 1;
    wp_status st;

    if (kind == 6) return WP_OK;
    wp_drawing_builder_init(&b);
    if (kind == 5) {
        if (drawfile_read(o + STW_OBJECT + 4, rd32(o + STW_OBJECT), &b, &info) != WP_OK) {
            wp_drawing_builder_free(&b);
            return WP_OK;
        }
        ext_w = info.ext_w;
        ext_h = info.ext_h;
    } else {
        stw_shape(&b, kind, (i16)ext_w, (i16)ext_h);
    }
    st = b.error;
    if (st == WP_OK) {
        st = wp_document_drawing_add(c->doc, b.bytes, b.length, ext_w, ext_h,
                                     units_twips((u32)(w > 0 ? w : 1)), units_twips((u32)(h > 0 ? h : 1)),
                                     (const char *)(o[16] != 0 ? o + 16 : NULL), &image);
    }
    wp_drawing_builder_free(&b);
    if (st != WP_OK) return st;
    memset(&fd, 0, sizeof(fd));
    fd.x = sunits_twips(x);
    fd.y = sunits_twips(dy);
    fd.w = units_twips((u32)(w > 0 ? w : 1));
    fd.xref = (u8)WP_FRAME_X_MARGIN;
    fd.yref = yref;
    st = wp_document_frame_find_or_add(c->doc, &fd, &frame);
    if (st == WP_OK) st = wp_section_paragraph_add(c->sec, &idx);
    if (st != WP_OK) return st;
    para = wp_section_paragraph_get(c->sec, idx);
    wp_paragraph_pstyle_set(para, c->doc->default_pstyle);
    para->kind = (u8)WP_PARA_IMAGE;
    para->image = image;
    para->frame = frame;
    return WP_OK;
}

/* Where each object goes. y is units down the whole document in the
   line table's own measure (each line its height; a page starts at a
   line whose flag word has bit 0). An object follows the last paragraph
   to end above it, its frame that far below -- so it stays beside its
   text whatever this machine's fonts make of the lines. Where that
   paragraph began on an earlier page, it is placed from its page's top
   margin instead, after the first paragraph to start on that page (a
   frame belongs to the page of the paragraph it follows). */
typedef struct {
    u32 after;   /* the paragraph it follows */
    i32 dy;      /* units below that paragraph, or below the top margin */
    u8 yref;     /* WP_FRAME_Y_PARA or WP_FRAME_Y_MARGIN */
} stw_anchor;

static wp_status stw_anchor_objects(const stw_map *m, stw_anchor *out)
{
    u32 *top = (u32 *)mem_alloc((m->lines + 1u) * sizeof(u32));
    u16 *first = (u16 *)mem_alloc((m->records + 1u) * sizeof(u16));
    u32 i, k, p, nt = 0, nf = 0;

    if (top == NULL || first == NULL) {
        if (top != NULL) mem_free(top);
        if (first != NULL) mem_free(first);
        return WP_NOMEM;
    }
    top[0] = 0;
    for (i = 0; i < m->lines; i++) top[i + 1] = top[i] + rd16(m->b + STW_LINES + 16u * i + 10);
    p = m->rec_at;
    for (k = 0; k < m->records; k++) {   /* each paragraph's first line */
        const u8 *fx = m->b + p + 2u + 2u * nt + STW_FIELD * nf;
        u16 ln = rd16(m->b + p);
        first[k] = (u16)(ln < m->lines ? ln : m->lines);
        nt = rd16(fx + 6);
        nf = rd16(fx + 22);
        p = (u32)(fx - m->b) + STW_REC_FIXED;
    }
    first[m->records] = (u16)m->lines;
    for (i = 0; i < m->objects_seen; i++) {
        i32 y = (i32)rd32(m->b + m->object_at[i] + 6);
        u32 page_line = 0, last = 0xFFFFFFFFuL;
        for (k = 0; k < m->lines; k++) {   /* its page's first line */
            if ((rd16(m->b + STW_LINES + 16u * k + 2) & 1u) != 0) {
                if ((i32)top[k] > y) break;
                page_line = k;
            }
        }
        for (k = 0; k < m->records; k++) {   /* the last paragraph to end above it */
            if ((i32)top[first[k + 1]] > y) break;
            last = k;
        }
        if (last != 0xFFFFFFFFuL && first[last] >= page_line) {
            out[i].after = last;
            out[i].dy = y - (i32)top[first[last + 1]];
            out[i].yref = (u8)WP_FRAME_Y_PARA;
        } else {
            for (k = 0; k + 1 < m->records && first[k] < page_line; k++) {}
            out[i].after = k;
            out[i].dy = y - (i32)top[page_line];
            out[i].yref = (u8)WP_FRAME_Y_MARGIN;
        }
    }
    mem_free(top);
    mem_free(first);
    return WP_OK;
}

/* The objects that follow paragraph `para`, each as a framed picture. */
static wp_status stw_place_objects(stw_ctx *c, const stw_anchor *a, u32 para, wp_bool rest)
{
    u32 k;
    wp_status st;

    for (k = 0; k < c->m->objects_seen; k++) {
        if (a[k].after != para && !(rest && a[k].after > para)) continue;
        st = stw_emit_object(c, k, a[k].dy, a[k].yref);
        if (st != WP_OK) return st;
    }
    return WP_OK;
}

/* A header or footer (72 bytes at `at`, NUL-ended) as RetroWP writes
   one: Atari Works' #C and #R start its centre and right parts, #P (or
   #p) is the page number; RetroWP's parts go between '|', its page
   number is '#'. #D (the date) and #L have nothing to stand for here
   and are left out. */
static wp_status stw_margin_text(wp_document *doc, const u8 *t, wp_span *out)
{
    u8 part[3][96];
    u32 n[3], cur = 0, i, k, len = 0;
    u8 buf[300];

    n[0] = n[1] = n[2] = 0;
    for (i = 0; i < 72 && t[i] != 0; i++) {
        u8 ch = t[i];
        if (ch == '#' && i + 1 < 72 && t[i + 1] != 0) {
            u8 code = t[++i];
            if (code == 'C' || code == 'c') { cur = 1; continue; }
            if (code == 'R' || code == 'r') { cur = 2; continue; }
            if (code == 'P' || code == 'p') { if (n[cur] < 93) part[cur][n[cur]++] = '#'; continue; }
            continue;   /* #D, #L and the rest */
        }
        if (ch == '|') ch = '/';   /* RetroWP's part mark */
        {
            u32 cp = wp_codepage_decode(WP_CODEPAGE_ATARI_ST, ch);
            if (cp == 0xFFFDuL || cp < 0x20u) continue;
            if (n[cur] + 3 < 93) n[cur] += utf8_encode(cp, part[cur] + n[cur]);
        }
    }
    if (n[0] + n[1] + n[2] == 0) return WP_OK;
    /* One part alone is centred, two left and right, three all three. */
    if (n[1] == 0 && n[2] == 0) {
        memcpy(buf, part[0], n[0]); len = n[0];
    } else if (n[1] == 0) {
        memcpy(buf, part[0], n[0]); len = n[0];
        buf[len++] = '|';
        memcpy(buf + len, part[2], n[2]); len += n[2];
    } else {
        for (k = 0; k < 3; k++) {
            if (k > 0) buf[len++] = '|';
            memcpy(buf + len, part[k], n[k]); len += n[k];
        }
    }
    return wp_strpool_append(&doc->strings, buf, len, out);
}

wp_status format_stw_import(const u8 *bytes, u32 len, wp_document *doc)
{
    stw_map m;
    stw_ctx c;
    wp_status st;
    u32 sec_idx, p, r, para_no = 0, next_rec_para, rec_p, nt = 0, nf = 0;
    stw_anchor anchors[STW_MAX_OBJECTS];
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
    st = stw_anchor_objects(&m, anchors);
    if (st != WP_OK) { wp_document_free(doc); return st; }
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

    /* The header (after the page's reals) and the footer (after the
       header's face), each 72 bytes. */
    st = stw_margin_text(doc, bytes + 0x52, &c.sec->header);
    if (st == WP_OK) st = stw_margin_text(doc, bytes + 0xC8, &c.sec->footer);
    if (st != WP_OK) { mem_free(c.scratch); wp_document_free(doc); return st; }

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
            if (st == WP_OK) st = stw_place_objects(&c, anchors, para_no, WP_FALSE);
            para_no++;
            at_para_start = WP_TRUE;
            p++;
            continue;
        }
        if (b == 0x01 && c.field_next < m.fields_seen) {   /* a merge field: «Name» (merge.h) */
            const u8 *f = bytes + m.field_at[c.field_next++];
            u32 n;
            stw_put(&c, 0xABu);
            for (n = 0; n < 32 && f[22 + n] != 0; n++) {
                u32 cp = wp_codepage_decode(WP_CODEPAGE_ATARI_ST, f[22 + n]);
                stw_put(&c, cp == 0xFFFDuL ? (u32)'?' : cp);
            }
            stw_put(&c, 0xBBu);
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
    c.para = NULL;
    if (st == WP_OK) st = stw_place_objects(&c, anchors, para_no, WP_TRUE);   /* any left */
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
