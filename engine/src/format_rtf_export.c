/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "merge.h"
#include "styles.h"
#include "format_rtf.h"
#include "utf8.h"
#include "format_rtf_internal.h"
#include "codepage.h"
#include "format_wmf.h"
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

/* Escapes and writes UTF-8 text as RTF text: '{'/'}'/'\\' are escaped,
   ASCII passes through, cp1252 characters go as '\'hh', and anything
   else as \uN followed by a '?' for readers that do not know \u (the
   header's \uc1 says one fallback character follows). N is RTF's signed
   16-bit; a character beyond the BMP has no \u spelling and goes as '?'. */
static void rtf_write_text(rtf_writer *w, const u8 *bytes, u32 len)
{
    u32 pos = 0;

    while (pos < len) {
        u32 cp;
        u32 n = utf8_decode(bytes + pos, len - pos, &cp);

        if (n == 0) { pos++; continue; } /* skip an invalid byte rather than emit garbage */

        if (cp == '\t') {
            rtf_write_str(w, "\\tab ");   /* a raw tab byte is ignorable whitespace to many readers */
        } else if (cp == '{' || cp == '}' || cp == '\\') {
            u8 esc[2];
            esc[0] = '\\';
            esc[1] = (u8)cp;
            rtf_write_bytes(w, esc, 2);
        } else if (cp < 0x80) {
            u8 b = (u8)cp;
            rtf_write_bytes(w, &b, 1);
        } else {
            u8 native;
            if (wp_codepage_encode(1252, cp, &native)) {
                char hex[4];
                hex[0] = '\\'; hex[1] = '\'';
                hex[2] = "0123456789abcdef"[native >> 4];
                hex[3] = "0123456789abcdef"[native & 0xF];
                rtf_write_bytes(w, (const u8 *)hex, 4);
            } else if (cp <= 0xFFFF) {
                rtf_write_str(w, "\\u");
                rtf_write_i32(w, (cp >= 0x8000) ? (i32)cp - 65536 : (i32)cp);
                rtf_write_str(w, "?");
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
    /* Which of the document's fonts some run uses (bit per wp_font id;
       ids past the bitmap are taken as used): only those reach the
       \fonttbl, numbered by their own id, so a run's \fN is its id and
       \f0 is the target default. */
    u8  font_used[32];
} rtf_color_table;

static wp_bool rtf_font_used(const rtf_color_table *ct, wp_font id)
{
    if (id >= 256u) return WP_TRUE;
    return (ct->font_used[id >> 3] & (u8)(1u << (id & 7u))) ? WP_TRUE : WP_FALSE;
}

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
    memset(ct->font_used, 0, sizeof(ct->font_used));

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            u16 ri;
            for (ri = 0; ri < para->run_count; ri++) {
                const wp_char_style *cs = wp_document_cstyle_get(doc, para->runs[ri].style);
                if (cs != NULL) rtf_color_index(ct, cs->color_r, cs->color_g, cs->color_b);
                if (cs != NULL && cs->font != 0 && cs->font < 256u) {
                    ct->font_used[cs->font >> 3] = (u8)(ct->font_used[cs->font >> 3] | (1u << (cs->font & 7u)));
                }
            }
        }
    }
    {
        /* The stylesheet's character formats too: a style names its face
           and colour even where no run is in it. */
        u16 id;
        for (id = 1; id < doc->named_count; id++) {
            const wp_char_style *cs = wp_document_cstyle_get(doc, doc->named[id].chars);
            if (cs == NULL) continue;
            rtf_color_index(ct, cs->color_r, cs->color_g, cs->color_b);
            if (cs->font != 0 && cs->font < 256u) {
                ct->font_used[cs->font >> 3] = (u8)(ct->font_used[cs->font >> 3] | (1u << (cs->font & 7u)));
            }
        }
    }
}

static void rtf_emit_para_props(rtf_writer *w, const wp_para_style *ps);
static void rtf_emit_char_props(rtf_writer *w, const wp_char_style *cs, const rtf_color_table *ct);

/* A section's columns, where it has more than one (\colsN, the gap
   \colsxN). */
static void rtf_emit_columns(rtf_writer *w, const wp_section *sec)
{
    if (sec->columns <= 1) return;
    rtf_write_str(w, "\\cols"); rtf_write_i32(w, (i32)sec->columns);
    rtf_write_str(w, "\\colsx"); rtf_write_i32(w, sec->col_gap > 0 ? sec->col_gap : 720);
}

