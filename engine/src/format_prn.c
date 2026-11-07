/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_prn.h"
#include "codepage.h"
#include "platform.h"
#include "utf8.h"

/* The widest line this lays out: 25.5 inches at 10 cpi, past any paper
   a character printer feeds. A wider page is printed this wide. */
#define PRN_MAX_COLS 255u

typedef struct {
    const wp_document    *doc;
    const wp_prn_options *opt;
    wp_prn_write_fn       write;
    void                 *user;
    wp_status             st;

    u32 cpi, lpi;
    i32 line_twips;          /* 1440 / lpi */

    /* The current section, at the printer's pitch. */
    const wp_section *sec;
    u32 cols;                /* the text column's width */
    u32 left_pad;            /* the left margin, in spaces */
    u32 top, body, bottom;   /* lines in the top margin, text, bottom margin */

    wp_bool page_open;
    u32     row;             /* the next text line on the page, 0..body */
    u32     prow;            /* PostScript: the physical line on the page */
    u8      cur_attr;        /* the style the printer is in now (PRN_A_*) */
    i32     sent_w, sent_h;  /* the paper size last sent to the printer */
    u32     page_no;         /* pages begun so far, the document through */
    u32     pending;         /* blank lines owed to spacing */
    i32     space_acc;       /* spacing in twips not yet a whole line */

    /* Working lines, here rather than on the stack (the Amiga's is 4 KB).
       Each text byte has a style byte beside it (PRN_A_*). */
    u8 lb[PRN_MAX_COLS + 1], la[PRN_MAX_COLS + 1];  /* one laid-out line */
    u8 hb[PRN_MAX_COLS + 1];       /* a header or footer, laid out */
    u8 za[PRN_MAX_COLS + 1];       /* no style, for headers and footers */
    u8 jb[PRN_MAX_COLS + 1], ja[PRN_MAX_COLS + 1];  /* a line being justified */
    u8 part[3][PRN_MAX_COLS + 1];  /* a header or footer's '|' parts */
} prn_state;

/* A printer byte's style. */
#define PRN_A_BOLD   0x01u
#define PRN_A_ITALIC 0x02u
#define PRN_A_UNDER  0x04u

static void put(prn_state *s, const u8 *bytes, u32 len)
{
    if (s->st != WP_OK || len == 0) return;
    s->st = s->write(s->user, bytes, len);
}

static void put_spaces(prn_state *s, u32 n)
{
    static const u8 sp[16] = { ' ',' ',' ',' ',' ',' ',' ',' ',
                               ' ',' ',' ',' ',' ',' ',' ',' ' };
    while (n > 0) {
        u32 k = n > 16u ? 16u : n;
        put(s, sp, k);
        n -= k;
    }
}

static void put_str(prn_state *s, const char *str)
{
    put(s, (const u8 *)str, (u32)strlen(str));
}

static void put_num(prn_state *s, long v)
{
    char d[12];
    u32 n = 0;
    unsigned long u = (unsigned long)(v < 0 ? -v : v);

    if (v < 0) put_str(s, "-");
    do { d[n++] = (char)('0' + u % 10u); u /= 10u; } while (u > 0 && n < 11);
    while (n > 0) put(s, (const u8 *)&d[--n], 1);
}

/* Tenths of a point as "123.4". */
static void put_tenths(prn_state *s, long t)
{
    if (t < 0) { put_str(s, "-"); t = -t; }
    put_num(s, t / 10);
    put_str(s, ".");
    put_num(s, t % 10);
}

static void put_eol(prn_state *s)
{
    static const u8 crlf[2] = { '\r', '\n' };
    if (s->opt->crlf) put(s, crlf, 2);
    else put(s, crlf + 1, 1);
}

static u32 twips_to(i32 twips, u32 per_inch)
{
    if (twips <= 0) return 0;
    return (u32)(((u32)twips * per_inch + 720u) / 1440u);
}

/* ---- Drivers ---------------------------------------------------------
   Everything that reaches the printer goes through these: the start and
   end of the document and of each page, a line of text, a blank line.
   The text drivers write a character stream with their own escape codes
   for style; PostScript places each line on a page instead. */

