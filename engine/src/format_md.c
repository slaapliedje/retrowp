/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_md.h"
#include "platform.h"   /* mem_alloc/mem_realloc for the paragraph buffers */

#define MD_H1_TWIPS 480
#define MD_H2_TWIPS 360
#define MD_H3_TWIPS 280

/* Emphasis markers resolved per paragraph; a paragraph with more runs of
   * or _ than this keeps the rest as text. */
#define MD_MAX_DELIMS 64

static const u8 md_nbsp[6] = { '&', 'n', 'b', 's', 'p', ';' };

static wp_bool is_ws(u8 c) { return (wp_bool)(c == ' ' || c == '\t'); }
static wp_bool is_alnum(u8 c)
{
    return (wp_bool)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}
static wp_bool is_punct(u8 c)
{
    return (wp_bool)(c >= 0x21 && c <= 0x7E && !is_alnum(c));
}

/* Three backticks after up to three spaces: a fence line on the way in,
   a fence paragraph on the way out. */
static wp_bool is_fence(const u8 *s, u32 n)
{
    u32 i = 0;
    while (i < n && i < 3 && s[i] == ' ') i++;
    return (wp_bool)(i + 3 <= n && s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`');
}

static u16 style_of_len(u32 n)
{
    return (n == 1) ? (u16)WP_STYLE_ITALIC
         : (n == 2) ? (u16)WP_STYLE_BOLD
                    : (u16)(WP_STYLE_BOLD | WP_STYLE_ITALIC);
}

/* ------------------------------------------------------------------ */
/* Import                                                               */
/* ------------------------------------------------------------------ */

typedef struct { u8 *p; u32 len, cap; } md_buf;

static wp_status buf_put(md_buf *b, const u8 *s, u32 n)
{
    if (b->len + n > b->cap) {
        u32 new_cap = (b->cap == 0) ? 256 : b->cap;
        void *mem;

        while (new_cap < b->len + n) new_cap *= 2;
        mem = mem_realloc(b->p, b->cap, new_cap);
        if (mem == NULL) return WP_NOMEM;
        b->p = (u8 *)mem;
        b->cap = new_cap;
    }
    if (n > 0) memcpy(b->p + b->len, s, n);
    b->len += n;
    return WP_OK;
}

static void buf_free(md_buf *b)
{
    if (b->p != NULL) mem_free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

/* One run of * or _ in a paragraph, and what it turned out to mean. */
typedef struct {
    u32 pos;
    u8  len, ch, can_open, can_close;
    u16 closes, opens;   /* style bits this run closes, then opens */
    u8  n_text;          /* markers that are plain text, written between */
} md_delim;

typedef struct { u8 delim; u8 len; u16 bits; } md_open;

typedef struct {
    wp_document *doc;
    wp_section  *sec;
    md_buf raw;         /* the open paragraph's Markdown, lines joined */
    md_buf txt;         /* the run being gathered, markup removed */
    wp_bool para_open;
    wp_bool literal;    /* a fence or a line inside one: no markup */
    u32   para_idx;
    u16   flags;        /* emphasis in force for txt */
    u16   base_flags;   /* a heading's bold, under whatever emphasis adds */
    i32   size_twips;   /* a heading's size, or 0 for the default */
    wp_bool in_code;
    wp_status error;
    md_delim delims[MD_MAX_DELIMS];
    md_open  stack[MD_MAX_DELIMS * 2];
} md_import;

/* Makes txt a run of the open paragraph in the style in force. */
static void flush_run(md_import *st)
{
    wp_span span;
    wp_cstyle cstyle;
    wp_paragraph *para;
    u16 run_idx;

    if (st->error != WP_OK || st->txt.len == 0 || !st->para_open) { st->txt.len = 0; return; }

    if (wp_strpool_append(&st->doc->strings, st->txt.p, st->txt.len, &span) != WP_OK) {
        st->error = WP_NOMEM;
        return;
    }
    st->txt.len = 0;

    if ((st->flags | st->base_flags) == 0 && st->size_twips == 0) {
        cstyle = st->doc->default_cstyle;
    } else {
        const wp_char_style *def = wp_document_cstyle_get(st->doc, st->doc->default_cstyle);
        wp_char_style cs;

        memset(&cs, 0, sizeof(cs));
        cs.flags = (u16)(st->flags | st->base_flags);
        cs.size_twips = (st->size_twips != 0) ? st->size_twips : (def != NULL ? def->size_twips : 240);
        if (def != NULL) {
            cs.color_r = def->color_r;
            cs.color_g = def->color_g;
            cs.color_b = def->color_b;
        }
        if (wp_document_cstyle_find_or_add(st->doc, &cs, &cstyle) != WP_OK) {
            st->error = WP_NOMEM;
            return;
        }
    }
    para = wp_section_paragraph_get(st->sec, st->para_idx);
    if (wp_paragraph_run_add(para, span, cstyle, &run_idx) != WP_OK) st->error = WP_NOMEM;
}

static void put_byte(md_import *st, u8 c)
{
    if (st->error == WP_OK && buf_put(&st->txt, &c, 1) != WP_OK) st->error = WP_NOMEM;
}

static void put_bytes(md_import *st, const u8 *s, u32 n)
{
    if (st->error == WP_OK && buf_put(&st->txt, s, n) != WP_OK) st->error = WP_NOMEM;
}

static void set_flags(md_import *st, u16 flags)
{
    if (flags == st->flags) return;
    flush_run(st);
    st->flags = flags;
}

/* Pass one: find every run of * or _ outside escapes and code spans, and
   note whether a reader would let it open or close emphasis. A run opens
   when text follows it and closes when text precedes it; _ also wants a
   word edge on the outside, so snake_case stays snake_case. */
static u32 scan_delims(const u8 *s, u32 n, md_delim *dl)
{
    u32 i = 0, cnt = 0;

    while (i < n) {
        u8 c = s[i];

        if (c == '\\' && i + 1 < n && is_punct(s[i + 1])) { i += 2; continue; }
        if (c == '`') {
            u32 j = i + 1;
            while (j < n && s[j] != '`') j++;
            i = (j < n) ? j + 1 : i + 1;
            continue;
        }
        if (c == '*' || c == '_') {
            u32 run = 1;
            while (i + run < n && s[i + run] == c) run++;
            if (cnt < MD_MAX_DELIMS && run < 256) {
                md_delim *d = &dl[cnt++];

                memset(d, 0, sizeof(*d));
                d->pos = i;
                d->len = (u8)run;
                d->ch = c;
                d->can_open = (u8)(i + run < n && !is_ws(s[i + run]));
                d->can_close = (u8)(i > 0 && !is_ws(s[i - 1]));
                if (c == '_') {
                    if (i > 0 && is_alnum(s[i - 1])) d->can_open = 0;
                    if (i + run < n && is_alnum(s[i + run])) d->can_close = 0;
                }
            }
            i += run;
            continue;
        }
        i++;
    }
    return cnt;
}

