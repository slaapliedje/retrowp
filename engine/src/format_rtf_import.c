/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_rtf.h"
#include "platform.h"
#include "utf8.h"
#include "growth_internal.h"
#include "format_rtf_internal.h"

/* Classic RTF 1.x import. Full scope, supported control words, and
   documented simplifications are in docs/formats/rtf.md -- read it
   before changing this file. */

#define RTF_MAX_DEPTH 64

typedef enum {
    RTF_DEST_NORMAL = 0,
    RTF_DEST_SKIP,       /* ignorable destination: fonttbl, info, etc. */
    RTF_DEST_COLORTBL,
    RTF_DEST_PICT        /* \pict -- hex picture data, not document text */
} rtf_dest;

typedef struct {
    u16 char_flags;    /* wp_style_flags bitfield */
    i32 size_twips;
    u8  color_r, color_g, color_b;
    u8  align;          /* wp_align */
    i32 indent_left, indent_right, indent_first;
    i32 space_before, space_after;
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

static void rtf_ensure_paragraph(rtf_parser *p)
{
    wp_section *sec;
    u32 new_idx;

    if (p->error != WP_OK || !p->para_pending) return;

    sec = wp_document_section_get(p->doc, p->section_idx);
    if (wp_section_paragraph_add(sec, &new_idx) != WP_OK) { p->error = WP_NOMEM; return; }
    p->para_idx = new_idx;
    p->para_pending = WP_FALSE;
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

    if (wp_document_pstyle_add(p->doc, &ps, &pstyle) != WP_OK) { p->error = WP_NOMEM; return; }

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

static void rtf_emit_bytes(rtf_parser *p, const u8 *bytes, u32 n)
{
    rtf_state *st;
    u32 i;

    if (p->error != WP_OK) return;
    st = &p->stack[p->depth];

    if (st->dest == RTF_DEST_SKIP) return;

    if (st->dest == RTF_DEST_COLORTBL) {
        if (n == 1 && bytes[0] == ';') rtf_colortbl_commit(p);
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

static void rtf_emit_cp1252_byte(rtf_parser *p, u8 byte_val)
{
    if (byte_val < 0x80) { rtf_emit_char(p, byte_val); return; }
    rtf_emit_cp(p, rtf_cp1252_to_unicode(byte_val));
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
    p->depth--;
}

/* -- control word / symbol dispatch ------------------------------------ */

static void rtf_dispatch_control_word(rtf_parser *p, const char *name, wp_bool has_param, i32 param)
{
    rtf_state *st = &p->stack[p->depth];

    if (p->error != WP_OK) return;

    /* Charset declarations are recognized regardless of destination. */
    if (strcmp(name, "ansi") == 0) return;
    if (strcmp(name, "ansicpg") == 0) {
        if (!has_param || param != 1252) p->error = WP_UNSUPPORTED;
        return;
    }
    if (strcmp(name, "mac") == 0 || strcmp(name, "pc") == 0 || strcmp(name, "pca") == 0) {
        p->error = WP_UNSUPPORTED;
        return;
    }

    if (st->dest == RTF_DEST_SKIP) return;

    /* Destinations: recognized so their content is skipped rather than
       mis-imported as document text (docs/formats/rtf.md). */
    if (strcmp(name, "fonttbl") == 0 || strcmp(name, "info") == 0 ||
        strcmp(name, "stylesheet") == 0 ||
        strcmp(name, "object") == 0 || strcmp(name, "footnote") == 0 ||
        strcmp(name, "header") == 0 || strcmp(name, "footer") == 0) {
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

    /* Character formatting. */
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
    case '*': p->stack[p->depth].dest = RTF_DEST_SKIP; return;
    case '\'': {
        u8 hi, lo;
        if (p->pos + 1 >= p->len) { p->error = WP_BADFMT; return; }
        hi = p->src[p->pos];
        lo = p->src[p->pos + 1];
        if (!is_hex(hi) || !is_hex(lo)) { p->error = WP_BADFMT; return; }
        p->pos += 2;
        rtf_emit_cp1252_byte(p, (u8)(hex_val(hi) * 16 + hex_val(lo)));
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
    rtf_parser p;
    wp_status st;
    wp_section *sec;
    u32 para_idx;

    if (len < 6 || bytes[0] != '{' || memcmp(bytes + 1, "\\rtf1", 5) != 0) return WP_BADFMT;

    st = wp_document_init(doc);
    if (st != WP_OK) return st;

    memset(&p, 0, sizeof(p));
    p.src = bytes;
    p.len = len;
    p.pos = 6;
    p.doc = doc;
    p.stack[0].char_flags = 0;
    p.stack[0].size_twips = 240; /* matches wp_document_init's default cstyle size */
    p.stack[0].color_r = 0; p.stack[0].color_g = 0; p.stack[0].color_b = 0;
    p.stack[0].align = WP_ALIGN_LEFT;
    p.stack[0].dest = RTF_DEST_NORMAL;
    p.depth = 0;
    p.error = WP_OK;
    p.sink = sink;

    if (wp_document_section_add(doc, &p.section_idx) != WP_OK) { wp_document_free(doc); return WP_NOMEM; }
    sec = wp_document_section_get(doc, p.section_idx);
    if (wp_section_paragraph_add(sec, &para_idx) != WP_OK) { wp_document_free(doc); return WP_NOMEM; }
    p.para_idx = para_idx;
    p.para_pending = WP_FALSE;

    while (p.pos < p.len && p.error == WP_OK) {
        u8 c = p.src[p.pos];

        if (c == '{') {
            rtf_push(&p);
            p.pos++;
        } else if (c == '}') {
            rtf_flush_run(&p);
            if (p.depth == 0) { p.pos++; break; } /* the document's closing brace */
            rtf_pop(&p);
            p.pos++;
        } else if (c == '\\') {
            p.pos++;
            rtf_handle_backslash(&p);
        } else if (c == '\r' || c == '\n') {
            p.pos++; /* raw source formatting, not document content */
        } else {
            rtf_emit_char(&p, c);
            p.pos++;
        }
    }

    if (p.error == WP_OK) rtf_finish(&p);

    rtf_buf_free(&p.run_buf);
    rtf_buf_free(&p.pict_buf);

    if (p.error != WP_OK) {
        wp_document_free(doc);
        return p.error;
    }
    return WP_OK;
}

wp_status format_rtf_import(const u8 *bytes, u32 len, wp_document *doc)
{
    return format_rtf_import_images(bytes, len, doc, NULL);
}