static void style_codes(prn_state *s, u8 from, u8 to)
{
    u8 off = (u8)(from & (u8)~to), on = (u8)(to & (u8)~from);

    switch (s->opt->driver) {
    case WP_PRN_EPSON:
        if (off & PRN_A_BOLD)   put_str(s, "\033F");
        if (off & PRN_A_ITALIC) put_str(s, "\0335");
        if (off & PRN_A_UNDER)  put_str(s, "\033-0");
        if (on & PRN_A_BOLD)    put_str(s, "\033E");
        if (on & PRN_A_ITALIC)  put_str(s, "\0334");
        if (on & PRN_A_UNDER)   put_str(s, "\033-1");
        break;
    case WP_PRN_PCL:
        if (off & PRN_A_BOLD)   put_str(s, "\033(s0B");
        if (off & PRN_A_ITALIC) put_str(s, "\033(s0S");
        if (off & PRN_A_UNDER)  put_str(s, "\033&d@");
        if (on & PRN_A_BOLD)    put_str(s, "\033(s3B");
        if (on & PRN_A_ITALIC)  put_str(s, "\033(s1S");
        if (on & PRN_A_UNDER)   put_str(s, "\033&d0D");
        break;
    case WP_PRN_AMIGA:
        if (off & PRN_A_BOLD)   put_str(s, "\033[22m");
        if (off & PRN_A_ITALIC) put_str(s, "\033[23m");
        if (off & PRN_A_UNDER)  put_str(s, "\033[24m");
        if (on & PRN_A_BOLD)    put_str(s, "\033[1m");
        if (on & PRN_A_ITALIC)  put_str(s, "\033[3m");
        if (on & PRN_A_UNDER)   put_str(s, "\033[4m");
        break;
    default:
        break;
    }
    s->cur_attr = to;
}

/* PCL's paper-size code for a page, else 0 (none sent). */
static int pcl_paper(i32 w, i32 h)
{
    i32 lo = w < h ? w : h, hi = w < h ? h : w;
    if (lo >= 12100 && lo <= 12380 && hi >= 15700 && hi <= 15980) return 2;   /* Letter */
    if (lo >= 12100 && lo <= 12380 && hi >= 20020 && hi <= 20300) return 3;   /* Legal */
    if (lo >= 11766 && lo <= 12046 && hi >= 16698 && hi <= 16978) return 26;  /* A4 */
    if (lo >= 8251 && lo <= 8531 && hi >= 11766 && hi <= 12046) return 25;    /* A5 */
    if (lo >= 10300 && lo <= 10580 && hi >= 14980 && hi <= 15260) return 1;   /* Executive */
    return 0;
}

static void drv_begin_doc(prn_state *s)
{
    switch (s->opt->driver) {
    case WP_PRN_EPSON:
        put_str(s, "\033@");                                     /* reset */
        put_str(s, s->lpi == 8 ? "\0330" : "\0332");            /* 1/8" or 1/6" lines */
        put_str(s, s->cpi == 12 ? "\033M" : s->cpi >= 15 ? "\017" : "\033P");
        break;
    case WP_PRN_PCL:
        put_str(s, "\033E");                                     /* reset */
        put_str(s, "\033&l");                                    /* simplex/duplex */
        put_num(s, (long)s->opt->sides);
        put_str(s, "S\033&l0L\033&l");                          /* no perforation skip; lpi */
        put_num(s, (long)s->lpi);
        put_str(s, "D\033(10U\033(s0p");                        /* PC-8; fixed pitch */
        put_num(s, (long)s->cpi);
        put_str(s, "h0s0b3T");                                    /* upright, medium, Courier */
        break;
    case WP_PRN_AMIGA:
        put_str(s, "\033#1");                                    /* initialize */
        put_str(s, s->cpi == 12 ? "\033[2w" : s->cpi >= 15 ? "\033[4w" : "\033[0w");
        put_str(s, s->lpi == 8 ? "\033[0z" : "\033[1z");
        break;
    case WP_PRN_POSTSCRIPT: {
        long size = 1200L / (long)s->cpi;                        /* tenths of a point */
        put_str(s, "%!PS-Adobe-3.0\n%%Creator: RetroWP\n%%EndComments\n"
                   "/RE { findfont dup length dict begin "
                   "{ 1 index /FID ne { def } { pop pop } ifelse } forall "
                   "/Encoding ISOLatin1Encoding def currentdict end definefont pop } bind def\n"
                   "/F0n /Courier RE /F1n /Courier-Bold RE "
                   "/F2n /Courier-Oblique RE /F3n /Courier-BoldOblique RE\n");
        put_str(s, "/F0 { /F0n findfont "); put_tenths(s, size); put_str(s, " scalefont setfont } bind def\n");
        put_str(s, "/F1 { /F1n findfont "); put_tenths(s, size); put_str(s, " scalefont setfont } bind def\n");
        put_str(s, "/F2 { /F2n findfont "); put_tenths(s, size); put_str(s, " scalefont setfont } bind def\n");
        put_str(s, "/F3 { /F3n findfont "); put_tenths(s, size); put_str(s, " scalefont setfont } bind def\n");
        put_str(s, "/M { moveto } bind def /S { show } bind def\n"
                   "/U { moveto 0 rlineto 0.6 setlinewidth stroke } bind def\n");
        if (s->opt->sides != WP_PRN_SIMPLEX) {
            put_str(s, s->opt->sides == WP_PRN_DUPLEX_SHORT
                       ? "<< /Duplex true /Tumble true >> setpagedevice\n"
                       : "<< /Duplex true /Tumble false >> setpagedevice\n");
        }
        break;
    }
    default:
        break;
    }
}

