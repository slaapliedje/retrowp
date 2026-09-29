/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_rtf.h"
#include "utf8.h"
#include "format_rtf_internal.h"
#include "platform.h"   /* mem_alloc/mem_free -- CLAUDE.md's allocator rule */

/* Classic RTF 1.x export -- the inverse of format_rtf_import.c. Full
   scope and the round-trip contract are in docs/formats/rtf.md.

   Single emission pass, run twice: once with a NULL sink (just counts
   bytes, matching format_rtf_export's "measure" contract) and once for
   real if the caller's buffer is big enough. One shared implementation
   for both, rather than a separately-maintained counting function
   (format_txt.c's simpler approach), because RTF emission has enough
   real logic (escaping, color table lookups, conditional control words)
   that two hand-synced copies would be a real drift risk. Nothing is
   written to `out` unless the full document fits -- matching
   format_rtf.h's "WP_RANGE (nothing written) if out_cap is too small"
   contract exactly, not just approximately. */

typedef struct {
    u8 *out;    /* NULL in measure-only mode */
    u32 cap;
    u32 pos;    /* total bytes the document needs, regardless of mode */
} rtf_writer;

static void rtf_write_bytes(rtf_writer *w, const u8 *bytes, u32 n)
{
    if (w->out != NULL) {
        u32 i;
        for (i = 0; i < n && w->pos + i < w->cap; i++) w->out[w->pos + i] = bytes[i];
    }
    w->pos += n;
}

static void rtf_write_str(rtf_writer *w, const char *s)
{
    rtf_write_bytes(w, (const u8 *)s, (u32)strlen(s));
}

static void rtf_write_i32(rtf_writer *w, i32 v)
{
    char buf[16];
    u32 i = 15;
    wp_bool neg = (v < 0);
    u32 uv = neg ? (u32)(-v) : (u32)v;

    buf[i--] = '\0';
    if (uv == 0) {
        buf[i--] = '0';
    } else {
        while (uv > 0) {
            buf[i--] = (char)('0' + (uv % 10));
            uv /= 10;
        }
    }
    if (neg) buf[i--] = '-';
    rtf_write_str(w, &buf[i + 1]);
}

/* Unicode codepoint -> cp1252 byte. Returns WP_FALSE if cp has no
   cp1252 representation (caller substitutes '?'). The only consumer of
   the reverse direction of rtf_cp1252_to_unicode
   (format_rtf_internal.h), so it lives here rather than in that shared
   header -- format_rtf_import.c never needs it, and a static function
   only one of two includers calls trips -Werror=unused-function. */
static wp_bool rtf_unicode_to_cp1252(u32 cp, u8 *out_byte)
{
    u32 i;

    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) { *out_byte = (u8)cp; return WP_TRUE; }
    for (i = 0; i < 32; i++) {
        if (rtf_cp1252_to_unicode((u8)(0x80 + i)) == cp) { *out_byte = (u8)(0x80 + i); return WP_TRUE; }
    }
    return WP_FALSE;
}

/* Escapes and writes UTF-8 text as RTF text: '{'/'}'/'\\' are escaped,
   ASCII passes through, everything else round-trips through cp1252 via
   '\'hh' (falling back to a literal '?' for codepoints cp1252 can't
   represent -- rare in documents this project can itself produce, since
   nothing populates styles from arbitrary Unicode input yet). */
static void rtf_write_text(rtf_writer *w, const u8 *bytes, u32 len)
{
    u32 pos = 0;

    while (pos < len) {
        u32 cp;
        u32 n = utf8_decode(bytes + pos, len - pos, &cp);

        if (n == 0) { pos++; continue; } /* skip an invalid byte rather than emit garbage */

        if (cp == '{' || cp == '}' || cp == '\\') {
            u8 esc[2];
            esc[0] = '\\';
            esc[1] = (u8)cp;
            rtf_write_bytes(w, esc, 2);
        } else if (cp < 0x80) {
            u8 b = (u8)cp;
            rtf_write_bytes(w, &b, 1);
        } else {
            u8 native;
            if (rtf_unicode_to_cp1252(cp, &native)) {
                char hex[4];
                hex[0] = '\\'; hex[1] = '\'';
                hex[2] = "0123456789abcdef"[native >> 4];
                hex[3] = "0123456789abcdef"[native & 0xF];
                rtf_write_bytes(w, (const u8 *)hex, 4);
            } else {
                rtf_write_bytes(w, (const u8 *)"?", 1);
            }
        }
        pos += n;
    }
}