/* Pass two: match closers to the nearest openers of the same character,
   innermost first; one marker is italic, two bold, three both. A run that
   closes and has markers left opens with them, so **bold***em* reads as a
   reader shows it. Openers never closed were text all along. */
static void resolve_delims(md_delim *dl, u32 count, md_open *stack)
{
    u32 sp = 0, k;
    u16 flags = 0;

    for (k = 0; k < count; k++) {
        md_delim *d = &dl[k];
        u32 remaining = d->len;

        if (d->can_close) {
            while (sp > 0 && remaining > 0) {
                md_open *top = &stack[sp - 1];

                if (dl[top->delim].ch != d->ch || top->len > remaining) break;
                d->closes = (u16)(d->closes | top->bits);
                flags = (u16)(flags & ~top->bits);
                remaining -= top->len;
                sp--;
            }
        }
        if (d->can_open && remaining > 0) {
            u32 o = (remaining > 3) ? 3 : remaining;
            u16 bits = style_of_len(o);

            if ((bits & flags) == 0) {
                if (o == 3) {
                    /* Bold outside, italic inside, so a lone closer can peel
                       the italic off. */
                    stack[sp].delim = (u8)k; stack[sp].len = 2; stack[sp].bits = (u16)WP_STYLE_BOLD;   sp++;
                    stack[sp].delim = (u8)k; stack[sp].len = 1; stack[sp].bits = (u16)WP_STYLE_ITALIC; sp++;
                } else {
                    stack[sp].delim = (u8)k; stack[sp].len = (u8)o; stack[sp].bits = bits; sp++;
                }
                d->opens = bits;
                flags = (u16)(flags | bits);
                remaining -= o;
            }
        }
        d->n_text = (u8)remaining;
    }
    while (sp > 0) {
        md_open *e = &stack[--sp];

        dl[e->delim].opens = (u16)(dl[e->delim].opens & ~e->bits);
        dl[e->delim].n_text = (u8)(dl[e->delim].n_text + e->len);
    }
}