static void drv_end_doc(prn_state *s)
{
    if (s->opt->driver == WP_PRN_PCL) put_str(s, "\033E");
    if (s->opt->driver == WP_PRN_POSTSCRIPT) put_str(s, "%%EOF\n");
}

/* At a page's start: the paper, when it changed from the last page's. */
static void drv_begin_page(prn_state *s)
{
    const wp_section *sec = s->sec;
    wp_bool changed = (wp_bool)(sec->page_width != s->sent_w || sec->page_height != s->sent_h);

    s->prow = 0;
    switch (s->opt->driver) {
    case WP_PRN_EPSON:
        if (changed) {                                           /* page length, in lines */
            u32 lines = twips_to(sec->page_height, s->lpi);
            u8 seq[3];
            seq[0] = 0x1B; seq[1] = 'C'; seq[2] = (u8)(lines > 127u ? 127u : lines);
            put(s, seq, 3);
        }
        break;
    case WP_PRN_PCL:
        if (changed) {
            int code = pcl_paper(sec->page_width, sec->page_height);
            if (code != 0) { put_str(s, "\033&l"); put_num(s, (long)code); put_str(s, "A"); }
            put_str(s, sec->page_width > sec->page_height ? "\033&l1O" : "\033&l0O");
            put_str(s, "\033&l0E");                                /* no top margin: ours */
        }
        break;
    case WP_PRN_POSTSCRIPT:
        put_str(s, "%%Page: "); put_num(s, (long)s->page_no);
        put_str(s, " "); put_num(s, (long)s->page_no); put_str(s, "\n");
        if (changed) {
            put_str(s, "<< /PageSize [");
            put_tenths(s, (long)sec->page_width / 2); put_str(s, " ");
            put_tenths(s, (long)sec->page_height / 2); put_str(s, "] >> setpagedevice\n");
        }
        break;
    default:
        break;
    }
    s->sent_w = sec->page_width;
    s->sent_h = sec->page_height;
}

static void drv_end_page(prn_state *s)
{
    static const u8 ff = 0x0C;

    if (s->opt->driver == WP_PRN_POSTSCRIPT) put_str(s, "showpage\n");
    else put(s, &ff, 1);
}

static void drv_blank(prn_state *s)
{
    if (s->opt->driver == WP_PRN_POSTSCRIPT) s->prow++;
    else put_eol(s);
}