static void rtf_emit_header(rtf_writer *w, const wp_document *doc, const rtf_color_table *ct)
{
    u32 i;

    static const char *const family_word[6] = {
        "\\fnil", "\\froman", "\\fswiss", "\\fmodern", "\\fscript", "\\fdecor"
    };
    wp_font id;

    /* \f0 is the target's default face, which has no name of its own to
       write: "Helvetica" stands in for it (a reader needs some name),
       and the \*\generator below is how this program's reader knows to
       take \f0 back as the default rather than as Helvetica. Every other
       entry is a face a run names, numbered by its font id. */
    rtf_write_str(w, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1\\deftab720"
                     "{\\fonttbl{\\f0\\fswiss Helvetica;}");
    for (id = 1; id < doc->font_count; id++) {
        const wp_font_desc *fd = wp_document_font_get(doc, id);
        if (fd == NULL || !rtf_font_used(ct, id)) continue;
        rtf_write_str(w, "{\\f"); rtf_write_i32(w, (i32)id);
        rtf_write_str(w, family_word[fd->family <= 5 ? fd->family : 0]);
        rtf_write_str(w, " ");
        rtf_write_text(w, wp_strpool_get(&doc->strings, fd->name), fd->name.length);
        rtf_write_str(w, ";}");
    }
    rtf_write_str(w, "}");

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
    if (doc->named_count > 1) {
        /* The stylesheet (styles.h): \\sN for each style, its formats and
           its name; \\s0 Normal too where the document has no Normal of
           its own, as readers expect a \\s0. */
        u16 id;
        rtf_write_str(w, "{\\stylesheet");
        if (wp_style_find(doc, (const u8 *)"Normal", 6) == 0) rtf_write_str(w, "{\\s0 Normal;}");
        for (id = 1; id < doc->named_count; id++) {
            u32 len;
            const u8 *nm = wp_style_name(doc, id, &len);
            if (nm == NULL) continue;
            rtf_write_str(w, "{\\s"); rtf_write_i32(w, (i32)id);
            rtf_emit_para_props(w, wp_document_pstyle_get(doc, doc->named[id].para));
            rtf_emit_char_props(w, wp_document_cstyle_get(doc, doc->named[id].chars), ct);
            rtf_write_str(w, " ");
            rtf_write_text(w, nm, len);
            rtf_write_str(w, ";}");
        }
        rtf_write_str(w, "}");
    }
    rtf_write_str(w, "{\\*\\generator RetroWP;}");

    /* Page setup, from the first section: the document's page as the
       reader should lay it out. */
    if (doc->section_count > 0) {
        const wp_section *sec = &doc->sections[0];
        rtf_write_str(w, "\\paperw"); rtf_write_i32(w, sec->page_width);
        rtf_write_str(w, "\\paperh"); rtf_write_i32(w, sec->page_height);
        rtf_write_str(w, "\\margl");  rtf_write_i32(w, sec->margin_left);
        rtf_write_str(w, "\\margr");  rtf_write_i32(w, sec->margin_right);
        rtf_write_str(w, "\\margt");  rtf_write_i32(w, sec->margin_top);
        rtf_write_str(w, "\\margb");  rtf_write_i32(w, sec->margin_bottom);
        rtf_emit_columns(w, sec);
        rtf_write_str(w, "\r\n");
    }
}

/* A run's text inside its group. */
static void rtf_emit_run_text(rtf_writer *w, const wp_document *doc, const wp_run *run);

/* Runs first.. that share first's format, as ONE group: typing and undo
   leave neighbouring runs in the same format (docs/editing.md), and a
   group each made a recovery file one group per keystroke. Answers the
   run after the last one written. */
static u16 rtf_emit_runs(rtf_writer *w, const wp_document *doc, const wp_paragraph *para,
                         u16 first, const rtf_color_table *ct)
{
    const wp_run *run = &para->runs[first];
    const wp_char_style *cs = wp_document_cstyle_get(doc, run->style);
    u32 color_idx;
    u16 ri;

    if (cs == NULL) return (u16)(first + 1);

    rtf_write_str(w, "{");
    if (cs->font != 0) { rtf_write_str(w, "\\f"); rtf_write_i32(w, (i32)cs->font); }
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
    for (ri = first; ri < para->run_count && para->runs[ri].style == run->style; ri++) {
        rtf_emit_run_text(w, doc, &para->runs[ri]);
    }
    rtf_write_str(w, "}");
    return ri;
}