/* The open paragraph's Markdown becomes its runs. */
static void emit_inline(md_import *st, const u8 *s, u32 n)
{
    md_delim *dl = st->delims;
    u32 count = scan_delims(s, n, dl);
    u32 di = 0, i = 0;

    resolve_delims(dl, count, st->stack);
    st->flags = 0;

    while (i < n && st->error == WP_OK) {
        u8 c = s[i];

        if (di < count && dl[di].pos == i) {
            md_delim *d = &dl[di++];
            u8 k;

            if (d->closes) set_flags(st, (u16)(st->flags & ~d->closes));
            for (k = 0; k < d->n_text; k++) put_byte(st, d->ch);
            if (d->opens) set_flags(st, (u16)(st->flags | d->opens));
            i += d->len;
            continue;
        }
        if (c == '\\' && i + 1 < n && is_punct(s[i + 1])) {
            put_byte(st, s[i + 1]);
            i += 2;
            continue;
        }
        if (c == '`') {
            /* A code span is kept whole, backticks and all: the model has
               no code style, and the text is what matters. */
            u32 j = i + 1;
            while (j < n && s[j] != '`') j++;
            if (j < n) {
                put_bytes(st, s + i, j - i + 1);
                i = j + 1;
            } else {
                put_byte(st, c);
                i++;
            }
            continue;
        }
        put_byte(st, c);
        i++;
    }
    flush_run(st);
}

static void open_paragraph(md_import *st, u16 base_flags, i32 size_twips, wp_bool literal)
{
    wp_paragraph *para;

    if (st->error != WP_OK) return;
    if (wp_section_paragraph_add(st->sec, &st->para_idx) != WP_OK) { st->error = WP_NOMEM; return; }
    para = wp_section_paragraph_get(st->sec, st->para_idx);
    wp_paragraph_pstyle_set(para, st->doc->default_pstyle);
    st->para_open = WP_TRUE;
    st->literal = literal;
    st->raw.len = 0;
    st->txt.len = 0;
    st->flags = 0;
    st->base_flags = base_flags;
    st->size_twips = size_twips;
}

static void append_raw(md_import *st, const u8 *s, u32 n)
{
    if (st->error == WP_OK && buf_put(&st->raw, s, n) != WP_OK) st->error = WP_NOMEM;
}

static void close_paragraph(md_import *st)
{
    if (!st->para_open || st->error != WP_OK) { st->para_open = WP_FALSE; return; }
    if (st->literal) {
        put_bytes(st, st->raw.p, st->raw.len);
        flush_run(st);
    } else if (st->raw.len == sizeof(md_nbsp) && memcmp(st->raw.p, md_nbsp, sizeof(md_nbsp)) == 0) {
        /* The blank-line idiom: a paragraph with nothing in it. */
    } else {
        emit_inline(st, st->raw.p, st->raw.len);
    }
    st->para_open = WP_FALSE;
}

static void add_literal_paragraph(md_import *st, const u8 *line, u32 n)
{
    close_paragraph(st);
    open_paragraph(st, 0, 0, WP_TRUE);
    append_raw(st, line, n);
    close_paragraph(st);
}

/* The level of an ATX heading, 0 if the line is not one; *text_start is
   where its text begins. */
static u32 heading_level(const u8 *line, u32 n, u32 *text_start)
{
    u32 level = 0, i;

    while (level < n && line[level] == '#') level++;
    if (level == 0 || level > 6) return 0;
    if (level < n && !is_ws(line[level])) return 0;
    i = level;
    while (i < n && is_ws(line[i])) i++;
    *text_start = i;
    return level;
}

/* Lines that begin a block of their own rather than continuing the
   paragraph above: list items, quotes, rules. Their text is kept as is. */
static wp_bool starts_block(const u8 *line, u32 n)
{
    u32 i = 0;

    if (n == 0) return WP_FALSE;
    if ((line[0] == '-' || line[0] == '*' || line[0] == '+') && n >= 2 && is_ws(line[1])) return WP_TRUE;
    if (line[0] == '>') return WP_TRUE;
    while (i < n && line[i] >= '0' && line[i] <= '9') i++;
    if (i > 0 && i + 1 < n && (line[i] == '.' || line[i] == ')') && is_ws(line[i + 1])) return WP_TRUE;
    if (n >= 3 && (line[0] == '-' || line[0] == '*' || line[0] == '_')) {
        u32 marks = 0;
        for (i = 0; i < n; i++) {
            if (line[i] == line[0]) marks++;
            else if (!is_ws(line[i])) break;
        }
        if (i == n && marks >= 3) return WP_TRUE;
    }
    return WP_FALSE;
}