/* A PostScript string's bytes. */
static void put_ps_text(prn_state *s, const u8 *t, u32 n)
{
    u32 i;
    put_str(s, "(");
    for (i = 0; i < n; i++) {
        u8 c = t[i];
        if (c == '(' || c == ')' || c == '\\') {
            u8 e[2]; e[0] = '\\'; e[1] = c; put(s, e, 2);
        } else if (c < 0x20 || c >= 0x7F) {
            u8 e[4];
            e[0] = '\\'; e[1] = (u8)('0' + (c >> 6)); e[2] = (u8)('0' + ((c >> 3) & 7));
            e[3] = (u8)('0' + (c & 7));
            put(s, e, 4);
        } else {
            put(s, &c, 1);
        }
    }
    put_str(s, ")");
}

/* One physical line: text with its style beside it, trailing spaces
   dropped. The text drivers write the left margin, the text with style
   codes where the style changes (back to none by the line's end), then
   the line end; PostScript moves to the line's place on the page and
   shows each run of one style in its Courier face. */
static void drv_line(prn_state *s, const u8 *text, const u8 *attr, u32 len)
{
    u32 i;

    while (len > 0 && text[len - 1] == ' ') len--;
    if (s->opt->driver == WP_PRN_POSTSCRIPT) {
        long cw = 720L / (long)s->cpi, lh = 720L / (long)s->lpi;
        long y = (long)s->sec->page_height / 2 - ((long)s->prow * lh + lh * 3 / 4);
        long x0 = (long)s->sec->margin_left / 2;
        u32 a = 0;
        while (a < len) {
            u32 b = a;
            while (b < len && attr[b] == attr[a]) b++;
            while (a < b && text[a] == ' ' && (attr[a] & PRN_A_UNDER) == 0) a++;
            if (a < b) {
                if (attr[a] & PRN_A_UNDER) {
                    put_tenths(s, (long)(b - a) * cw); put_str(s, " ");
                    put_tenths(s, x0 + (long)a * cw); put_str(s, " ");
                    put_tenths(s, y - 15); put_str(s, " U ");
                }
                put_str(s, attr[a] & PRN_A_BOLD ? (attr[a] & PRN_A_ITALIC ? "F3 " : "F1 ")
                                                : (attr[a] & PRN_A_ITALIC ? "F2 " : "F0 "));
                put_tenths(s, x0 + (long)a * cw); put_str(s, " ");
                put_tenths(s, y); put_str(s, " M ");
                put_ps_text(s, text + a, b - a);
                put_str(s, " S\n");
            }
            a = b;
        }
        s->prow++;
        return;
    }
    if (len > 0) {
        u32 pad = s->left_pad;
        /* PCL measures from its logical page, a quarter inch in. */
        if (s->opt->driver == WP_PRN_PCL) pad = pad > s->cpi / 4u ? pad - s->cpi / 4u : 0;
        put_spaces(s, pad);
        for (i = 0; i < len; i++) {
            if (attr[i] != s->cur_attr) style_codes(s, s->cur_attr, attr[i]);
            put(s, text + i, 1);
        }
        if (s->cur_attr != 0) style_codes(s, s->cur_attr, 0);
    }
    put_eol(s);
}

/* ---- Characters ------------------------------------------------------ */

/* The printer bytes for one code point: 0..3 of them into out. */
static u32 encode_cp(const prn_state *s, u32 cp, u8 *out)
{
    u16 page = s->opt->codepage;
    u8 b;

    if (cp == '\t') { out[0] = '\t'; return 1; }
    if (cp < 0x20u || cp == 0x7Fu) { out[0] = ' '; return 1; }
    if (cp < 0x7Fu) { out[0] = (u8)cp; return 1; }
    if (page != 0 && wp_codepage_encode(page, cp, &b) && b >= 0x80u
        && !(page == 1252u && b < 0xA0u)) {
        out[0] = b;
        return 1;
    }

    switch (cp) {
    case 0x00ADu:                                  /* soft hyphen */
        return 0;
    case 0x00A0u: case 0x2002u: case 0x2003u: case 0x2009u:
        out[0] = ' '; return 1;
    case 0x2018u: case 0x2019u: case 0x201Au: case 0x2032u:
        out[0] = '\''; return 1;
    case 0x201Cu: case 0x201Du: case 0x201Eu: case 0x2033u:
        out[0] = '"'; return 1;
    case 0x2010u: case 0x2011u: case 0x2012u: case 0x2013u:
    case 0x2014u: case 0x2015u: case 0x2212u:
        out[0] = '-'; return 1;
    case 0x2022u: case 0x2023u: case 0x25CFu:
        out[0] = '*'; return 1;
    case 0x2026u:
        out[0] = '.'; out[1] = '.'; out[2] = '.'; return 3;
    default:
        out[0] = '?'; return 1;
    }
}