static void rtf_emit_run_text(rtf_writer *w, const wp_document *doc, const wp_run *run)
{
    {
        /* A merge field «Name» (merge.h) goes as a real MERGEFIELD, its
           shown result the «Name» itself -- as Word writes one, so Word
           merges with it and every reader without fields still shows
           the name. A name with a space is quoted. */
        const u8 *t = wp_strpool_get(&doc->strings, run->text);
        u32 pos = 0, start, end, na, nl, k;
        while (t != NULL && wp_merge_next_field(t, run->text.length, pos, &start, &end, &na, &nl)) {
            wp_bool spaced = WP_FALSE;
            for (k = 0; k < nl; k++) if (t[na + k] == ' ') spaced = WP_TRUE;
            rtf_write_text(w, t + pos, start - pos);
            rtf_write_str(w, "{\\field{\\*\\fldinst MERGEFIELD ");
            if (spaced) rtf_write_str(w, "\"");
            rtf_write_text(w, t + na, nl);
            if (spaced) rtf_write_str(w, "\"");
            rtf_write_str(w, " }{\\fldrslt ");
            rtf_write_text(w, t + start, end - start);
            rtf_write_str(w, "}}");
            pos = end;
        }
        if (t != NULL) rtf_write_text(w, t + pos, run->text.length - pos);
    }
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

static void put_wmf_hex(void *user, const u8 *bytes, u32 n)
{
    rtf_write_hex((rtf_writer *)user, bytes, n);
}

/* A drawing: a Windows metafile picture, which Word and the rest read
   (docs/document-model.md, "Drawings"). \picw/\pich are a metafile's
   size in hundredths of a millimetre; the goal is the display box. */
static void rtf_emit_drawing(rtf_writer *w, const wp_document *doc, const wp_image_desc *desc)
{
    const u8 *ops = wp_strpool_get(&doc->strings, desc->drawing);

    if (drawing_to_wmf(ops, desc->drawing.length, desc->width_px, desc->height_px,
                       NULL, NULL, NULL) != WP_OK) {
        rtf_emit_image_placeholder(w, doc, desc);
        return;
    }
    rtf_write_str(w, "{\\pict\\wmetafile8");
    rtf_write_str(w, "\\picw");     rtf_write_i32(w, (desc->display_width_twips * 2540L) / 1440L);
    rtf_write_str(w, "\\pich");     rtf_write_i32(w, (desc->display_height_twips * 2540L) / 1440L);
    rtf_write_str(w, "\\picwgoal"); rtf_write_i32(w, desc->display_width_twips);
    rtf_write_str(w, "\\pichgoal"); rtf_write_i32(w, desc->display_height_twips);
    rtf_write_str(w, "\r\n");
    (void)drawing_to_wmf(ops, desc->drawing.length, desc->width_px, desc->height_px,
                         put_wmf_hex, w, NULL);
    rtf_write_str(w, "}");
}

static void rtf_emit_image(rtf_writer *w, const wp_document *doc,
                            const wp_paragraph *para, const wp_image_source *src)
{
    const wp_image_desc *desc = wp_document_image_get(doc, para->image);
    u32 row_bytes, padded, y;
    u8 header[40];
    u8 *row;

    if (desc != NULL && WP_IMAGE_IS_DRAWING(desc)) {
        rtf_emit_drawing(w, doc, desc);
        return;
    }

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

/* A paragraph format's words -- alignment, indents, spacing, borders,
   shading, line spacing, tabs -- for a paragraph or a stylesheet entry. */
static void rtf_emit_para_props(rtf_writer *w, const wp_para_style *ps)
{
    if (ps != NULL) {
        if (ps->align == WP_ALIGN_RIGHT)  rtf_write_str(w, "\\qr");
        if (ps->align == WP_ALIGN_CENTER) rtf_write_str(w, "\\qc");
        if (ps->align == WP_ALIGN_JUSTIFY) rtf_write_str(w, "\\qj");
        if (ps->indent_left != 0)   { rtf_write_str(w, "\\li"); rtf_write_i32(w, ps->indent_left); }
        if (ps->indent_right != 0)  { rtf_write_str(w, "\\ri"); rtf_write_i32(w, ps->indent_right); }
        if (ps->indent_first != 0)  { rtf_write_str(w, "\\fi"); rtf_write_i32(w, ps->indent_first); }
        if (ps->space_before != 0)  { rtf_write_str(w, "\\sb"); rtf_write_i32(w, ps->space_before); }
        if (ps->space_after != 0)   { rtf_write_str(w, "\\sa"); rtf_write_i32(w, ps->space_after); }
        if (ps->page_break_before == WP_BREAK_PAGE) rtf_write_str(w, "\\pagebb");
        if (ps->border_sides != 0) {
            /* \box for all four, else each side; every side carries the
               one kind, width and spacing the model keeps. */
            static const char *const side_words[4] = { "\\brdrt", "\\brdrb", "\\brdrl", "\\brdrr" };
            u8 k;
            for (k = 0; k < 4; k++) {
                if (ps->border_sides == WP_BORDER_BOX && k > 0) break;
                if (ps->border_sides != WP_BORDER_BOX && !(ps->border_sides & (1u << k))) continue;
                rtf_write_str(w, ps->border_sides == WP_BORDER_BOX ? "\\box" : side_words[k]);
                rtf_write_str(w, ps->border_kind == WP_BORDER_DOUBLE ? "\\brdrdb" :
                                 ps->border_kind == WP_BORDER_THICK ? "\\brdrth" : "\\brdrs");
                rtf_write_str(w, "\\brdrw");
                rtf_write_i32(w, ps->border_width > 0 ? ps->border_width : WP_BORDER_DEFAULT_WIDTH);
                if (ps->border_space != 0) { rtf_write_str(w, "\\brsp"); rtf_write_i32(w, ps->border_space); }
            }
        }
        if (ps->shading != 0) { rtf_write_str(w, "\\shading"); rtf_write_i32(w, (i32)ps->shading * 100); }
        /* The model's line spacing back to RTF's (docmodel.h). */
        if (ps->line_spacing > 0) {
            rtf_write_str(w, "\\sl"); rtf_write_i32(w, ps->line_spacing); rtf_write_str(w, "\\slmult1");
        } else if (ps->line_spacing < 0) {
            rtf_write_str(w, "\\sl"); rtf_write_i32(w, -ps->line_spacing); rtf_write_str(w, "\\slmult0");
        }
        {
            u8 t;
            for (t = 0; t < ps->tab_count && t < WP_MAX_TABS; t++) {
                if (ps->tab_kind[t] == (u8)WP_TAB_RIGHT) rtf_write_str(w, "\\tqr");
                else if (ps->tab_kind[t] == (u8)WP_TAB_CENTER) rtf_write_str(w, "\\tqc");
                else if (ps->tab_kind[t] == (u8)WP_TAB_DECIMAL) rtf_write_str(w, "\\tqdec");
                rtf_write_str(w, "\\tx"); rtf_write_i32(w, ps->tab_pos[t]);
            }
        }
    }
}

/* A character format's words, for a stylesheet entry (a run writes its
   own, in rtf_emit_runs). */
static void rtf_emit_char_props(rtf_writer *w, const wp_char_style *cs, const rtf_color_table *ct)
{
    u32 color_idx;
    if (cs == NULL) return;
    if (cs->font != 0) { rtf_write_str(w, "\\f"); rtf_write_i32(w, (i32)cs->font); }
    if (cs->flags & WP_STYLE_BOLD)      rtf_write_str(w, "\\b");
    if (cs->flags & WP_STYLE_ITALIC)    rtf_write_str(w, "\\i");
    if (cs->flags & WP_STYLE_UNDERLINE) rtf_write_str(w, "\\ul");
    rtf_write_str(w, "\\fs"); rtf_write_i32(w, cs->size_twips / 10);
    color_idx = rtf_color_lookup(ct, cs->color_r, cs->color_g, cs->color_b);
    if (color_idx != 0) { rtf_write_str(w, "\\cf"); rtf_write_i32(w, (i32)color_idx); }
}

static void rtf_emit_row_shape(rtf_writer *w, const wp_table_row *row);

static void rtf_emit_paragraph(rtf_writer *w, const wp_document *doc, const wp_paragraph *para,
                                const rtf_color_table *ct, const wp_image_source *src);

/* Note `id`'s paragraphs, wherever they are, as a \footnote group. */
static void rtf_emit_footnote(rtf_writer *w, const wp_document *doc, u16 id,
                              const rtf_color_table *ct, const wp_image_source *src)
{
    u32 si, pi;
    rtf_write_str(w, "{\\footnote");
    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            if (sec->paras[pi].note == id) rtf_emit_paragraph(w, doc, &sec->paras[pi], ct, src);
        }
    }
    rtf_write_str(w, "}");
}