static void import_line(md_import *st, const u8 *line, u32 n)
{
    u32 lead = 0, end = n;
    const u8 *body;
    u32 bn;

    if (st->error != WP_OK) return;

    if (st->in_code) {
        add_literal_paragraph(st, line, n);
        if (is_fence(line, n)) st->in_code = WP_FALSE;
        return;
    }

    while (lead < n && is_ws(line[lead])) lead++;
    if (lead == n) {                 /* blank: the paragraph ends here */
        close_paragraph(st);
        return;
    }
    while (end > lead && is_ws(line[end - 1])) end--;   /* trailing blanks, a hard break's two spaces */
    body = line + lead;
    bn = end - lead;

    if (lead <= 3) {
        u32 text_start = 0;
        u32 level;

        if (is_fence(body, bn)) {
            add_literal_paragraph(st, line, n);
            st->in_code = WP_TRUE;
            return;
        }
        level = heading_level(body, bn, &text_start);
        if (level > 0) {
            i32 size = (level == 1) ? MD_H1_TWIPS : (level == 2) ? MD_H2_TWIPS
                     : (level == 3) ? MD_H3_TWIPS : 0;
            u32 j = bn;

            /* Closing #s are decoration when a space precedes them: "## Title ##" */
            while (j > text_start && body[j - 1] == '#') j--;
            if (j == text_start || is_ws(body[j - 1])) {
                bn = j;
                while (bn > text_start && is_ws(body[bn - 1])) bn--;
            }
            close_paragraph(st);
            open_paragraph(st, (u16)WP_STYLE_BOLD, size, WP_FALSE);
            append_raw(st, body + text_start, bn - text_start);
            close_paragraph(st);
            return;
        }
        if (starts_block(body, bn)) {
            close_paragraph(st);
            open_paragraph(st, 0, 0, WP_FALSE);
            append_raw(st, line, end);      /* indentation kept: "  - sub" */
            return;                         /* stays open: a plain line continues it */
        }
    }

    if (st->para_open) {                    /* a wrapped line continues its paragraph */
        append_raw(st, (const u8 *)" ", 1);
        append_raw(st, body, bn);
    } else {
        open_paragraph(st, 0, 0, WP_FALSE);
        append_raw(st, line, end);
    }
}

wp_status format_md_import(const u8 *bytes, u32 len, wp_document *doc)
{
    md_import *st;
    u32 sec_idx;
    u32 i, line_start;
    wp_status rc;

    rc = wp_document_init(doc);
    if (rc != WP_OK) return rc;
    rc = wp_document_section_add(doc, &sec_idx);
    if (rc != WP_OK) { wp_document_free(doc); return rc; }

    /* On the heap: the delimiter tables would not fit a 4KB target stack. */
    st = (md_import *)mem_alloc(sizeof(*st));
    if (st == NULL) { wp_document_free(doc); return WP_NOMEM; }
    memset(st, 0, sizeof(*st));
    st->error = WP_OK;
    st->doc = doc;
    st->sec = wp_document_section_get(doc, sec_idx);

    line_start = 0;
    for (i = 0; i <= len && st->error == WP_OK; i++) {
        if (i == len || bytes[i] == '\n') {
            u32 line_end = i;

            if (i == len && line_start == len) break;   /* no final unterminated line */
            if (line_end > line_start && bytes[line_end - 1] == '\r') line_end--;
            import_line(st, bytes + line_start, line_end - line_start);
            line_start = i + 1;
        }
    }
    close_paragraph(st);

    rc = st->error;
    buf_free(&st->raw);
    buf_free(&st->txt);
    mem_free(st);
    if (rc != WP_OK) wp_document_free(doc);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Export                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    u8 *out;
    u32 cap;
    u32 pos;        /* bytes needed so far; written only while they fit */
} md_w;

static void w_bytes(md_w *w, const u8 *s, u32 n)
{
    if (w->out != NULL && w->pos + n <= w->cap) memcpy(w->out + w->pos, s, n);
    w->pos += n;
}