/* text[0..len) as printer bytes into out, which holds 3 * len, each
   with the style `attr` beside it in out_attr. */
static u32 encode_text(const prn_state *s, const u8 *text, u32 len, u8 *out,
                       u8 *out_attr, u8 attr)
{
    u32 i = 0, n = 0, k0;

    while (i < len) {
        u32 cp;
        u32 k = utf8_decode(text + i, len - i, &cp);
        k0 = n;
        if (k == 0) { out[n++] = '?'; i++; }
        else { n += encode_cp(s, cp, out + n); i += k; }
        while (k0 < n) out_attr[k0++] = attr;
    }
    return n;
}

/* ---- Pages ----------------------------------------------------------- */

static void set_section(prn_state *s, const wp_section *sec)
{
    i32 text_w = sec->page_width - sec->margin_left - sec->margin_right;
    u32 lines = twips_to(sec->page_height, s->lpi);

    s->sec = sec;
    s->cols = twips_to(text_w, s->cpi);
    if (s->cols < 1) s->cols = 1;
    if (s->cols > PRN_MAX_COLS) s->cols = PRN_MAX_COLS;
    s->left_pad = twips_to(sec->margin_left, s->cpi);
    if (s->left_pad > PRN_MAX_COLS) s->left_pad = PRN_MAX_COLS;
    s->top = twips_to(sec->margin_top, s->lpi);
    s->bottom = twips_to(sec->margin_bottom, s->lpi);
    if (s->top + s->bottom + 1 > lines) {
        /* Margins that leave no room are kept to one text line. */
        s->body = 1;
    } else {
        s->body = lines - s->top - s->bottom;
    }
}

/* A header or footer for the current page, laid into s->hb across the
   column: '|' parts left, centre, right (one part centred, two left and
   right, a fourth and later joined to the third), '#' the page number. */
static void put_margin_line(prn_state *s, wp_span span)
{
    const u8 *src = wp_strpool_get(&s->doc->strings, span);
    u8 (*part)[PRN_MAX_COLS + 1] = s->part;
    u32 plen[3], nparts = 1, i, w = 0;
    u8 enc[4];
    char num[12];
    u32 nlen = 0, v = s->page_no;

    do { num[nlen++] = (char)('0' + v % 10u); v /= 10u; } while (v > 0 && nlen < 11);

    plen[0] = plen[1] = plen[2] = 0;
    i = 0;
    while (i < span.length) {
        u32 cp, k = utf8_decode(src + i, span.length - i, &cp);
        u32 e, j;
        if (k == 0) { cp = '?'; k = 1; }
        i += k;
        if (cp == '|' && nparts < 3) { nparts++; continue; }
        w = nparts - 1;
        if (cp == '#') {
            for (j = nlen; j > 0; j--) {
                if (plen[w] < PRN_MAX_COLS) part[w][plen[w]++] = (u8)num[j - 1];
            }
            continue;
        }
        e = encode_cp(s, cp == '\t' ? ' ' : cp, enc);
        for (j = 0; j < e; j++) {
            if (plen[w] < PRN_MAX_COLS) part[w][plen[w]++] = enc[j];
        }
    }

    memset(s->hb, ' ', s->cols);
    {
        /* Which slot each part fills: one part is the centre. */
        u32 slot_of[3];
        if (nparts == 1)      { slot_of[0] = 1; }
        else if (nparts == 2) { slot_of[0] = 0; slot_of[1] = 2; }
        else                  { slot_of[0] = 0; slot_of[1] = 1; slot_of[2] = 2; }
        for (i = 0; i < nparts; i++) {
            u32 n = plen[i] > s->cols ? s->cols : plen[i];
            u32 at = slot_of[i] == 0 ? 0
                   : slot_of[i] == 1 ? (s->cols - n) / 2u
                   : s->cols - n;
            memcpy(s->hb + at, part[i], n);
        }
    }
    drv_line(s, s->hb, s->za, s->cols);
}