static void rtf_emit_paragraph(rtf_writer *w, const wp_document *doc, const wp_paragraph *para,
                                const rtf_color_table *ct, const wp_image_source *src)
{
    const wp_para_style *ps = wp_document_pstyle_get(doc, para->style);
    u16 ri;

    /* A column break is a character in Word, not a paragraph property:
       written before the paragraph, as Word's "\column" between two. */
    if (ps != NULL && ps->page_break_before == WP_BREAK_COLUMN) rtf_write_str(w, "\\column\r\n");
    rtf_write_str(w, "\\pard");
    if (para->trow != 0) rtf_write_str(w, "\\intbl");
    {
        const wp_frame_desc *fr = wp_document_frame_get(doc, para->frame);
        if (fr != NULL && para->trow == 0) {
            rtf_write_str(w, fr->xref == WP_FRAME_X_PAGE ? "\\phpg" : "\\phmrg");
            rtf_write_str(w, fr->yref == WP_FRAME_Y_PAGE ? "\\pvpg" :
                             fr->yref == WP_FRAME_Y_MARGIN ? "\\pvmrg" : "\\pvpara");
            rtf_write_str(w, "\\posx"); rtf_write_i32(w, fr->x);
            rtf_write_str(w, "\\posy"); rtf_write_i32(w, fr->y);
            if (fr->w > 0) { rtf_write_str(w, "\\absw"); rtf_write_i32(w, fr->w); }
            if (fr->h != 0) { rtf_write_str(w, "\\absh"); rtf_write_i32(w, fr->h); }
        }
    }
    if (ps != NULL && ps->style_id != 0) { rtf_write_str(w, "\\s"); rtf_write_i32(w, (i32)ps->style_id); }
    rtf_emit_para_props(w, ps);

    if (para->kind == (u8)WP_PARA_IMAGE) {
        /* An image paragraph has no runs at all, so the loop below would
           emit an empty paragraph and the picture would be gone. */
        rtf_emit_image(w, doc, para, src);
    } else {
        for (ri = 0; ri < para->run_count; ) {
            const wp_char_style *rcs = wp_document_cstyle_get(doc, para->runs[ri].style);
            if (rcs != NULL && rcs->note != 0) {
                /* A footnote reference (notes.h): Word's own form, the
                   note's paragraphs following in their \footnote group;
                   in the note itself, just the mark. */
                rtf_write_str(w, "{\\super\\chftn}");
                if (para->note == 0) rtf_emit_footnote(w, doc, rcs->note, ct, src);
                ri++;
                continue;
            }
            ri = rtf_emit_runs(w, doc, para, (u16)ri, ct);
        }
    }

    /* A table paragraph ends its cell, its row, or just itself. */
    if (para->trow != 0 && (para->tflags & WP_TROW_END)) rtf_write_str(w, "\\cell\\row\r\n");
    else if (para->trow != 0 && (para->tflags & WP_TCELL_END)) rtf_write_str(w, "\\cell\r\n");
    else rtf_write_str(w, "\\par\r\n");
}