/* Colors actually used by the document, gathered once up front so both
   emission passes (measure and real) agree on \cfN numbering without
   redoing the scan differently each time. Index 0 is always the
   implicit "auto" (black) entry per RTF convention; unmatched/overflow
   colors (more than RTF_MAX_COLORS-1 distinct non-black colors, an
   unlikely case for this project's realistic style usage) fall back to
   index 0 rather than growing the table further. */
typedef struct {
    u8  rgb[RTF_MAX_COLORS][3];
    u32 count;
} rtf_color_table;

/* Read-only lookup, used during emission once the table is already
   complete (built by rtf_build_color_table before either emission pass
   runs) -- never adds an entry, so it never needs to mutate ct. */
static u32 rtf_color_lookup(const rtf_color_table *ct, u8 r, u8 g, u8 b)
{
    u32 i;

    if (r == 0 && g == 0 && b == 0) return 0;
    for (i = 1; i < ct->count; i++) {
        if (ct->rgb[i][0] == r && ct->rgb[i][1] == g && ct->rgb[i][2] == b) return i;
    }
    return 0; /* unreachable in practice: every run's color was already
                 added to ct by rtf_build_color_table's identical scan */
}

/* Mutating: adds r/g/b if not already present (up to RTF_MAX_COLORS),
   used only while building the table up front. */
static u32 rtf_color_index(rtf_color_table *ct, u8 r, u8 g, u8 b)
{
    u32 existing = rtf_color_lookup(ct, r, g, b);

    if (existing != 0 || (r == 0 && g == 0 && b == 0)) return existing;
    if (ct->count < RTF_MAX_COLORS) {
        ct->rgb[ct->count][0] = r;
        ct->rgb[ct->count][1] = g;
        ct->rgb[ct->count][2] = b;
        return ct->count++;
    }
    return 0;
}

static void rtf_build_color_table(const wp_document *doc, rtf_color_table *ct)
{
    u32 si, pi;

    ct->rgb[0][0] = 0; ct->rgb[0][1] = 0; ct->rgb[0][2] = 0;
    ct->count = 1;

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            u16 ri;
            for (ri = 0; ri < para->run_count; ri++) {
                const wp_char_style *cs = wp_document_cstyle_get(doc, para->runs[ri].style);
                if (cs != NULL) rtf_color_index(ct, cs->color_r, cs->color_g, cs->color_b);
            }
        }
    }
}

static void rtf_emit_header(rtf_writer *w, const rtf_color_table *ct)
{
    u32 i;

    rtf_write_str(w, "{\\rtf1\\ansi\\ansicpg1252\\deff0"
                     "{\\fonttbl{\\f0 Helvetica;}}"); /* honest placeholder -- see docs/formats/rtf.md */

    if (ct->count > 1) {
        rtf_write_str(w, "{\\colortbl;");
        for (i = 1; i < ct->count; i++) {
            rtf_write_str(w, "\\red");   rtf_write_i32(w, (i32)ct->rgb[i][0]);
            rtf_write_str(w, "\\green"); rtf_write_i32(w, (i32)ct->rgb[i][1]);
            rtf_write_str(w, "\\blue");  rtf_write_i32(w, (i32)ct->rgb[i][2]);
            rtf_write_str(w, ";");
        }
        rtf_write_str(w, "}");
    }
}

static void rtf_emit_run(rtf_writer *w, const wp_document *doc, const wp_run *run,
                          const rtf_color_table *ct)
{
    const wp_char_style *cs = wp_document_cstyle_get(doc, run->style);
    u32 color_idx;

    if (cs == NULL) return;

    rtf_write_str(w, "{");
    if (cs->flags & WP_STYLE_BOLD)      rtf_write_str(w, "\\b");
    if (cs->flags & WP_STYLE_ITALIC)    rtf_write_str(w, "\\i");
    if (cs->flags & WP_STYLE_UNDERLINE) rtf_write_str(w, "\\ul");
    if (cs->flags & WP_STYLE_STRIKE)    rtf_write_str(w, "\\strike");
    if (cs->flags & WP_STYLE_SUPER)     rtf_write_str(w, "\\super");
    if (cs->flags & WP_STYLE_SUB)       rtf_write_str(w, "\\sub");
    rtf_write_str(w, "\\fs"); rtf_write_i32(w, cs->size_twips / 10);

    color_idx = rtf_color_lookup(ct, cs->color_r, cs->color_g, cs->color_b);
    if (color_idx != 0) { rtf_write_str(w, "\\cf"); rtf_write_i32(w, (i32)color_idx); }

    rtf_write_str(w, " ");
    rtf_write_text(w, wp_strpool_get(&doc->strings, run->text), run->text.length);
    rtf_write_str(w, "}");
}