static void blank_lines(prn_state *s, u32 n)
{
    while (n-- > 0) drv_blank(s);
}

static void begin_page(prn_state *s)
{
    u32 hrow = s->top / 2u;

    if (s->page_no == 0) drv_begin_doc(s);   /* with the first page: no pages, no bytes */
    s->page_no++;
    s->page_open = WP_TRUE;
    s->row = 0;
    s->pending = 0;
    drv_begin_page(s);
    if (s->sec->header.length > 0 && s->top > 0) {
        blank_lines(s, hrow);
        put_margin_line(s, s->sec->header);
        blank_lines(s, s->top - hrow - 1);
    } else {
        blank_lines(s, s->top);
    }
}

static void end_page(prn_state *s)
{
    if (!s->page_open) return;
    if (s->sec->footer.length > 0 && s->bottom > 0) {
        blank_lines(s, s->body - s->row + (s->bottom - 1) / 2u);
        put_margin_line(s, s->sec->footer);
    }
    drv_end_page(s);
    s->page_open = WP_FALSE;
    s->pending = 0;
    s->space_acc = 0;
}

/* Spacing owed before the next text line. At the top of a page it is
   dropped, as a word processor drops a paragraph's space before there. */
static void add_space(prn_state *s, i32 twips)
{
    if (twips <= 0) return;
    s->space_acc += twips;
    while (s->space_acc >= s->line_twips) {
        s->space_acc -= s->line_twips;
        s->pending++;
    }
}

static void emit_text_line(prn_state *s, const u8 *text, const u8 *attr, u32 len)
{
    if (s->page_open && s->row >= s->body) end_page(s);
    if (!s->page_open) begin_page(s);
    if (s->row > 0) {
        while (s->pending > 0 && s->row < s->body) {
            drv_blank(s);
            s->row++;
            s->pending--;
        }
        if (s->row >= s->body) {
            end_page(s);
            begin_page(s);
        }
    }
    s->pending = 0;
    drv_line(s, text, attr, len);
    s->row++;
}

/* ---- Paragraphs ------------------------------------------------------ */

/* Justifies lb[start..start+len) to `width` by widening its word gaps,
   leading spaces left alone. Returns the new length. */
static u32 justify_line(prn_state *s, u32 start, u32 len, u32 width)
{
    u8 *tmp = s->jb, *tmpa = s->ja;
    u8 *t = s->lb + start, *ta = s->la + start;
    u32 i, gaps = 0, extra, lead = 0, n = 0, g = 0;

    while (len > 0 && t[len - 1] == ' ') len--;
    if (len >= width) return len;
    while (lead < len && t[lead] == ' ') lead++;
    for (i = lead; i < len; i++) {
        if (t[i] == ' ' && t[i - 1] != ' ') gaps++;
    }
    if (gaps == 0) return len;
    extra = width - len;

    for (i = 0; i < len; i++) {
        tmpa[n] = ta[i];
        tmp[n++] = t[i];
        if (i >= lead && t[i] == ' ' && t[i - 1] != ' ') {
            /* This gap's share: the extra spread evenly, earlier gaps
               taking the remainder, in the gap's own style (an underline
               stays unbroken). */
            u32 share = extra / gaps + (g < extra % gaps ? 1u : 0u);
            while (share-- > 0) { tmpa[n] = ta[i]; tmp[n++] = ' '; }
            g++;
        }
    }
    memcpy(t, tmp, n);
    memcpy(ta, tmpa, n);
    return n;
}

static u32 tab_col(const prn_state *s, const wp_para_style *ps, u32 col)
{
    u32 i, step;

    if (ps != NULL) {
        for (i = 0; i < ps->tab_count; i++) {
            u32 at = twips_to(ps->tab_pos[i], s->cpi);
            if (at > col) return at;
        }
    }
    step = twips_to(WP_DEFAULT_TAB_TWIPS, s->cpi);
    if (step < 1) step = 1;
    return (col / step + 1u) * step;
}