/* A table row's shape before its first paragraph: \trowd, the gap and
   left edge, then each cell's borders, shading and right edge. */
static void rtf_emit_row_shape(rtf_writer *w, const wp_table_row *row)
{
    static const char *const sides[4] = { "\\clbrdrt", "\\clbrdrb", "\\clbrdrl", "\\clbrdrr" };
    u8 k, b;

    if (row == NULL) return;
    rtf_write_str(w, "\\trowd\\trgaph"); rtf_write_i32(w, row->gap);
    rtf_write_str(w, "\\trleft"); rtf_write_i32(w, row->left);
    if (row->height != 0) { rtf_write_str(w, "\\trrh"); rtf_write_i32(w, row->height); }
    for (k = 0; k < row->cell_count && k < WP_MAX_CELLS; k++) {
        for (b = 0; b < 4; b++) {
            if (!(row->cell_borders[k] & (1u << b))) continue;
            rtf_write_str(w, sides[b]);
            rtf_write_str(w, row->border_kind == WP_BORDER_DOUBLE ? "\\brdrdb" :
                             row->border_kind == WP_BORDER_THICK ? "\\brdrth" : "\\brdrs");
            rtf_write_str(w, "\\brdrw");
            rtf_write_i32(w, row->border_width > 0 ? row->border_width : WP_BORDER_DEFAULT_WIDTH);
        }
        if (row->cell_shading[k] != 0) { rtf_write_str(w, "\\clshdng"); rtf_write_i32(w, (i32)row->cell_shading[k] * 100); }
        rtf_write_str(w, "\\cellx"); rtf_write_i32(w, row->cell_right[k]);
    }
    rtf_write_str(w, "\r\n");
}