static void w_str(md_w *w, const char *s)
{
    w_bytes(w, (const u8 *)s, (u32)strlen(s));
}

static void w_byte(md_w *w, u8 c)
{
    w_bytes(w, &c, 1);
}

/* The markers that take the text from `cur` to `want`. Bold stays
   outside italic, so italic is closed first and, when bold changes
   underneath it, reopened after. */
static void w_transition(md_w *w, u16 cur, u16 want)
{
    const u16 b = (u16)WP_STYLE_BOLD, i = (u16)WP_STYLE_ITALIC;

    if (cur == want) return;
    if ((cur & i) && (!(want & i) || (cur & b) != (want & b))) {
        w_str(w, "*");
        cur = (u16)(cur & ~i);
    }
    if ((cur & b) && !(want & b)) {
        w_str(w, "**");
        cur = (u16)(cur & ~b);
    }
    if ((want & b) && !(cur & b)) w_str(w, "**");
    if ((want & i) && !(cur & i)) w_str(w, "*");
}

static u16 run_emphasis(const wp_document *doc, const wp_run *run, wp_bool heading)
{
    const wp_char_style *cs = wp_document_cstyle_get(doc, run->style);
    u16 want = (cs != NULL) ? (u16)(cs->flags & (WP_STYLE_BOLD | WP_STYLE_ITALIC)) : 0;

    if (heading) want = (u16)(want & ~WP_STYLE_BOLD);   /* the heading is the bold */
    return want;
}

/* Heading level of a paragraph: every run bold, all one size, 14 pt up. */
static u32 para_heading_level(const wp_document *doc, const wp_paragraph *para)
{
    i32 size = 0;
    u16 ri;

    if (para->run_count == 0) return 0;
    for (ri = 0; ri < para->run_count; ri++) {
        const wp_char_style *cs = wp_document_cstyle_get(doc, para->runs[ri].style);

        if (cs == NULL || (cs->flags & WP_STYLE_BOLD) == 0) return 0;
        if (ri == 0) size = cs->size_twips;
        else if (cs->size_twips != size) return 0;
    }
    if (size >= MD_H1_TWIPS) return 1;
    if (size >= MD_H2_TWIPS) return 2;
    if (size >= MD_H3_TWIPS) return 3;
    return 0;
}

/* The first few bytes of a paragraph's text, across runs. */
static u32 para_prefix(const wp_document *doc, const wp_paragraph *para, u8 *buf, u32 cap)
{
    u32 got = 0;
    u16 ri;

    for (ri = 0; ri < para->run_count && got < cap; ri++) {
        const u8 *s = wp_strpool_get(&doc->strings, para->runs[ri].text);
        u32 n = para->runs[ri].text.length;
        u32 take = (n < cap - got) ? n : cap - got;

        memcpy(buf + got, s, take);
        got += take;
    }
    return got;
}

static void w_para_verbatim(md_w *w, const wp_document *doc, const wp_paragraph *para)
{
    u16 ri;

    for (ri = 0; ri < para->run_count; ri++) {
        w_bytes(w, wp_strpool_get(&doc->strings, para->runs[ri].text), para->runs[ri].text.length);
    }
}

/* Emphasis of the next non-blank byte at or after (ri, off); -1 if none. */
static i32 next_word_emphasis(const wp_document *doc, const wp_paragraph *para,
                              u16 ri, u32 off, wp_bool heading)
{
    for (; ri < para->run_count; ri++, off = 0) {
        const u8 *s = wp_strpool_get(&doc->strings, para->runs[ri].text);
        u32 n = para->runs[ri].text.length;

        for (; off < n; off++) {
            if (!is_ws(s[off])) return (i32)run_emphasis(doc, &para->runs[ri], heading);
        }
    }
    return -1;
}

/* One word (no blanks), escaped so it reads back as the same text: \ and `
   always; * unless the word is nothing but stars (which no reader takes
   as emphasis); _ when a reader could take it as emphasis; and, as the
   paragraph's first word, a leading # or the blank-line idiom. */