/* One line ready in lb[0..len): aligned, then sent. */
static void finish_line(prn_state *s, const wp_para_style *ps, u32 start,
                        u32 len, u32 limit, wp_bool last, wp_bool has_tab)
{
    u32 align = ps != NULL ? ps->align : WP_ALIGN_LEFT;

    while (len > start && s->lb[len - 1] == ' ') len--;
    if (!has_tab && len > start && len < limit
        && (align == WP_ALIGN_CENTER || align == WP_ALIGN_RIGHT)) {
        u32 shift = align == WP_ALIGN_RIGHT ? limit - len : (limit - len) / 2u;
        memmove(s->lb + start + shift, s->lb + start, len - start);
        memmove(s->la + start + shift, s->la + start, len - start);
        memset(s->lb + start, ' ', shift);
        memset(s->la + start, 0, shift);
        len += shift;
    } else if (!has_tab && !last && align == WP_ALIGN_JUSTIFY && len > start) {
        len = start + justify_line(s, start, len - start, limit - start);
    }
    emit_text_line(s, s->lb, s->la, len);
}

static i32 line_extra_twips(const prn_state *s, const wp_para_style *ps)
{
    if (ps == NULL || ps->line_spacing == 0) return 0;
    if (ps->line_spacing > 0) {
        return (i32)(((long)ps->line_spacing - 240L) * s->line_twips / 240L);
    }
    return -ps->line_spacing > s->line_twips ? -ps->line_spacing - s->line_twips : 0;
}

/* text[0..len) is the paragraph in printer bytes, attr[] their styles. */
static void print_text(prn_state *s, const wp_para_style *ps, const u8 *text,
                       const u8 *attr, u32 len)
{
    i32 il = ps != NULL ? ps->indent_left : 0;
    i32 ir = ps != NULL ? ps->indent_right : 0;
    i32 ifirst = ps != NULL ? ps->indent_first : 0;
    u32 left = twips_to(il, s->cpi);
    u32 first = twips_to(il + ifirst, s->cpi);
    u32 right = twips_to(ir, s->cpi);
    u32 limit = s->cols > right ? s->cols - right : 1;
    i32 extra = line_extra_twips(s, ps);
    u32 i = 0, start, col, brk_src = 0, brk_col = 0;
    wp_bool have_brk = WP_FALSE, has_tab = WP_FALSE;

    if (left >= limit) left = limit - 1;
    if (first >= limit) first = limit - 1;
    start = first;
    memset(s->lb, ' ', start);
    memset(s->la, 0, start);
    col = start;

    if (len == 0) {
        emit_text_line(s, s->lb, s->la, 0);
        return;
    }

    while (i < len) {
        u8 c = text[i];

        if (c == ' ') {
            if (col < limit) { s->la[col] = attr[i]; s->lb[col++] = ' '; }
            i++;
            brk_src = i; brk_col = col; have_brk = WP_TRUE;
            continue;
        }
        if (c == '\t') {
            u32 to = tab_col(s, ps, col);
            if (to <= limit) {
                while (col < to) { s->la[col] = 0; s->lb[col++] = ' '; }
                has_tab = WP_TRUE;
                i++;
                brk_src = i; brk_col = col; have_brk = WP_TRUE;
                continue;
            }
            /* A tab past the line's end breaks it. */
            finish_line(s, ps, start, col, limit, WP_FALSE, has_tab);
            add_space(s, extra);
            i++;
        } else if (col < limit) {
            s->la[col] = attr[i];
            s->lb[col++] = c;
            i++;
            continue;
        } else if (have_brk && brk_col > start) {
            finish_line(s, ps, start, brk_col, limit, WP_FALSE, has_tab);
            add_space(s, extra);
            i = brk_src;
        } else {
            /* A word wider than the line is cut. */
            finish_line(s, ps, start, col, limit, WP_FALSE, has_tab);
            add_space(s, extra);
        }

        /* The next line, its leading spaces dropped. */
        while (i < len && text[i] == ' ') i++;
        start = left;
        memset(s->lb, ' ', start);
        memset(s->la, 0, start);
        col = start;
        have_brk = WP_FALSE;
        has_tab = WP_FALSE;
        if (i >= len) return;
    }
    finish_line(s, ps, start, col, limit, WP_TRUE, has_tab);
    add_space(s, extra);
}

