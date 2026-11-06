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
    u32     page_no;         /* pages begun so far, the document through */
    u32     pending;         /* blank lines owed to spacing */
    i32     space_acc;       /* spacing in twips not yet a whole line */

    /* Working lines, here rather than on the stack (the Amiga's is 4 KB). */
    u8 lb[PRN_MAX_COLS + 1];       /* one laid-out line */
    u8 hb[PRN_MAX_COLS + 1];       /* a header or footer, laid out */
    u8 jb[PRN_MAX_COLS + 1];       /* a line being justified */
    u8 part[3][PRN_MAX_COLS + 1];  /* a header or footer's '|' parts */
} prn_state;

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

static void put_eol(prn_state *s)
{
    static const u8 crlf[2] = { '\r', '\n' };
    if (s->opt->crlf) put(s, crlf, 2);
    else put(s, crlf + 1, 1);
}

/* One physical line: the left margin, then text with its trailing spaces
   dropped, then the line end. */
static void put_line(prn_state *s, const u8 *text, u32 len)
{
    while (len > 0 && text[len - 1] == ' ') len--;
    if (len > 0) {
        put_spaces(s, s->left_pad);
        put(s, text, len);
    }
    put_eol(s);
}

static u32 twips_to(i32 twips, u32 per_inch)
{
    if (twips <= 0) return 0;
    return (u32)(((u32)twips * per_inch + 720u) / 1440u);
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

/* text[0..len) as printer bytes into out, which holds 3 * len. */
static u32 encode_text(const prn_state *s, const u8 *text, u32 len, u8 *out)
{
    u32 i = 0, n = 0;

    while (i < len) {
        u32 cp;
        u32 k = utf8_decode(text + i, len - i, &cp);
        if (k == 0) { out[n++] = '?'; i++; continue; }
        n += encode_cp(s, cp, out + n);
        i += k;
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
    put_line(s, s->hb, s->cols);
}

static void blank_lines(prn_state *s, u32 n)
{
    while (n-- > 0) put_eol(s);
}

static void begin_page(prn_state *s)
{
    u32 hrow = s->top / 2u;

    s->page_no++;
    s->page_open = WP_TRUE;
    s->row = 0;
    s->pending = 0;
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
    static const u8 ff = 0x0C;

    if (!s->page_open) return;
    if (s->sec->footer.length > 0 && s->bottom > 0) {
        blank_lines(s, s->body - s->row + (s->bottom - 1) / 2u);
        put_margin_line(s, s->sec->footer);
    }
    put(s, &ff, 1);
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

static void emit_text_line(prn_state *s, const u8 *text, u32 len)
{
    if (s->page_open && s->row >= s->body) end_page(s);
    if (!s->page_open) begin_page(s);
    if (s->row > 0) {
        while (s->pending > 0 && s->row < s->body) {
            put_eol(s);
            s->row++;
            s->pending--;
        }
        if (s->row >= s->body) {
            end_page(s);
            begin_page(s);
        }
    }
    s->pending = 0;
    put_line(s, text, len);
    s->row++;
}

/* ---- Paragraphs ------------------------------------------------------ */

/* Justifies lb[start..start+len) to `width` by widening its word gaps,
   leading spaces left alone. Returns the new length. */
static u32 justify_line(prn_state *s, u32 start, u32 len, u32 width)
{
    u8 *tmp = s->jb;
    u8 *t = s->lb + start;
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
        tmp[n++] = t[i];
        if (i >= lead && t[i] == ' ' && t[i - 1] != ' ') {
            /* This gap's share: the extra spread evenly, earlier gaps
               taking the remainder. */
            u32 share = extra / gaps + (g < extra % gaps ? 1u : 0u);
            while (share-- > 0) tmp[n++] = ' ';
            g++;
        }
    }
    memcpy(t, tmp, n);
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
        memset(s->lb + start, ' ', shift);
        len += shift;
    } else if (!has_tab && !last && align == WP_ALIGN_JUSTIFY && len > start) {
        len = start + justify_line(s, start, len - start, limit - start);
    }
    emit_text_line(s, s->lb, len);
}

static i32 line_extra_twips(const prn_state *s, const wp_para_style *ps)
{
    if (ps == NULL || ps->line_spacing == 0) return 0;
    if (ps->line_spacing > 0) {
        return (i32)(((long)ps->line_spacing - 240L) * s->line_twips / 240L);
    }
    return -ps->line_spacing > s->line_twips ? -ps->line_spacing - s->line_twips : 0;
}

/* text[0..len) is the paragraph in printer bytes. */
static void print_text(prn_state *s, const wp_para_style *ps, const u8 *text, u32 len)
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
    col = start;

    if (len == 0) {
        emit_text_line(s, s->lb, 0);
        return;
    }

    while (i < len) {
        u8 c = text[i];

        if (c == ' ') {
            if (col < limit) s->lb[col++] = ' ';
            i++;
            brk_src = i; brk_col = col; have_brk = WP_TRUE;
            continue;
        }
        if (c == '\t') {
            u32 to = tab_col(s, ps, col);
            if (to <= limit) {
                while (col < to) s->lb[col++] = ' ';
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
        col = start;
        have_brk = WP_FALSE;
        has_tab = WP_FALSE;
        if (i >= len) return;
    }
    finish_line(s, ps, start, col, limit, WP_TRUE, has_tab);
    add_space(s, extra);
}

static void print_paragraph(prn_state *s, const wp_paragraph *para, u8 *buf)
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
    } else {
        for (ri = 0; ri < para->run_count; ri++) {
            const wp_run *run = &para->runs[ri];
            n += encode_text(s, wp_strpool_get(&s->doc->strings, run->text),
                             run->text.length, buf + n);
        }
    }
    print_text(s, ps, buf, n);
    if (ps != NULL) add_space(s, ps->space_after);
}

wp_status format_prn_export(const wp_document *doc, const wp_prn_options *opt,
                            wp_prn_write_fn write, void *user, u32 *out_pages)
{
    prn_state *s;
    u8 *buf = NULL;
    u32 cap = 16, si, pi;
    wp_status st;

    if (out_pages != NULL) *out_pages = 0;
    if (doc == NULL || opt == NULL || write == NULL) return WP_ERR;

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
    if (buf == NULL) { mem_free(s); return WP_NOMEM; }

    memset(s, 0, sizeof(*s));
    s->doc = doc;
    s->opt = opt;
    s->write = write;
    s->user = user;
    s->st = WP_OK;
    s->cpi = opt->cpi != 0 ? opt->cpi : 10u;
    s->lpi = opt->lpi != 0 ? opt->lpi : 6u;
    s->line_twips = (i32)(1440u / s->lpi);

    for (si = 0; si < doc->section_count && s->st == WP_OK; si++) {
        const wp_section *sec = &doc->sections[si];
        if (sec->para_count == 0) continue;
        end_page(s);                /* a section begins a page */
        set_section(s, sec);
        for (pi = 0; pi < sec->para_count && s->st == WP_OK; pi++) {
            print_paragraph(s, &sec->paras[pi], buf);
        }
    }
    end_page(s);

    st = s->st;
    if (out_pages != NULL && st == WP_OK) *out_pages = s->page_no;
    mem_free(buf);
    mem_free(s);
    return st;
}
