/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_rtf.h"
#include "platform.h"
#include "utf8.h"
#include "growth_internal.h"
#include "format_rtf_internal.h"
#include "codepage.h"

/* Classic RTF 1.x import. Full scope, supported control words, and
   documented simplifications are in docs/formats/rtf.md -- read it
   before changing this file. */

#define RTF_MAX_DEPTH 64

typedef enum {
    RTF_DEST_NORMAL = 0,
    RTF_DEST_SKIP,       /* ignorable destination: fonttbl, info, etc. */
    RTF_DEST_COLORTBL,
    RTF_DEST_PICT,       /* \pict -- hex picture data, not document text */
    RTF_DEST_HEADER,     /* \header/\headerr: captured as the section's header */
    RTF_DEST_FOOTER,     /* \footer/\footerr: its footer */
    RTF_DEST_FLDINST,    /* a field's instruction inside one of those: is it PAGE? */
    RTF_DEST_FONTTBL,    /* \fonttbl: each entry's name becomes a document font */
    RTF_DEST_GENERATOR   /* \*\generator: which program wrote the file */
} rtf_dest;

/* Fonts a \fonttbl can name before the rest are read as the default.
   Word writes a dozen or two; one per script it ever saw. */
#define RTF_MAX_FONTS 64

typedef struct {
    u16 char_flags;    /* wp_style_flags bitfield */
    i32 size_twips;
    u8  color_r, color_g, color_b;
    u8  align;          /* wp_align */
    i32 indent_left, indent_right, indent_first;
    i32 space_before, space_after;
    i32 sl;             /* \slN, 0 = single */
    u8  slmult;         /* \slmultN: 1 = sl is a multiple (240ths), 0 = twips */
    u8  uc;             /* \ucN: fallback characters after each \uN */
    u8  hidden;         /* \v: text not imported */
    u8  pagebb;         /* \pagebb: this paragraph starts a page */
    u8  fidx;           /* \fN: its fmap entry + 1; 0 = the target default */
    u16 cp;             /* that font's \fcharset as a code page; 0 = the document's */
    rtf_dest dest;
} rtf_state;

typedef struct {
    u8 *bytes;
    u32 length;
    u32 capacity;
} rtf_buf;

typedef struct {
    const u8 *src;
    u32 len;
    u32 pos;

    rtf_state stack[RTF_MAX_DEPTH];
    u32 depth;

    u8  color_table[RTF_MAX_COLORS][3];
    u32 color_count;
    u8  pending_r, pending_g, pending_b;

    wp_document *doc;
    u32 section_idx;
    u32 para_idx;
    wp_bool para_pending; /* next content must open a fresh paragraph */

    rtf_buf run_buf;   /* accumulated decoded bytes for the run in progress */

    /* The \pict in progress.  The hex is decoded as it arrives rather
       than kept as text, and the whole DIB is held because its rows
       arrive bottom-up and the model wants them top-down -- the reversal
       needs the last row before it can write the first. */
    rtf_buf  pict_buf;
    wp_bool  pict_is_dib;
    wp_bool  pict_nibble_pending;
    u8       pict_nibble;
    i32      pict_w, pict_h;
    i32      pict_goal_w, pict_goal_h;
    const wp_image_sink *sink;

    u16 codepage;      /* \ansicpgN / \mac / \pc / \pca; \'hh decodes through it */
    u32 skip;          /* fallback characters still to skip after a \uN */
    u8  break_next;    /* a \page was seen: the next paragraph starts a page */

    /* Sections. A \sect leaves the next section PENDING: it is opened
       (rtf_open_section) by the first thing that belongs to it -- a
       paragraph, its page setup, its header or footer, \sectd -- so a
       file that ends on \sect has no empty last section. A new section
       inherits the previous one's page and header and footer, as Word's
       "same as previous" does, until it says otherwise; \sectd puts the
       page back to the document's (\paperw...). */
    u8  sect_pending;
    u8  sec_has_header, sec_has_footer;   /* this section's own, read already */
    i32 doc_page[6];   /* \paperw \paperh \margl \margr \margt \margb */
    u8  star_pending;  /* \* just seen: the next control word may be one we read anyway */
    u8  fld_is_page;   /* the last field instruction was PAGE */
    rtf_buf margin_buf;   /* the header or footer being read */
    rtf_buf fld_buf;      /* the field instruction being read */
    u8  break_this;    /* ...and this is that paragraph */

    /* \fonttbl: the entry being read, and each entry's number, name (in
       ft_names), family and code page. A face reaches the document's
       font table only when a run first uses it (rtf_flush_run): Word
       lists every font it ever saw, and this program's own \f0 is a
       placeholder the \*\generator after the table reveals. The default
       (\deffN) is what \plain returns to. */
    struct { i32 num; u32 name_off, name_len; u8 family; u16 cp; wp_font id; wp_bool resolved; } fmap[RTF_MAX_FONTS];
    u32 fmap_count;
    rtf_buf ft_names;
    i32 ft_num;
    u8  ft_family;
    u16 ft_cp;
    rtf_buf ft_name;
    i32 deff;
    wp_bool ours;      /* \*\generator says RetroWP: its \f0 is the target default */
    char gen[16];
    u32  gen_len;

    /* Tab stops of the paragraph being read. Kept here, not in the group
       state: \pard resets them and nothing scopes them to a group, and
       twelve stops in each of 64 nested states would put kilobytes more
       on a stack this parser no longer lives on, but did. */
    u8  tab_count;
    u8  tab_kind_pending;
    u8  tab_kind[WP_MAX_TABS];
    i32 tab_pos[WP_MAX_TABS];

    wp_status error;   /* sticky first error; once set, parsing short-circuits */
} rtf_parser;

/* -- byte classification (plain ASCII only -- RTF control words are
   always ASCII; avoids any libc ctype locale surprises) ---------------- */