static void print_paragraph(prn_state *s, const wp_paragraph *para, u8 *buf, u8 *abuf)
{
    const wp_para_style *ps = wp_document_pstyle_get(s->doc, para->style);
    u32 n = 0;
    u16 ri;

    if (ps != NULL && ps->page_break_before && s->page_open && s->row > 0) {
        end_page(s);
    }
    if (ps != NULL && s->page_open && s->row > 0) add_space(s, ps->space_before);

    if (para->kind == WP_PARA_IMAGE) {
        static const u8 pic[] = "[Picture]";
        memcpy(buf, pic, sizeof(pic) - 1);
        n = sizeof(pic) - 1;
        memset(abuf, 0, n);
    } else {
        for (ri = 0; ri < para->run_count; ri++) {
            const wp_run *run = &para->runs[ri];
            const wp_char_style *cs = wp_document_cstyle_get(s->doc, run->style);
            u8 a = 0;
            if (cs != NULL) {
                if (cs->flags & WP_STYLE_BOLD) a |= PRN_A_BOLD;
                if (cs->flags & WP_STYLE_ITALIC) a |= PRN_A_ITALIC;
                if (cs->flags & WP_STYLE_UNDERLINE) a |= PRN_A_UNDER;
            }
            n += encode_text(s, wp_strpool_get(&s->doc->strings, run->text),
                             run->text.length, buf + n, abuf + n, a);
        }
    }
    print_text(s, ps, buf, abuf, n);
    if (ps != NULL) add_space(s, ps->space_after);
}

wp_status format_prn_export(const wp_document *doc, const wp_prn_options *opt_in,
                            wp_prn_write_fn write, void *user, u32 *out_pages)
{
    prn_state *s;
    u8 *buf = NULL, *abuf = NULL;
    wp_prn_options optc;
    const wp_prn_options *opt = &optc;
    u32 cap = 16, si, pi;
    wp_status st;

    if (out_pages != NULL) *out_pages = 0;
    if (doc == NULL || opt_in == NULL || write == NULL) return WP_ERR;
    optc = *opt_in;
    if (optc.driver == WP_PRN_POSTSCRIPT) {
        optc.codepage = 1252;   /* the prolog's ISO Latin-1 encoding */
        optc.crlf = WP_FALSE;
    }

    /* The paragraph buffer: 3 printer bytes a source byte at most. */
    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            u32 n = 0;
            u16 ri;
            for (ri = 0; ri < para->run_count; ri++) n += para->runs[ri].text.length;
            if (n * 3u + 16u > cap) cap = n * 3u + 16u;
        }
    }

    /* The state is a heap block: its line buffers are past what the
       Amiga's 4 KB stack can spare. */
    s = (prn_state *)mem_alloc((u32)sizeof(prn_state));
    if (s == NULL) return WP_NOMEM;
    buf = (u8 *)mem_alloc(cap);
    abuf = (u8 *)mem_alloc(cap);
    if (buf == NULL || abuf == NULL) {
        if (buf != NULL) mem_free(buf);
        if (abuf != NULL) mem_free(abuf);
        mem_free(s);
        return WP_NOMEM;
    }

    memset(s, 0, sizeof(*s));
    s->doc = doc;
    s->opt = opt;
    s->write = write;
    s->user = user;
    s->st = WP_OK;
    s->cpi = opt->cpi != 0 ? opt->cpi : 10u;
    s->lpi = opt->lpi != 0 ? opt->lpi : 6u;
    s->line_twips = (i32)(1440u / s->lpi);
    s->sent_w = s->sent_h = -1;

    for (si = 0; si < doc->section_count && s->st == WP_OK; si++) {
        const wp_section *sec = &doc->sections[si];
        if (sec->para_count == 0) continue;
        end_page(s);                /* a section begins a page */
        set_section(s, sec);
        for (pi = 0; pi < sec->para_count && s->st == WP_OK; pi++) {
            print_paragraph(s, &sec->paras[pi], buf, abuf);
        }
    }
    end_page(s);
    if (s->page_no > 0) drv_end_doc(s);

    st = s->st;
    if (out_pages != NULL && st == WP_OK) *out_pages = s->page_no;
    mem_free(abuf);
    mem_free(buf);
    mem_free(s);
    return st;
}