/* A running header or footer (docmodel.h): one paragraph, its '|' parts
   on tab stops -- one part centred, two left and right, three left,
   centre and right -- and '#' as \chpgn, the page number. */
static void rtf_emit_margin_text(rtf_writer *w, const wp_document *doc, const wp_section *sec,
                                 const char *dest, wp_span text)
{
    const u8 *b = wp_strpool_get(&doc->strings, text);
    i32 col = sec->page_width - sec->margin_left - sec->margin_right;
    u32 parts = 1, i, start;

    for (i = 0; i < text.length; i++) if (b[i] == '|') parts++;
    rtf_write_str(w, "{\\");
    rtf_write_str(w, dest);
    rtf_write_str(w, "\\pard\\plain");
    if (parts == 1) {
        rtf_write_str(w, "\\qc");
    } else {
        if (parts >= 3) { rtf_write_str(w, "\\tqc\\tx"); rtf_write_i32(w, col / 2); }
        rtf_write_str(w, "\\tqr\\tx"); rtf_write_i32(w, col);
    }
    rtf_write_str(w, " ");
    start = 0;
    for (i = 0; i <= text.length; i++) {
        if (i == text.length || b[i] == '|' || b[i] == '#') {
            if (i > start) rtf_write_text(w, b + start, i - start);
            if (i < text.length) rtf_write_str(w, (b[i] == '|') ? "\\tab " : "\\chpgn ");
            start = i + 1;
        }
    }
    rtf_write_str(w, "\\par}\r\n");
}

static void rtf_emit_document(rtf_writer *w, const wp_document *doc, const rtf_color_table *ct,
                               const wp_image_source *src)
{
    u32 si, pi;

    rtf_emit_header(w, doc, ct);

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        const wp_section *prev = (si > 0) ? &doc->sections[si - 1] : NULL;

        /* Each section after the first: \sect, then \sectd and its own
           page (the first's is the document's, in the header). */
        if (prev != NULL) {
            rtf_write_str(w, "\\sect\\sectd");
            rtf_write_str(w, "\\pgwsxn"); rtf_write_i32(w, sec->page_width);
            rtf_write_str(w, "\\pghsxn"); rtf_write_i32(w, sec->page_height);
            rtf_write_str(w, "\\marglsxn"); rtf_write_i32(w, sec->margin_left);
            rtf_write_str(w, "\\margrsxn"); rtf_write_i32(w, sec->margin_right);
            rtf_write_str(w, "\\margtsxn"); rtf_write_i32(w, sec->margin_top);
            rtf_write_str(w, "\\margbsxn"); rtf_write_i32(w, sec->margin_bottom);
            rtf_emit_columns(w, sec);
            rtf_write_str(w, "\r\n");
        }
        /* Its header and footer -- an empty one only where the section
           before had one, which is how a section says it has none (a
           reader carries the last one on otherwise). */
        if (sec->header.length > 0 || (prev != NULL && prev->header.length > 0)) {
            rtf_emit_margin_text(w, doc, sec, "header", sec->header);
        }
        if (sec->footer.length > 0 || (prev != NULL && prev->footer.length > 0)) {
            rtf_emit_margin_text(w, doc, sec, "footer", sec->footer);
        }
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            if (para->note != 0) continue;   /* written where it is referred to */
            if (para->trow != 0 &&
                (pi == 0 || sec->paras[pi - 1].trow == 0 || (sec->paras[pi - 1].tflags & WP_TROW_END))) {
                rtf_emit_row_shape(w, wp_document_trow_get(doc, para->trow));
            }
            rtf_emit_paragraph(w, doc, para, ct, src);
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