static void w_word(md_w *w, const u8 *s, u32 n, wp_bool first_word)
{
    u32 k;
    wp_bool all_stars = WP_TRUE;

    for (k = 0; k < n; k++) if (s[k] != '*') { all_stars = WP_FALSE; break; }

    if (first_word && n > 0 && s[0] == '#') w_byte(w, '\\');
    if (first_word && n == sizeof(md_nbsp) && memcmp(s, md_nbsp, n) == 0) w_byte(w, '\\');

    for (k = 0; k < n; k++) {
        u8 c = s[k];

        if (c == '\\' || c == '`') {
            w_byte(w, '\\');
        } else if (c == '*') {
            if (!all_stars) w_byte(w, '\\');
        } else if (c == '_') {
            wp_bool prev_alnum = (wp_bool)(k > 0 && is_alnum(s[k - 1]));
            wp_bool next_alnum = (wp_bool)(k + 1 < n && is_alnum(s[k + 1]));
            wp_bool can_open = (wp_bool)(k + 1 < n && !prev_alnum);
            wp_bool can_close = (wp_bool)(k > 0 && !next_alnum);

            if (can_open || can_close) w_byte(w, '\\');
        }
        w_byte(w, c);
    }
}

/* A paragraph's runs as Markdown. Styles open and close at word edges
   so a reader honours them: blanks between words of one style stay
   inside the markers, blanks at a style's edge go outside. */
static void export_paragraph(md_w *w, const wp_document *doc, const wp_paragraph *para)
{
    u32 level = para_heading_level(doc, para);
    wp_bool heading = (wp_bool)(level > 0);
    u16 cur = 0;
    u16 ri;
    wp_bool first_word = WP_TRUE;

    if (level == 1) w_str(w, "# ");
    else if (level == 2) w_str(w, "## ");
    else if (level == 3) w_str(w, "### ");

    for (ri = 0; ri < para->run_count; ri++) {
        const wp_run *run = &para->runs[ri];
        const u8 *s = wp_strpool_get(&doc->strings, run->text);
        u32 n = run->text.length;
        u16 want = run_emphasis(doc, run, heading);
        u32 i = 0;

        while (i < n) {
            u32 j = i;

            if (is_ws(s[i])) {
                i32 next;

                while (j < n && is_ws(s[j])) j++;
                next = (j < n) ? (i32)want : next_word_emphasis(doc, para, (u16)(ri + 1), 0, heading);
                if (cur != 0 && next != (i32)cur) {
                    w_transition(w, cur, 0);
                    cur = 0;
                }
                w_bytes(w, s + i, j - i);
            } else {
                while (j < n && !is_ws(s[j])) j++;
                if (cur != want) {
                    w_transition(w, cur, want);
                    cur = want;
                }
                w_word(w, s + i, j - i, first_word);
                first_word = WP_FALSE;
            }
            i = j;
        }
    }
    w_transition(w, cur, 0);
}

static wp_bool para_is_empty(const wp_paragraph *para)
{
    u16 ri;

    for (ri = 0; ri < para->run_count; ri++) {
        if (para->runs[ri].text.length > 0) return WP_FALSE;
    }
    return WP_TRUE;
}

wp_status format_md_export(const wp_document *doc, u8 *out, u32 out_cap, u32 *out_len)
{
    md_w w;
    u32 si, pi;
    wp_bool first = WP_TRUE;
    wp_bool in_code = WP_FALSE;

    w.out = out;
    w.cap = out_cap;
    w.pos = 0;

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];

        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            u8 head[6];
            u32 head_len;

            if (!first) w_str(&w, in_code ? "\n" : "\n\n");
            first = WP_FALSE;

            if (para->kind == (u8)WP_PARA_IMAGE) {
                const wp_image_desc *img = wp_document_image_get(doc, para->image);

                w_str(&w, "![image](");
                if (img != NULL && img->blob_path.length > 0) {
                    w_bytes(&w, wp_strpool_get(&doc->strings, img->blob_path), img->blob_path.length);
                }
                w_str(&w, ")");
                continue;
            }

            head_len = para_prefix(doc, para, head, sizeof(head));
            if (is_fence(head, head_len)) {
                /* A fence and what it encloses go out as they came in:
                   one line each, nothing escaped. */
                w_para_verbatim(&w, doc, para);
                in_code = (wp_bool)!in_code;
                continue;
            }
            if (in_code) {
                w_para_verbatim(&w, doc, para);
                continue;
            }
            if (para_is_empty(para)) {
                w_bytes(&w, md_nbsp, sizeof(md_nbsp));
                continue;
            }
            export_paragraph(&w, doc, para);
        }
    }
    if (!first) w_str(&w, "\n");

    if (out_len != NULL) *out_len = w.pos;
    if (w.pos > out_cap) return WP_RANGE;
    return WP_OK;
}