/* -- Image paragraphs ---------------------------------------------- */

/* A placed image, as a real RTF \pict carrying a Windows DIB.
 *
 * \dibitmap0 rather than a private destination, because this is the one
 * shape a foreign word processor can actually open: the document stops
 * being RetroWP-only the moment the picture is standard.  The model
 * already stores exactly what a 4-bit DIB wants -- a palette of up to 16
 * RGB triples and packed 4-bit indices -- so the conversion is a header,
 * a palette in BGRA order, and the rows turned upside down.
 *
 * Three DIB details that are easy to get wrong and silently produce a
 * picture no reader will show:
 *   - every field is LITTLE-endian, unlike everything else this project
 *     writes to disk (CLAUDE.md's big-endian rule is about OUR formats;
 *     a DIB is a foreign one and has its own order),
 *   - a positive biHeight means the rows are stored BOTTOM-UP, and
 *   - each row is padded to a 4-byte boundary, which the model's rows
 *     are not. */

static void dib_u32(u8 *p, u32 v)      /* little-endian, deliberately */
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

static void dib_u16(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

static void rtf_write_hex(rtf_writer *w, const u8 *bytes, u32 n)
{
    static const char digits[] = "0123456789abcdef";
    u32 i;

    for (i = 0; i < n; i++) {
        char pair[2];
        pair[0] = digits[(bytes[i] >> 4) & 0x0Fu];
        pair[1] = digits[bytes[i] & 0x0Fu];
        rtf_write_bytes(w, (const u8 *)pair, 2);
    }
}

/* The bracketed marker an image becomes when there is no way to get its
   pixels -- no wp_image_source, or no descriptor. Deliberately the same
   wording platform/dos prints on screen for the same reason. */
static void rtf_emit_image_placeholder(rtf_writer *w, const wp_document *doc,
                                        const wp_image_desc *desc)
{
    rtf_write_str(w, "{\\f0 [image");
    if (desc != NULL) {
        if (desc->source_name.length > 0) {
            rtf_write_str(w, ": ");
            rtf_write_text(w, wp_strpool_get(&doc->strings, desc->source_name),
                            desc->source_name.length);
        }
        rtf_write_str(w, " ");
        rtf_write_i32(w, desc->width_px);
        rtf_write_str(w, "x");
        rtf_write_i32(w, desc->height_px);
    }
    rtf_write_str(w, "]}");
}

static void rtf_emit_image(rtf_writer *w, const wp_document *doc,
                            const wp_paragraph *para, const wp_image_source *src)
{
    const wp_image_desc *desc = wp_document_image_get(doc, para->image);
    u32 row_bytes, padded, y;
    u8 header[40];
    u8 *row;

    if (desc == NULL || desc->width_px <= 0 || desc->height_px <= 0 ||
        desc->palette_count < 1 || src == NULL || src->read == NULL) {
        rtf_emit_image_placeholder(w, doc, desc);
        return;
    }

    row_bytes = ((u32)desc->width_px + 1u) / 2u;
    padded = (row_bytes + 3u) & ~3u;

    row = (u8 *)mem_alloc(padded);
    if (row == NULL) {              /* the measure pass never allocates */
        rtf_emit_image_placeholder(w, doc, desc);
        return;
    }

    rtf_write_str(w, "{\\pict\\dibitmap0");
    rtf_write_str(w, "\\picw");     rtf_write_i32(w, desc->width_px);
    rtf_write_str(w, "\\pich");     rtf_write_i32(w, desc->height_px);
    rtf_write_str(w, "\\picwgoal"); rtf_write_i32(w, desc->display_width_twips);
    rtf_write_str(w, "\\pichgoal"); rtf_write_i32(w, desc->display_height_twips);
    rtf_write_str(w, "\r\n");

    /* BITMAPINFOHEADER */
    memset(header, 0, sizeof(header));
    dib_u32(header + 0, 40u);                       /* biSize */
    dib_u32(header + 4, (u32)desc->width_px);       /* biWidth */
    dib_u32(header + 8, (u32)desc->height_px);      /* biHeight, +ve: bottom-up */
    dib_u16(header + 12, 1u);                       /* biPlanes */
    dib_u16(header + 14, 4u);                       /* biBitCount */
    dib_u32(header + 20, padded * (u32)desc->height_px);  /* biSizeImage */
    dib_u32(header + 32, (u32)desc->palette_count); /* biClrUsed */
    rtf_write_hex(w, header, 40u);

    {   /* palette, BGRA per RGBQUAD */
        u32 i;
        for (i = 0; i < (u32)desc->palette_count; i++) {
            u8 quad[4];
            quad[0] = desc->palette[i][2];
            quad[1] = desc->palette[i][1];
            quad[2] = desc->palette[i][0];
            quad[3] = 0;
            rtf_write_hex(w, quad, 4u);
        }
    }

    /* Rows, last first.  A read that fails or falls short leaves zeros
       behind it: the measure pass has already promised this exact byte
       count and a save must not produce a different one. */
    for (y = (u32)desc->height_px; y > 0; y--) {
        u32 got = 0;

        memset(row, 0, padded);
        if (w->out != NULL) {
            if (src->read(src->user, desc->blob_path, (y - 1u) * row_bytes,
                          row, row_bytes, &got) != WP_OK) got = 0;
            if (got > row_bytes) got = row_bytes;
            if (got < row_bytes) memset(row + got, 0, row_bytes - got);
        }
        rtf_write_hex(w, row, padded);
        if ((y & 7u) == 1u) rtf_write_str(w, "\r\n");   /* keep lines sane */
    }

    mem_free(row);
    rtf_write_str(w, "}");
}

static void rtf_emit_paragraph(rtf_writer *w, const wp_document *doc, const wp_paragraph *para,
                                const rtf_color_table *ct, const wp_image_source *src)
{
    const wp_para_style *ps = wp_document_pstyle_get(doc, para->style);
    u16 ri;

    rtf_write_str(w, "\\pard");
    if (ps != NULL) {
        if (ps->align == WP_ALIGN_RIGHT)  rtf_write_str(w, "\\qr");
        if (ps->align == WP_ALIGN_CENTER) rtf_write_str(w, "\\qc");
        if (ps->align == WP_ALIGN_JUSTIFY) rtf_write_str(w, "\\qj");
        if (ps->indent_left != 0)   { rtf_write_str(w, "\\li"); rtf_write_i32(w, ps->indent_left); }
        if (ps->indent_right != 0)  { rtf_write_str(w, "\\ri"); rtf_write_i32(w, ps->indent_right); }
        if (ps->indent_first != 0)  { rtf_write_str(w, "\\fi"); rtf_write_i32(w, ps->indent_first); }
        if (ps->space_before != 0)  { rtf_write_str(w, "\\sb"); rtf_write_i32(w, ps->space_before); }
        if (ps->space_after != 0)   { rtf_write_str(w, "\\sa"); rtf_write_i32(w, ps->space_after); }
    }

    if (para->kind == (u8)WP_PARA_IMAGE) {
        /* An image paragraph has no runs at all, so the loop below would
           emit an empty paragraph and the picture would be gone. */
        rtf_emit_image(w, doc, para, src);
    } else {
        for (ri = 0; ri < para->run_count; ri++) {
            rtf_emit_run(w, doc, &para->runs[ri], ct);
        }
    }

    rtf_write_str(w, "\\par\r\n");
}

static void rtf_emit_document(rtf_writer *w, const wp_document *doc, const rtf_color_table *ct,
                               const wp_image_source *src)
{
    u32 si, pi;

    rtf_emit_header(w, ct);

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            rtf_emit_paragraph(w, doc, &sec->paras[pi], ct, src);
        }
    }

    rtf_write_str(w, "}");
}

wp_status format_rtf_export_images(const wp_document *doc, u8 *out, u32 out_cap,
                                    u32 *out_len, const wp_image_source *src)
{
    rtf_color_table ct;
    rtf_writer w;

    rtf_build_color_table(doc, &ct);

    w.out = NULL;
    w.cap = 0;
    w.pos = 0;
    rtf_emit_document(&w, doc, &ct, src);

    if (out_len) *out_len = w.pos;
    if (w.pos > out_cap) return WP_RANGE;

    w.out = out;
    w.cap = out_cap;
    w.pos = 0;
    rtf_emit_document(&w, doc, &ct, src);

    return WP_OK;
}

wp_status format_rtf_export(const wp_document *doc, u8 *out, u32 out_cap, u32 *out_len)
{
    return format_rtf_export_images(doc, out, out_cap, out_len, NULL);
}