static wp_bool is_alpha(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static wp_bool is_digit(u8 c) { return c >= '0' && c <= '9'; }
static wp_bool is_hex(u8 c)   { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static u32 hex_val(u8 c)
{
    if (c >= '0' && c <= '9') return (u32)(c - '0');
    if (c >= 'a' && c <= 'f') return (u32)(c - 'a' + 10);
    return (u32)(c - 'A' + 10);
}

/* -- growable byte buffer (run text accumulator) ----------------------- */

static wp_status rtf_buf_append_byte(rtf_buf *buf, u8 byte)
{
    if (buf->length >= buf->capacity) {
        u32 new_cap = wp_grow_capacity(buf->capacity, buf->length + 1, 64);
        u8 *new_bytes = (u8 *)mem_realloc(buf->bytes, buf->capacity, new_cap);
        if (new_bytes == NULL) return WP_NOMEM;
        buf->bytes = new_bytes;
        buf->capacity = new_cap;
    }
    buf->bytes[buf->length++] = byte;
    return WP_OK;
}

static void rtf_buf_free(rtf_buf *buf)
{
    mem_free(buf->bytes);
    buf->bytes = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

/* -- paragraph/run construction ---------------------------------------- */

static void rtf_open_section(rtf_parser *p)
{
    wp_section prev, *sec;
    u32 idx;

    if (p->error != WP_OK || !p->sect_pending) return;
    p->sect_pending = 0;
    prev = *wp_document_section_get(p->doc, p->section_idx);
    if (wp_document_section_add(p->doc, &idx) != WP_OK) { p->error = WP_NOMEM; return; }
    sec = wp_document_section_get(p->doc, idx);
    sec->page_width = prev.page_width;
    sec->page_height = prev.page_height;
    sec->margin_left = prev.margin_left;
    sec->margin_right = prev.margin_right;
    sec->margin_top = prev.margin_top;
    sec->margin_bottom = prev.margin_bottom;
    sec->header = prev.header;
    sec->footer = prev.footer;
    p->section_idx = idx;
    p->sec_has_header = p->sec_has_footer = 0;
    p->para_pending = WP_TRUE;
}

static void rtf_ensure_paragraph(rtf_parser *p)
{
    wp_section *sec;
    u32 new_idx;

    if (p->error != WP_OK || !p->para_pending) return;
    rtf_open_section(p);
    if (p->error != WP_OK) return;

    sec = wp_document_section_get(p->doc, p->section_idx);
    if (wp_section_paragraph_add(sec, &new_idx) != WP_OK) { p->error = WP_NOMEM; return; }
    p->para_idx = new_idx;
    p->para_pending = WP_FALSE;
    p->break_this = p->break_next;
    p->break_next = 0;
}

static void rtf_flush_run(rtf_parser *p)
{
    rtf_state *st;
    wp_span span;
    wp_cstyle cstyle;
    wp_char_style cs;
    wp_section *sec;
    wp_paragraph *para;
    u16 run_idx;

    if (p->error != WP_OK || p->run_buf.length == 0) return;

    st = &p->stack[p->depth];

    if (wp_strpool_append(&p->doc->strings, p->run_buf.bytes, p->run_buf.length, &span) != WP_OK) {
        p->error = WP_NOMEM;
        return;
    }
    p->run_buf.length = 0;

    memset(&cs, 0, sizeof(cs));
    cs.flags = st->char_flags;
    cs.size_twips = st->size_twips;
    cs.color_r = st->color_r;
    cs.color_g = st->color_g;
    cs.color_b = st->color_b;
    if (st->fidx > 0) {
        u32 fi = (u32)st->fidx - 1;
        if (!p->fmap[fi].resolved) {
            if (wp_document_font_find_or_add(p->doc, p->ft_names.bytes + p->fmap[fi].name_off,
                                             p->fmap[fi].name_len, p->fmap[fi].family,
                                             &p->fmap[fi].id) != WP_OK) { p->error = WP_NOMEM; return; }
            p->fmap[fi].resolved = WP_TRUE;
        }
        cs.font = p->fmap[fi].id;
    }

    if (wp_document_cstyle_add(p->doc, &cs, &cstyle) != WP_OK) { p->error = WP_NOMEM; return; }

    sec = wp_document_section_get(p->doc, p->section_idx);
    para = wp_section_paragraph_get(sec, p->para_idx);
    if (wp_paragraph_run_add(para, span, cstyle, &run_idx) != WP_OK) { p->error = WP_NOMEM; return; }
}

static void rtf_apply_pstyle_to_current_paragraph(rtf_parser *p)
{
    rtf_state *st = &p->stack[p->depth];
    wp_pstyle pstyle;
    wp_para_style ps;
    wp_section *sec;
    wp_paragraph *para;

    memset(&ps, 0, sizeof(ps));
    ps.align = st->align;
    ps.indent_left = st->indent_left;
    ps.indent_right = st->indent_right;
    ps.indent_first = st->indent_first;
    ps.space_before = st->space_before;
    ps.space_after = st->space_after;
    /* RTF's line spacing to the model's (docmodel.h): \slN\slmult1 is
       a multiple in 240ths, \slN\slmult0 "at least N" twips, and a
       negative \sl ("exactly") is read as at least. 240ths of exactly
       one line is single. */
    if (st->sl != 0) {
        if (st->slmult && st->sl > 0) ps.line_spacing = (st->sl == 240) ? 0 : st->sl;
        else ps.line_spacing = -(st->sl < 0 ? -st->sl : st->sl);
    }
    ps.page_break_before = (u8)(st->pagebb || p->break_this);
    p->break_this = 0;
    ps.tab_count = p->tab_count;
    memcpy(ps.tab_kind, p->tab_kind, sizeof(ps.tab_kind));
    memcpy(ps.tab_pos, p->tab_pos, sizeof(ps.tab_pos));

    /* Find-or-add, so a document of a thousand alike paragraphs has one
       style, not a thousand. */
    if (wp_document_pstyle_find_or_add(p->doc, &ps, &pstyle) != WP_OK) { p->error = WP_NOMEM; return; }

    sec = wp_document_section_get(p->doc, p->section_idx);
    para = wp_section_paragraph_get(sec, p->para_idx);
    wp_paragraph_pstyle_set(para, pstyle);
}

/* \par and \line both end the current paragraph -- see docs/formats/rtf.md
   on why \line (a soft break) is approximated this way: the document
   model has no concept of a break within a paragraph. */
static void rtf_end_paragraph(rtf_parser *p)
{
    if (p->error != WP_OK) return;

    rtf_flush_run(p);
    if (p->error != WP_OK) return;

    rtf_ensure_paragraph(p); /* handles back-to-back \par (blank line) */
    if (p->error != WP_OK) return;

    rtf_apply_pstyle_to_current_paragraph(p);
    if (p->error != WP_OK) return;

    p->para_pending = WP_TRUE;
}

static void rtf_colortbl_commit(rtf_parser *p)
{
    if (p->color_count >= RTF_MAX_COLORS) { p->error = WP_RANGE; return; }
    p->color_table[p->color_count][0] = p->pending_r;
    p->color_table[p->color_count][1] = p->pending_g;
    p->color_table[p->color_count][2] = p->pending_b;
    p->color_count++;
    p->pending_r = p->pending_g = p->pending_b = 0;
}

/* \fcharsetN as one of the code pages codepage.c can decode; 0 for the
   document's own (ANSI, "default", and anything unknown -- whose \'hh
   bytes then decode as they did before fonts were read at all). */
static u16 rtf_charset_cp(i32 charset)
{
    u16 cp = 0;

    switch (charset) {
    case 77:  cp = 10000; break;   /* Mac Roman */
    case 204: cp = 1251; break;    /* Cyrillic */
    case 238: cp = 1250; break;    /* Central European */
    case 255: cp = 437; break;     /* OEM */
    default:  break;
    }
    return (cp != 0 && wp_codepage_known(cp)) ? cp : (u16)0;
}

/* One \fonttbl entry is complete (its ';', or its group closing). */
static void rtf_fonttbl_commit(rtf_parser *p)
{
    u32 len = p->ft_name.length, i;
    const u8 *name = p->ft_name.bytes;

    while (len > 0 && name[0] == ' ') { name++; len--; }
    while (len > 0 && name[len - 1] == ' ') len--;
    if (len > 0 && p->ft_num >= 0 && p->fmap_count < RTF_MAX_FONTS) {
        u32 at = p->fmap_count;
        p->fmap[at].num = p->ft_num;
        p->fmap[at].name_off = p->ft_names.length;
        p->fmap[at].name_len = len;
        p->fmap[at].family = p->ft_family;
        p->fmap[at].cp = p->ft_cp;
        p->fmap[at].id = 0;
        p->fmap[at].resolved = WP_FALSE;
        for (i = 0; i < len; i++) {
            if (rtf_buf_append_byte(&p->ft_names, name[i]) != WP_OK) { p->error = WP_NOMEM; return; }
        }
        p->fmap_count++;
    }
    p->ft_name.length = 0;
    p->ft_num = -1;
    p->ft_family = 0;
    p->ft_cp = 0;
}

/* RTF font number `num` into st's font and code page; an unknown number
   is the target default. In a file this program wrote, the default
   font number IS the target default (rtf_emit_header's placeholder). */
static void rtf_select_font(rtf_parser *p, rtf_state *st, i32 num)
{
    u32 i;

    st->fidx = 0;
    st->cp = 0;
    for (i = 0; i < p->fmap_count; i++) {
        if (p->fmap[i].num == num) {
            if (!(p->ours && num == p->deff)) st->fidx = (u8)(i + 1);
            st->cp = p->fmap[i].cp;
            return;
        }
    }
}

static void rtf_emit_bytes(rtf_parser *p, const u8 *bytes, u32 n)
{
    rtf_state *st;
    u32 i;

    if (p->error != WP_OK) return;
    st = &p->stack[p->depth];

    if (st->dest == RTF_DEST_SKIP) return;

    if (st->dest == RTF_DEST_NORMAL && st->hidden) return;   /* \v */

    if (st->dest == RTF_DEST_HEADER || st->dest == RTF_DEST_FOOTER || st->dest == RTF_DEST_FLDINST) {
        rtf_buf *b = (st->dest == RTF_DEST_FLDINST) ? &p->fld_buf : &p->margin_buf;
        if (st->hidden) return;
        for (i = 0; i < n; i++) {
            /* A literal '|' or '#' would read back as a part break or a
               page number: keep it as text that cannot (a broken bar,
               a number sign's fullwidth twin would be worse). */
            u8 c = bytes[i];
            if (b == &p->margin_buf && (c == '|' || c == '#')) c = (c == '|') ? '/' : 'N';
            if (rtf_buf_append_byte(b, c) != WP_OK) { p->error = WP_NOMEM; return; }
        }
        return;
    }

    if (st->dest == RTF_DEST_COLORTBL) {
        if (n == 1 && bytes[0] == ';') rtf_colortbl_commit(p);
        return;
    }

    if (st->dest == RTF_DEST_FONTTBL) {
        for (i = 0; i < n; i++) {
            if (bytes[i] == ';') { rtf_fonttbl_commit(p); continue; }
            if (rtf_buf_append_byte(&p->ft_name, bytes[i]) != WP_OK) { p->error = WP_NOMEM; return; }
        }
        return;
    }

    if (st->dest == RTF_DEST_GENERATOR) {
        for (i = 0; i < n && p->gen_len + 1 < sizeof(p->gen); i++) p->gen[p->gen_len++] = (char)bytes[i];
        return;
    }

    if (st->dest == RTF_DEST_PICT) {
        /* Everything reaching the text path inside \pict is picture
           data: the control words were consumed before it.  Decoded a
           nibble at a time so that whitespace anywhere in the hex --
           which every emitter inserts, this one included -- costs
           nothing to tolerate. */
        for (i = 0; i < n; i++) {
            u8 c = bytes[i];
            if (!is_hex(c)) continue;
            if (!p->pict_nibble_pending) {
                p->pict_nibble = (u8)hex_val(c);
                p->pict_nibble_pending = WP_TRUE;
            } else {
                u8 byte = (u8)(p->pict_nibble * 16 + hex_val(c));
                p->pict_nibble_pending = WP_FALSE;
                if (rtf_buf_append_byte(&p->pict_buf, byte) != WP_OK) {
                    p->error = WP_NOMEM;
                    return;
                }
            }
        }
        return;
    }

    rtf_ensure_paragraph(p);
    if (p->error != WP_OK) return;

    for (i = 0; i < n; i++) {
        if (rtf_buf_append_byte(&p->run_buf, bytes[i]) != WP_OK) { p->error = WP_NOMEM; return; }
    }
}

static void rtf_emit_char(rtf_parser *p, u8 c) { rtf_emit_bytes(p, &c, 1); }

static void rtf_emit_cp(rtf_parser *p, u32 cp)
{
    u8 buf[4];
    u32 n = utf8_encode(cp, buf);
    if (n == 0) return;
    rtf_emit_bytes(p, buf, n);
}

/* A byte of 8-bit text in the document's code page (\'hh, or a raw
   high byte in the source). */
static void rtf_emit_cp_byte(rtf_parser *p, u8 byte_val)
{
    u16 cp = p->stack[p->depth].cp;

    if (byte_val < 0x80) { rtf_emit_char(p, byte_val); return; }
    rtf_emit_cp(p, wp_codepage_decode(cp != 0 ? cp : p->codepage, byte_val));
}

/* One character of document text from the source -- a literal byte or
   a \'hh -- unless it is a \uN's fallback, which is skipped. */
static void rtf_source_char(rtf_parser *p, u8 byte_val)
{
    if (p->skip > 0) { p->skip--; return; }
    rtf_emit_cp_byte(p, byte_val);
}

/* -- group nesting -------------------------------------------------------
   ob_next/head/tail-style bugs don't apply here, but the equivalent
   footgun does: a fresh '{' inherits the WHOLE enclosing state
   (including dest), which is what makes "ignorable destination content
   is skipped, including any nested groups inside it" and "character
   formatting nests" both fall out for free -- neither needs special
   casing beyond this one copy. */
static void rtf_push(rtf_parser *p)
{
    if (p->error != WP_OK) return;
    if (p->depth + 1 >= RTF_MAX_DEPTH) { p->error = WP_RANGE; return; }
    p->stack[p->depth + 1] = p->stack[p->depth];
    p->depth++;
    p->skip = 0;   /* a \uN's fallback never spans a group boundary */
}

/* -- \pict -------------------------------------------------------------
 *
 * A Windows DIB, which is what this project's own exporter writes and
 * what a foreign word processor is most likely to have put there for a
 * palette image.  Three things have to be undone, each the inverse of a
 * step format_rtf_export.c documents: the fields are little-endian, a
 * positive biHeight means the rows are stored bottom-up, and each row is
 * padded to four bytes where the model's are packed tight.
 *
 * Anything unsupported -- a compression other than BI_RGB, a depth other
 * than 4, a truncated buffer -- drops the picture and keeps the
 * document.  A file whose picture this parser cannot read is still a
 * file whose text is worth having. */

static u32 dib_rd32(const u8 *p) /* little-endian, unlike this project's own formats */
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 dib_rd16(const u8 *p)
{
    return (u16)((u16)p[0] | ((u16)p[1] << 8));
}

static void rtf_finish_pict(rtf_parser *p)
{
    const u8 *dib = p->pict_buf.bytes;
    u32 len = p->pict_buf.length;
    u32 hdr, clr_used, row_bytes, padded, pixel_off;
    i32 w, h;
    u16 bits;
    wp_image_desc desc;
    wp_image handle;
    char path[128];
    u32 path_len = 0;
    u32 i;
    wp_section *sec;
    u32 new_idx;
    wp_paragraph *para;

    p->pict_nibble_pending = WP_FALSE;
    if (p->error != WP_OK || p->sink == NULL || !p->pict_is_dib) return;
    if (len < 40u) return;

    hdr = dib_rd32(dib + 0);
    if (hdr < 40u || hdr > len) return;
    w = (i32)dib_rd32(dib + 4);
    h = (i32)dib_rd32(dib + 8);
    bits = dib_rd16(dib + 14);
    if (dib_rd32(dib + 16) != 0u) return;            /* compressed: not ours */
    if (bits != 4u || w <= 0 || h == 0) return;
    if (h < 0) h = -h;                               /* negative: already top-down */
    if (w > 4096 || h > 4096) return;                /* refuse the absurd */

    clr_used = dib_rd32(dib + 32);
    if (clr_used == 0u || clr_used > 16u) clr_used = 16u;
    pixel_off = hdr + clr_used * 4u;

    row_bytes = ((u32)w + 1u) / 2u;
    padded = (row_bytes + 3u) & ~3u;
    if (len < pixel_off + padded * (u32)h) return;   /* truncated */

    memset(&desc, 0, sizeof(desc));
    desc.width_px = w;
    desc.height_px = h;
    desc.palette_count = (u8)clr_used;
    for (i = 0; i < clr_used; i++) {
        const u8 *quad = dib + hdr + i * 4u;         /* RGBQUAD: B, G, R, 0 */
        desc.palette[i][0] = quad[2];
        desc.palette[i][1] = quad[1];
        desc.palette[i][2] = quad[0];
    }

    /* \picwgoal is the display size in twips and is what the model
       stores; without one, fall back to the pixel count so the image is
       at least placed rather than collapsed to nothing. */
    desc.display_width_twips  = p->pict_goal_w > 0 ? p->pict_goal_w : w * 15;
    desc.display_height_twips = p->pict_goal_h > 0 ? p->pict_goal_h : h * 15;

    if (p->sink->begin == NULL || p->sink->write == NULL) return;
    if (p->sink->begin(p->sink->user, w, h, path, (u32)sizeof(path), &path_len) != WP_OK) return;
    if (path_len == 0 || path_len >= sizeof(path)) return;

    {   /* rows: bottom-up and padded on the way in, top-down and packed
           on the way out */
        wp_bool bottom_up = (i32)dib_rd32(dib + 8) > 0;
        wp_status st = WP_OK;
        u32 y;

        for (y = 0; y < (u32)h && st == WP_OK; y++) {
            u32 src_row = bottom_up ? ((u32)h - 1u - y) : y;
            st = p->sink->write(p->sink->user, dib + pixel_off + src_row * padded, row_bytes);
        }
        if (p->sink->end != NULL) p->sink->end(p->sink->user);
        if (st != WP_OK) return;
    }

    if (wp_strpool_append(&p->doc->strings, (const u8 *)path, path_len,
                           &desc.blob_path) != WP_OK) { p->error = WP_NOMEM; return; }
    if (wp_document_image_add(p->doc, &desc, &handle) != WP_OK) { p->error = WP_NOMEM; return; }

    /* An image is its own paragraph, the same shape wp_edit_insert_image
       gives it. */
    rtf_flush_run(p);
    if (p->error != WP_OK) return;
    rtf_ensure_paragraph(p);
    if (p->error != WP_OK) return;

    sec = wp_document_section_get(p->doc, p->section_idx);
    para = wp_section_paragraph_get(sec, p->para_idx);
    if (para->run_count > 0) {                 /* text already here: start a new one */
        if (wp_section_paragraph_add(sec, &new_idx) != WP_OK) { p->error = WP_NOMEM; return; }
        p->para_idx = new_idx;
        para = wp_section_paragraph_get(sec, new_idx);
    }
    para->kind = (u8)WP_PARA_IMAGE;
    para->image = handle;
    para->style = p->doc->default_pstyle;
    para->layout_dirty = WP_TRUE;
    p->para_pending = WP_TRUE;                 /* whatever follows gets its own */
}

static void rtf_pop(rtf_parser *p)
{
    if (p->error != WP_OK) return;
    if (p->depth == 0) { p->error = WP_BADFMT; return; }

    /* Leaving the \pict group is what says the picture is complete --
       there is no terminator inside it. */
    if (p->stack[p->depth].dest == RTF_DEST_PICT &&
        (p->depth == 0 || p->stack[p->depth - 1].dest != RTF_DEST_PICT)) {
        rtf_finish_pict(p);
    }
    /* Leaving a field instruction: was it the page number? */
    if (p->stack[p->depth].dest == RTF_DEST_FLDINST && p->depth > 0 &&
        p->stack[p->depth - 1].dest != RTF_DEST_FLDINST) {
        u32 k;
        p->fld_is_page = 0;
        for (k = 0; k + 4 <= p->fld_buf.length; k++) {
            if (memcmp(p->fld_buf.bytes + k, "PAGE", 4) == 0 &&
                (k + 4 == p->fld_buf.length || p->fld_buf.bytes[k + 4] == ' ' ||
                 p->fld_buf.bytes[k + 4] == '\\')) { p->fld_is_page = 1; break; }
        }
    }
    /* Leaving the header or footer group: it becomes the section's --
       the first of several in one section wins, and it replaces the one
       inherited from the section before, an empty one included (that is
       how a section says it has none). */
    if ((p->stack[p->depth].dest == RTF_DEST_HEADER || p->stack[p->depth].dest == RTF_DEST_FOOTER) &&
        p->depth > 0 && p->stack[p->depth - 1].dest != p->stack[p->depth].dest) {
        wp_section *sec = wp_document_section_get(p->doc, p->section_idx);
        wp_bool is_header = (wp_bool)(p->stack[p->depth].dest == RTF_DEST_HEADER);
        wp_span *slot = is_header ? &sec->header : &sec->footer;
        u8 *have = is_header ? &p->sec_has_header : &p->sec_has_footer;
        u32 len = p->margin_buf.length;

        while (len > 0 && p->margin_buf.bytes[len - 1] == ' ') len--;
        if (!*have) {
            *have = 1;
            slot->offset = 0;
            slot->length = 0;
            if (len > 0 && wp_strpool_append(&p->doc->strings, p->margin_buf.bytes, len, slot) != WP_OK) {
                p->error = WP_NOMEM;
            }
        }
        p->margin_buf.length = 0;
    }
    /* A \fonttbl entry in its own group may omit its ';'. */
    if (p->stack[p->depth].dest == RTF_DEST_FONTTBL && p->ft_name.length > 0) rtf_fonttbl_commit(p);
    /* Leaving the table: the default font is in force from here on. */
    if (p->stack[p->depth].dest == RTF_DEST_FONTTBL && p->depth > 0 &&
        p->stack[p->depth - 1].dest != RTF_DEST_FONTTBL) {
        rtf_select_font(p, &p->stack[p->depth - 1], p->deff);
    }
    if (p->stack[p->depth].dest == RTF_DEST_GENERATOR && p->depth > 0 &&
        p->stack[p->depth - 1].dest != RTF_DEST_GENERATOR) {
        p->ours = (p->gen_len >= 7 && memcmp(p->gen, "RetroWP", 7) == 0) ? WP_TRUE : WP_FALSE;
        /* The table came first: what it said of the default font is
           now known to be the placeholder. */
        if (p->ours) rtf_select_font(p, &p->stack[p->depth - 1], p->deff);
    }
    p->depth--;
    p->skip = 0;   /* nor does it outlive the group the \uN was in */
}

/* -- control word / symbol dispatch ------------------------------------ */

static void rtf_dispatch_control_word(rtf_parser *p, const char *name, wp_bool has_param, i32 param)
{
    rtf_state *st = &p->stack[p->depth];

    if (p->error != WP_OK) return;

    /* Charset declarations are recognized regardless of destination. */
    /* An unknown code page no longer refuses the file: its \'hh bytes
       decode as 1252 (right for every Western writer), and a modern
       writer spells everything outside it as \uN anyway. */
    if (strcmp(name, "ansi") == 0) { p->codepage = 1252; return; }
    if (strcmp(name, "ansicpg") == 0) {
        if (has_param && param > 0 && param < 65536 && wp_codepage_known((u16)param)) p->codepage = (u16)param;
        return;
    }
    if (strcmp(name, "mac") == 0) { p->codepage = 10000; return; }
    if (strcmp(name, "pc") == 0)  { p->codepage = 437; return; }
    if (strcmp(name, "pca") == 0) { p->codepage = 850; return; }
    if (strcmp(name, "bin") == 0) {
        /* Raw binary follows, N bytes, in any destination: step over it
           or it would be parsed as RTF. */
        if (has_param && param > 0) {
            u32 n = (u32)param;
            p->pos = (n > p->len - p->pos) ? p->len : p->pos + n;
        }
        return;
    }

    if (p->star_pending) {
        u8 in_margin = (u8)(p->star_pending == 1);
        p->star_pending = 0;
        if (in_margin && strcmp(name, "fldinst") == 0) {
            st->dest = RTF_DEST_FLDINST;
            p->fld_buf.length = 0;
            return;
        }
        if (strcmp(name, "generator") == 0) {
            st->dest = RTF_DEST_GENERATOR;
            p->gen_len = 0;
            return;
        }
    }
    if (strcmp(name, "deff") == 0) { if (has_param) p->deff = param; return; }

    if (st->dest == RTF_DEST_FONTTBL) {
        if (strcmp(name, "f") == 0) {
            if (p->ft_name.length > 0) rtf_fonttbl_commit(p);
            p->ft_num = has_param ? param : -1;
            p->ft_family = 0;
            p->ft_cp = 0;
            return;
        }
        if (strcmp(name, "fnil") == 0)    { p->ft_family = (u8)WP_FAMILY_NIL; return; }
        if (strcmp(name, "froman") == 0)  { p->ft_family = (u8)WP_FAMILY_ROMAN; return; }
        if (strcmp(name, "fswiss") == 0)  { p->ft_family = (u8)WP_FAMILY_SWISS; return; }
        if (strcmp(name, "fmodern") == 0) { p->ft_family = (u8)WP_FAMILY_MODERN; return; }
        if (strcmp(name, "fscript") == 0) { p->ft_family = (u8)WP_FAMILY_SCRIPT; return; }
        if (strcmp(name, "fdecor") == 0 || strcmp(name, "ftech") == 0 ||
            strcmp(name, "fbidi") == 0)   { p->ft_family = (u8)WP_FAMILY_DECOR; return; }
        if (strcmp(name, "fcharset") == 0) { p->ft_cp = rtf_charset_cp(has_param ? param : 0); return; }
        if (strcmp(name, "u") == 0 && has_param) {
            u8 enc[4];
            u32 k, en = utf8_encode((u32)((param < 0) ? param + 65536 : param), enc);
            for (k = 0; k < en; k++) (void)rtf_buf_append_byte(&p->ft_name, enc[k]);
            p->skip = st->uc;
            return;
        }
        return;   /* \fprq, \fttruetype, ...: nothing a name needs */
    }
    if (st->dest == RTF_DEST_GENERATOR) return;

    if (st->dest == RTF_DEST_SKIP) return;

    /* Inside a captured header or footer only a few words mean anything:
       a tab is a part break ('|'), \chpgn or a PAGE field the page
       number ('#'), \par a space; paragraph and character formatting
       there must not reach the body's state, so the rest is ignored. */
    if (st->dest == RTF_DEST_HEADER || st->dest == RTF_DEST_FOOTER) {
        if (strcmp(name, "tab") == 0) { (void)rtf_buf_append_byte(&p->margin_buf, '|'); return; }
        if (strcmp(name, "chpgn") == 0) { (void)rtf_buf_append_byte(&p->margin_buf, '#'); return; }
        if (strcmp(name, "par") == 0 || strcmp(name, "line") == 0) {
            if (p->margin_buf.length > 0) (void)rtf_buf_append_byte(&p->margin_buf, ' ');
            return;
        }
        if (strcmp(name, "fldrslt") == 0) {
            /* The field's last shown result: the page number itself when
               the instruction was PAGE, which '#' stands for. */
            if (p->fld_is_page) {
                (void)rtf_buf_append_byte(&p->margin_buf, '#');
                st->dest = RTF_DEST_SKIP;
                p->fld_is_page = 0;
            }
            return;
        }
        if (strcmp(name, "u") == 0 && has_param) {
            u8 enc[4];
            u32 k, en = utf8_encode((u32)((param < 0) ? param + 65536 : param), enc);
            for (k = 0; k < en; k++) (void)rtf_buf_append_byte(&p->margin_buf, enc[k]);
            p->skip = st->uc;
            return;
        }
        if (strcmp(name, "uc") == 0) { st->uc = (u8)((has_param && param >= 0 && param < 256) ? param : 1); return; }
        if (strcmp(name, "v") == 0) { st->hidden = (u8)(!has_param || param != 0); return; }
        return;
    }
    if (st->dest == RTF_DEST_FLDINST) return;

    /* The header and footer (odd, or all, pages) are captured; the
       left-page and first-page variants stay skipped below. */
    if (strcmp(name, "header") == 0 || strcmp(name, "headerr") == 0 ||
        strcmp(name, "footer") == 0 || strcmp(name, "footerr") == 0) {
        rtf_flush_run(p);
        rtf_open_section(p);
        st->dest = (name[0] == 'h') ? RTF_DEST_HEADER : RTF_DEST_FOOTER;
        st->hidden = 0;
        p->margin_buf.length = 0;
        return;
    }

    /* Destinations: recognized so their content is skipped rather than
       mis-imported as document text (docs/formats/rtf.md). */
    /* Word's left/right/first-page headers and footers, list-number
       text (\pntext, \listtext -- the number is generated, not text),
       \pn and the file and revision tables join them: all of these
       leaked into the body before. */
    if (strcmp(name, "fonttbl") == 0) {
        st->dest = RTF_DEST_FONTTBL;
        p->ft_num = -1;
        p->ft_name.length = 0;
        return;
    }
    if (strcmp(name, "info") == 0 ||
        strcmp(name, "stylesheet") == 0 ||
        strcmp(name, "object") == 0 || strcmp(name, "footnote") == 0 ||
        strcmp(name, "headerl") == 0 ||
        strcmp(name, "headerf") == 0 || strcmp(name, "footerl") == 0 ||
        strcmp(name, "footerf") == 0 ||
        strcmp(name, "pntext") == 0 || strcmp(name, "listtext") == 0 ||
        strcmp(name, "pn") == 0 || strcmp(name, "filetbl") == 0 ||
        strcmp(name, "revtbl") == 0 || strcmp(name, "fldinst") == 0) {
        st->dest = RTF_DEST_SKIP;
        return;
    }
    if (strcmp(name, "pict") == 0) {
        /* Read rather than skipped, but only when the caller gave us
           somewhere to put the pixels -- without a sink there is no file
           to name and the picture may as well be skipped, which is what
           this parser did for its whole life before now. */
        st->dest = p->sink ? RTF_DEST_PICT : RTF_DEST_SKIP;
        p->pict_buf.length = 0;
        p->pict_is_dib = WP_FALSE;
        p->pict_nibble_pending = WP_FALSE;
        p->pict_w = p->pict_h = 0;
        p->pict_goal_w = p->pict_goal_h = 0;
        return;
    }
    if (st->dest == RTF_DEST_PICT) {
        if (strcmp(name, "dibitmap") == 0)  { p->pict_is_dib = WP_TRUE; return; }
        if (strcmp(name, "picw") == 0)      { p->pict_w = param; return; }
        if (strcmp(name, "pich") == 0)      { p->pict_h = param; return; }
        if (strcmp(name, "picwgoal") == 0)  { p->pict_goal_w = param; return; }
        if (strcmp(name, "pichgoal") == 0)  { p->pict_goal_h = param; return; }
        return;   /* every other picture control word: not ours to act on */
    }
    if (strcmp(name, "colortbl") == 0) {
        st->dest = RTF_DEST_COLORTBL;
        p->color_count = 0;
        p->pending_r = p->pending_g = p->pending_b = 0;
        return;
    }

    /* Paragraph formatting. */
    if (strcmp(name, "pard") == 0) {
        rtf_flush_run(p);
        st->align = WP_ALIGN_LEFT;
        st->indent_left = 0; st->indent_right = 0; st->indent_first = 0;
        st->space_before = 0; st->space_after = 0;
        st->sl = 0; st->slmult = 0;
        st->pagebb = 0;
        p->tab_count = 0;
        p->tab_kind_pending = (u8)WP_TAB_LEFT;
        return;
    }
    if (strcmp(name, "par") == 0)  { rtf_end_paragraph(p); return; }
    if (strcmp(name, "line") == 0) { rtf_end_paragraph(p); return; }
    if (strcmp(name, "ql") == 0) { st->align = WP_ALIGN_LEFT; return; }
    if (strcmp(name, "qr") == 0) { st->align = WP_ALIGN_RIGHT; return; }
    if (strcmp(name, "qc") == 0) { st->align = WP_ALIGN_CENTER; return; }
    if (strcmp(name, "qj") == 0) { st->align = WP_ALIGN_JUSTIFY; return; }
    if (strcmp(name, "li") == 0) { st->indent_left = has_param ? param : 0; return; }
    if (strcmp(name, "ri") == 0) { st->indent_right = has_param ? param : 0; return; }
    if (strcmp(name, "fi") == 0) { st->indent_first = has_param ? param : 0; return; }
    if (strcmp(name, "sb") == 0) { st->space_before = has_param ? param : 0; return; }
    if (strcmp(name, "sa") == 0) { st->space_after = has_param ? param : 0; return; }
    if (strcmp(name, "sl") == 0) { st->sl = has_param ? param : 0; return; }
    if (strcmp(name, "pagebb") == 0) { st->pagebb = (u8)(!has_param || param != 0); return; }
    if (strcmp(name, "slmult") == 0) { st->slmult = (u8)(has_param && param != 0); return; }
    if (strcmp(name, "tqr") == 0) { p->tab_kind_pending = (u8)WP_TAB_RIGHT; return; }
    if (strcmp(name, "tqc") == 0) { p->tab_kind_pending = (u8)WP_TAB_CENTER; return; }
    if (strcmp(name, "tqdec") == 0) { p->tab_kind_pending = (u8)WP_TAB_DECIMAL; return; }
    if (strcmp(name, "tb") == 0) { p->tab_kind_pending = (u8)WP_TAB_LEFT; return; }   /* bar tab: not drawn */
    if (strcmp(name, "tx") == 0) {
        /* A stop, kept sorted; past twelve the rest are dropped. */
        if (has_param && param > 0 && p->tab_count < WP_MAX_TABS) {
            u8 at = p->tab_count;
            while (at > 0 && p->tab_pos[at - 1] > param) {
                p->tab_pos[at] = p->tab_pos[at - 1];
                p->tab_kind[at] = p->tab_kind[at - 1];
                at--;
            }
            p->tab_pos[at] = param;
            p->tab_kind[at] = p->tab_kind_pending;
            p->tab_count++;
        }
        p->tab_kind_pending = (u8)WP_TAB_LEFT;
        return;
    }
    /* \sectd: this section's properties back to the document's. */
    if (strcmp(name, "sectd") == 0) {
        wp_section *sec;
        rtf_open_section(p);
        sec = wp_document_section_get(p->doc, p->section_idx);
        sec->page_width = p->doc_page[0];
        sec->page_height = p->doc_page[1];
        sec->margin_left = p->doc_page[2];
        sec->margin_right = p->doc_page[3];
        sec->margin_top = p->doc_page[4];
        sec->margin_bottom = p->doc_page[5];
        return;
    }
    /* Page setup, document-wide (\paperw...: the document's, and the
       section being read) or for the section alone (\pgwsxn...). */
    if (has_param && param > 0 &&
        (strcmp(name, "paperw") == 0 || strcmp(name, "pgwsxn") == 0 ||
         strcmp(name, "paperh") == 0 || strcmp(name, "pghsxn") == 0)) {
        wp_section *sec;
        wp_bool w = (wp_bool)(name[1] == 'a' ? name[5] == 'w' : name[2] == 'w');
        if (name[1] == 'a') p->doc_page[w ? 0 : 1] = param;
        else rtf_open_section(p);
        sec = wp_document_section_get(p->doc, p->section_idx);
        if (w) sec->page_width = param;
        else sec->page_height = param;
        return;
    }
    if (has_param && param >= 0 &&
        (strncmp(name, "marg", 4) == 0) &&
        (strcmp(name + 4, "l") == 0 || strcmp(name + 4, "r") == 0 ||
         strcmp(name + 4, "t") == 0 || strcmp(name + 4, "b") == 0 ||
         strcmp(name + 4, "lsxn") == 0 || strcmp(name + 4, "rsxn") == 0 ||
         strcmp(name + 4, "tsxn") == 0 || strcmp(name + 4, "bsxn") == 0)) {
        wp_section *sec;
        if (name[5] == '\0') {          /* \margl...: the document's too */
            p->doc_page[name[4] == 'l' ? 2 : name[4] == 'r' ? 3 : name[4] == 't' ? 4 : 5] = param;
        } else {
            rtf_open_section(p);
        }
        sec = wp_document_section_get(p->doc, p->section_idx);
        switch (name[4]) {
        case 'l': sec->margin_left = param; break;
        case 'r': sec->margin_right = param; break;
        case 't': sec->margin_top = param; break;
        default:  sec->margin_bottom = param; break;
        }
        return;
    }
    /* Tables arrive as rows of tab-separated cells, which the ruler's tab
       stops can then line up: no table model yet, but no glued-together
       cell text either. */
    if (strcmp(name, "cell") == 0 || strcmp(name, "nestcell") == 0) { rtf_emit_char(p, '\t'); return; }
    if (strcmp(name, "row") == 0 || strcmp(name, "nestrow") == 0) {
        if (p->run_buf.length > 0 && p->run_buf.bytes[p->run_buf.length - 1] == '\t') p->run_buf.length--;
        rtf_end_paragraph(p);
        return;
    }
    /* A page or section break ends the paragraph. After \page the next
       one starts a page (page_break_before); after \sect it starts the
       next section, which starts a page of its own (\sbkpage, the
       default and the only kind this model has). */
    if (strcmp(name, "page") == 0 || strcmp(name, "sect") == 0) {
        /* Straight after a \par (Word's usual "\par\page") there is no
           paragraph to end -- ending one would add an empty paragraph at
           every break. */
        if (!p->para_pending || p->run_buf.length > 0) rtf_end_paragraph(p);
        if (name[0] == 'p') {
            p->break_next = 1;
        } else {
            p->sect_pending = 1;      /* two in a row are one: no section without a paragraph */
        }
        return;
    }

    /* Character formatting. */
    if (strcmp(name, "plain") == 0) {
        /* Back to the default character format -- Word writes \pard\plain
           before nearly every paragraph, and without this a bold or a
           size carried on into text meant to be plain. */
        rtf_flush_run(p);
        st->char_flags = 0;
        st->size_twips = 240;
        st->color_r = st->color_g = st->color_b = 0;
        st->hidden = 0;
        rtf_select_font(p, st, p->deff);
        return;
    }
    if (strcmp(name, "f") == 0) {
        rtf_flush_run(p);
        rtf_select_font(p, st, has_param ? param : p->deff);
        return;
    }
    if (strcmp(name, "v") == 0) {
        rtf_flush_run(p);
        st->hidden = (u8)(!has_param || param != 0);
        return;
    }
    if (strcmp(name, "uc") == 0) { st->uc = (u8)((has_param && param >= 0 && param < 256) ? param : 1); return; }
    if (strcmp(name, "u") == 0) {
        /* A Unicode character (signed 16-bit in the source), then \ucN
           fallback characters for readers that do not know \u. */
        if (has_param) {
            u32 cp = (u32)((param < 0) ? param + 65536 : param);
            rtf_emit_cp(p, cp);   /* dropped there in a skipped destination */
            p->skip = st->uc;
        }
        return;
    }
    if (strcmp(name, "b") == 0) {
        rtf_flush_run(p);
        if (!has_param || param) st->char_flags = (u16)(st->char_flags | WP_STYLE_BOLD);
        else st->char_flags = (u16)(st->char_flags & ~WP_STYLE_BOLD);
        return;
    }
    if (strcmp(name, "i") == 0) {
        rtf_flush_run(p);
        if (!has_param || param) st->char_flags = (u16)(st->char_flags | WP_STYLE_ITALIC);
        else st->char_flags = (u16)(st->char_flags & ~WP_STYLE_ITALIC);
        return;
    }
    if (strcmp(name, "ul") == 0) {
        rtf_flush_run(p);
        if (!has_param || param) st->char_flags = (u16)(st->char_flags | WP_STYLE_UNDERLINE);
        else st->char_flags = (u16)(st->char_flags & ~WP_STYLE_UNDERLINE);
        return;
    }
    if (strcmp(name, "ulnone") == 0) { rtf_flush_run(p); st->char_flags = (u16)(st->char_flags & ~WP_STYLE_UNDERLINE); return; }
    if (strcmp(name, "strike") == 0) {
        rtf_flush_run(p);
        if (!has_param || param) st->char_flags = (u16)(st->char_flags | WP_STYLE_STRIKE);
        else st->char_flags = (u16)(st->char_flags & ~WP_STYLE_STRIKE);
        return;
    }
    if (strcmp(name, "super") == 0) {
        rtf_flush_run(p);
        st->char_flags = (u16)((st->char_flags & ~WP_STYLE_SUB) | WP_STYLE_SUPER);
        return;
    }
    if (strcmp(name, "sub") == 0) {
        rtf_flush_run(p);
        st->char_flags = (u16)((st->char_flags & ~WP_STYLE_SUPER) | WP_STYLE_SUB);
        return;
    }
    if (strcmp(name, "nosupersub") == 0) {
        rtf_flush_run(p);
        st->char_flags = (u16)(st->char_flags & ~(WP_STYLE_SUPER | WP_STYLE_SUB));
        return;
    }
    if (strcmp(name, "fs") == 0) {
        rtf_flush_run(p);
        if (has_param) st->size_twips = param * 10; /* half-points -> twips */
        return;
    }
    if (strcmp(name, "cf") == 0) {
        rtf_flush_run(p);
        if (has_param && param >= 0 && (u32)param < p->color_count) {
            st->color_r = p->color_table[param][0];
            st->color_g = p->color_table[param][1];
            st->color_b = p->color_table[param][2];
        }
        return;
    }

    /* Color table entries -- only meaningful while inside \colortbl. */
    if (st->dest == RTF_DEST_COLORTBL) {
        if (strcmp(name, "red") == 0)   { p->pending_r = has_param ? (u8)param : 0; return; }
        if (strcmp(name, "green") == 0) { p->pending_g = has_param ? (u8)param : 0; return; }
        if (strcmp(name, "blue") == 0)  { p->pending_b = has_param ? (u8)param : 0; return; }
    }

    if (strcmp(name, "tab") == 0)       { rtf_emit_char(p, '\t'); return; }
    if (strcmp(name, "emdash") == 0)    { rtf_emit_cp(p, 0x2014); return; }
    if (strcmp(name, "endash") == 0)    { rtf_emit_cp(p, 0x2013); return; }
    if (strcmp(name, "lquote") == 0)    { rtf_emit_cp(p, 0x2018); return; }
    if (strcmp(name, "rquote") == 0)    { rtf_emit_cp(p, 0x2019); return; }
    if (strcmp(name, "ldblquote") == 0) { rtf_emit_cp(p, 0x201C); return; }
    if (strcmp(name, "rdblquote") == 0) { rtf_emit_cp(p, 0x201D); return; }
    if (strcmp(name, "bullet") == 0)    { rtf_emit_cp(p, 0x2022); return; }

    /* Anything else: unrecognized control word -- ignored (its text, if
       any follows, is still imported), per RTF's own forward-
       compatibility rule. See docs/formats/rtf.md. */
}

static void rtf_dispatch_control_symbol(rtf_parser *p, u8 sym)
{
    if (p->error != WP_OK) return;

    switch (sym) {
    case '{': rtf_emit_char(p, '{'); return;
    case '}': rtf_emit_char(p, '}'); return;
    case '\\': rtf_emit_char(p, '\\'); return;
    case '~': rtf_emit_char(p, ' '); return;  /* non-breaking space -> space (documented approximation) */
    case '-': return;                          /* optional hyphen -- dropped */
    case '_': rtf_emit_char(p, '-'); return;   /* non-breaking hyphen -> hyphen */
    case '*':
        /* Inside a header or footer, \*\fldinst is read (to tell a page
           number field); everywhere else a starred group is skipped. */
        /* \*\generator is read anywhere (1: in a margin, 2: not). */
        p->star_pending = (u8)((p->stack[p->depth].dest == RTF_DEST_HEADER ||
                                p->stack[p->depth].dest == RTF_DEST_FOOTER) ? 1 : 2);
        p->stack[p->depth].dest = RTF_DEST_SKIP;
        return;
    case '\'': {
        u8 hi, lo;
        if (p->pos + 1 >= p->len) { p->error = WP_BADFMT; return; }
        hi = p->src[p->pos];
        lo = p->src[p->pos + 1];
        if (!is_hex(hi) || !is_hex(lo)) { p->error = WP_BADFMT; return; }
        p->pos += 2;
        rtf_source_char(p, (u8)(hex_val(hi) * 16 + hex_val(lo)));
        return;
    }
    default:
        return; /* unrecognized symbol -- ignored, same policy as unknown control words */
    }
}

static void rtf_handle_backslash(rtf_parser *p)
{
    u8 c;

    if (p->pos >= p->len) { p->error = WP_BADFMT; return; }
    c = p->src[p->pos];

    if (is_alpha(c)) {
        char name[32];
        u32 name_len = 0;
        i32 param = 0;
        wp_bool has_param = WP_FALSE;
        wp_bool neg = WP_FALSE;

        while (p->pos < p->len && is_alpha(p->src[p->pos]) && name_len < sizeof(name) - 1) {
            name[name_len++] = (char)p->src[p->pos];
            p->pos++;
        }
        name[name_len] = '\0';

        if (p->pos < p->len && p->src[p->pos] == '-') { neg = WP_TRUE; p->pos++; }
        while (p->pos < p->len && is_digit(p->src[p->pos])) {
            has_param = WP_TRUE;
            param = param * 10 + (i32)(p->src[p->pos] - '0');
            p->pos++;
        }
        if (neg) param = -param;

        /* A single trailing space delimits the control word and is
           consumed, not emitted as a literal space. */
        if (p->pos < p->len && p->src[p->pos] == ' ') p->pos++;

        rtf_dispatch_control_word(p, name, has_param, param);
    } else {
        p->pos++;
        rtf_dispatch_control_symbol(p, c);
    }
}

static void rtf_finish(rtf_parser *p)
{
    rtf_flush_run(p);
    if (p->error != WP_OK) return;
    if (p->para_pending) return; /* last thing was a closing \par -- no phantom trailing paragraph */
    rtf_apply_pstyle_to_current_paragraph(p);
}

wp_status format_rtf_import_images(const u8 *bytes, u32 len, wp_document *doc,
                                    const wp_image_sink *sink)
{
    rtf_parser *p;
    wp_status st;
    wp_section *sec;
    u32 para_idx;

    if (len < 6 || bytes[0] != '{' || memcmp(bytes + 1, "\\rtf1", 5) != 0) return WP_BADFMT;

    st = wp_document_init(doc);
    if (st != WP_OK) return st;

    /* On the heap: 64 nested group states are kilobytes, and an Amiga
       program launched from RUN has a 4 KB stack in all. */
    p = (rtf_parser *)mem_alloc((u32)sizeof(rtf_parser));
    if (p == NULL) { wp_document_free(doc); return WP_NOMEM; }
    memset(p, 0, sizeof(*p));
    p->src = bytes;
    p->len = len;
    p->pos = 6;
    p->doc = doc;
    p->stack[0].char_flags = 0;
    p->stack[0].size_twips = 240; /* matches wp_document_init's default cstyle size */
    p->stack[0].color_r = 0; p->stack[0].color_g = 0; p->stack[0].color_b = 0;
    p->stack[0].align = WP_ALIGN_LEFT;
    p->stack[0].dest = RTF_DEST_NORMAL;
    p->stack[0].uc = 1;
    p->codepage = 1252;
    p->ft_num = -1;
    p->doc_page[0] = 12240; p->doc_page[1] = 15840;   /* RTF's defaults: Letter, an inch */
    p->doc_page[2] = p->doc_page[3] = p->doc_page[4] = p->doc_page[5] = 1440;
    p->depth = 0;
    p->error = WP_OK;
    p->sink = sink;

    if (wp_document_section_add(doc, &p->section_idx) != WP_OK) { mem_free(p); wp_document_free(doc); return WP_NOMEM; }
    sec = wp_document_section_get(doc, p->section_idx);
    if (wp_section_paragraph_add(sec, &para_idx) != WP_OK) { mem_free(p); wp_document_free(doc); return WP_NOMEM; }
    p->para_idx = para_idx;
    p->para_pending = WP_FALSE;

    while (p->pos < p->len && p->error == WP_OK) {
        u8 c = p->src[p->pos];

        if (c == '{') {
            rtf_push(p);
            p->pos++;
        } else if (c == '}') {
            rtf_flush_run(p);
            if (p->depth == 0) { p->pos++; break; } /* the document's closing brace */
            rtf_pop(p);
            p->pos++;
        } else if (c == '\\') {
            p->pos++;
            rtf_handle_backslash(p);
        } else if (c == '\r' || c == '\n') {
            p->pos++; /* raw source formatting, not document content */
        } else {
            rtf_source_char(p, c);
            p->pos++;
        }
    }

    if (p->error == WP_OK) rtf_finish(p);

    rtf_buf_free(&p->run_buf);
    rtf_buf_free(&p->margin_buf);
    rtf_buf_free(&p->fld_buf);
    rtf_buf_free(&p->ft_name);
    rtf_buf_free(&p->ft_names);
    rtf_buf_free(&p->pict_buf);

    st = p->error;
    mem_free(p);
    if (st != WP_OK) {
        wp_document_free(doc);
        return st;
    }
    return WP_OK;
}

wp_status format_rtf_import(const u8 *bytes, u32 len, wp_document *doc)
{
    return format_rtf_import_images(bytes, len, doc, NULL);
}
