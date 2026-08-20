#include <string.h>
#include "format_ws.h"
#include "platform.h"

/* WordStar document mode. See docs/formats/wordstar.md for the account
   of the format itself; this file documents the decisions, not the
   conventions.
 *
 * The whole reader rests on one idea: WordStar marks its own
 * word-wrapping in the file. A carriage return the WRITER typed is 0x0D;
 * one WordStar inserted while wrapping a line is 0x8D, the same byte
 * with bit 7 set. So a paragraph is text up to the next HARD return, and
 * every soft return inside it is just where the line happened to break
 * on a screen that no longer exists.
 *
 * That is exactly the right shape for this project: the document model
 * stores paragraphs and re-wraps them for whatever screen it is on
 * (docs/layout.md). A reader that treated every return as a line break
 * would import a novel as a few thousand one-line paragraphs, all of
 * them broken to some 1980s margin, and re-wrapping would be impossible
 * without guessing which breaks were real. */

#define WS_CR_HARD   0x0Du
#define WS_CR_SOFT   0x8Du
#define WS_LF        0x0Au

/* Embedded character-style toggles. These are the raw control bytes the
   file carries; the WordStar KEYSTROKES that produce them are ^PB, ^PS
   and ^PY (docs/keymaps.md's WordStar table), where the ^P is a literal-
   next prefix that never reaches the file. */
#define WS_TOGGLE_BOLD       0x02u
#define WS_TOGGLE_UNDERLINE  0x13u
#define WS_TOGGLE_ITALIC     0x19u

typedef struct {
    wp_document  *doc;
    wp_section   *sec;
    wp_paragraph *para;      /* NULL until a paragraph is actually needed */
    u8           *scratch;   /* pending run text, never longer than the input */
    u32           scratch_len;
    u16           style_flags;
} ws_ctx;

/* Resolves the current toggle state to a real cstyle handle, reusing one
   per distinct combination rather than growing the table per run --
   find_or_add is the same dedup every other style path in this codebase
   uses. */
static wp_status ws_style_handle(ws_ctx *c, wp_cstyle *out)
{
    const wp_char_style *base;
    wp_char_style style;

    if (c->style_flags == 0) {
        *out = c->doc->default_cstyle;
        return WP_OK;
    }
    base = wp_document_cstyle_get(c->doc, c->doc->default_cstyle);
    if (base == NULL) return WP_ERR;
    style = *base;
    style.flags = c->style_flags;
    return wp_document_cstyle_find_or_add(c->doc, &style, out);
}

static wp_status ws_open_para(ws_ctx *c)
{
    u32 para_idx;
    wp_status st;

    if (c->para != NULL) return WP_OK;
    st = wp_section_paragraph_add(c->sec, &para_idx);
    if (st != WP_OK) return st;
    c->para = wp_section_paragraph_get(c->sec, para_idx);
    wp_paragraph_pstyle_set(c->para, c->doc->default_pstyle);
    return WP_OK;
}

/* Commits pending bytes as one run. Called whenever the style changes or
   the paragraph ends -- a run is precisely "text with one style". */
static wp_status ws_flush_run(ws_ctx *c)
{
    wp_status st;
    wp_span span;
    wp_cstyle handle;
    u16 run_idx;

    if (c->scratch_len == 0) return WP_OK;

    st = ws_open_para(c);
    if (st != WP_OK) return st;
    st = ws_style_handle(c, &handle);
    if (st != WP_OK) return st;
    st = wp_strpool_append(&c->doc->strings, c->scratch, c->scratch_len, &span);
    if (st != WP_OK) return st;
    st = wp_paragraph_run_add(c->para, span, handle, &run_idx);
    if (st != WP_OK) return st;

    c->scratch_len = 0;
    return WP_OK;
}

/* Ends the current paragraph at a HARD return.
 *
 * Opens an empty paragraph if none exists, so a run of hard returns
 * imports as real blank paragraphs. Blank lines are how a writer spaces
 * their manuscript, and silently swallowing them would quietly reformat
 * someone's work on the way in. */
static wp_status ws_end_para(ws_ctx *c)
{
    wp_status st = ws_flush_run(c);

    if (st != WP_OK) return st;
    st = ws_open_para(c);
    if (st != WP_OK) return st;
    c->para = NULL;
    return WP_OK;
}

static void ws_put(ws_ctx *c, u8 ch)
{
    c->scratch[c->scratch_len++] = ch;
}

wp_status format_ws_import(const u8 *bytes, u32 len, wp_document *doc)
{
    ws_ctx c;
    wp_status st;
    u32 sec_idx, i;
    wp_bool at_line_start = WP_TRUE;

    st = wp_document_init(doc);
    if (st != WP_OK) return st;
    st = wp_document_section_add(doc, &sec_idx);
    if (st != WP_OK) { wp_document_free(doc); return st; }

    c.doc = doc;
    c.sec = wp_document_section_get(doc, sec_idx);
    c.para = NULL;
    c.scratch_len = 0;
    c.style_flags = 0;

    /* One scratch buffer for the whole import, sized to the input: no run
       can exceed it, since every byte contributes at most one character.
       Heap rather than stack on purpose -- a RUN-launched Amiga program
       gets 4096 bytes of stack total (docs/amiga-platform.md), so a
       buffer scaled to a document has no business being there. */
    c.scratch = (u8 *)mem_alloc(len > 0 ? len : 1u);
    if (c.scratch == NULL) { wp_document_free(doc); return WP_NOMEM; }

    for (i = 0; i < len; i++) {
        u8 ch = bytes[i];

        /* Dot commands: a '.' in column one introduces a formatting
           directive (.pa page break, .lm left margin, .he header, ...)
           that runs to the end of the line. Consumed and dropped -- this
           document model has no page-break, margin or header concept to
           put them in, and inventing one from a directive we cannot
           render is worse than losing it. Recorded as a known gap in
           docs/formats/wordstar.md rather than passed off as supported.

           Only ".<letter>" counts. A paragraph legitimately beginning
           with "..." or a numeric ".22 calibre" is text, and treating it
           as a command would eat a line of someone's prose. */
        if (at_line_start && ch == '.' && i + 1 < len) {
            u8 next = (u8)(bytes[i + 1] & 0x7Fu);
            if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z')) {
                while (i < len && bytes[i] != WS_LF &&
                       (u8)(bytes[i] & 0x7Fu) != WS_CR_HARD) {
                    i++;
                }
                /* Leave the terminator to the normal path below so the
                   hard/soft distinction is honoured exactly once. */
                i--;
                continue;
            }
        }

        if (ch == WS_LF) continue;   /* the LF half of a CRLF pair */

        if (ch == WS_CR_HARD) {
            st = ws_end_para(&c);
            if (st != WP_OK) goto fail;
            at_line_start = WP_TRUE;
            continue;
        }

        if (ch == WS_CR_SOFT) {
            /* Word wrap, not a paragraph break: the words either side
               belong to the same paragraph. WordStar consumed the space
               at the break, so put one back -- but never two, or every
               re-wrapped line gains a double space. */
            if (c.scratch_len > 0 && c.scratch[c.scratch_len - 1] != ' ') {
                ws_put(&c, (u8)' ');
            }
            at_line_start = WP_TRUE;
            continue;
        }

        at_line_start = WP_FALSE;

        if (ch == WS_TOGGLE_BOLD || ch == WS_TOGGLE_UNDERLINE ||
            ch == WS_TOGGLE_ITALIC) {
            u16 bit = (ch == WS_TOGGLE_BOLD)      ? (u16)WP_STYLE_BOLD
                    : (ch == WS_TOGGLE_UNDERLINE) ? (u16)WP_STYLE_UNDERLINE
                                                  : (u16)WP_STYLE_ITALIC;
            st = ws_flush_run(&c);
            if (st != WP_OK) goto fail;
            c.style_flags ^= bit;
            continue;
        }

        /* Strip bit 7. In document mode WordStar sets it on soft spaces
           and on the last character of a wrapped word; the underlying
           character is the low seven bits either way.

           This is also the reader's one real limitation, and it is
           inherent rather than an oversight: a file written in an 8-bit
           codepage has accented characters that collide with the flag,
           and nothing in the file distinguishes them. Seven-bit ASCII is
           what the classic line overwhelmingly produced, so that is what
           is assumed. */
        ch &= 0x7Fu;

        /* Remaining control bytes are print-control codes this model has
           no equivalent for (^V superscript, ^T subscript, and friends).
           Dropped, not rendered as garbage glyphs. Tab is real text. */
        if (ch < 0x20u && ch != 0x09u) continue;

        ws_put(&c, ch);
    }

    st = ws_flush_run(&c);
    if (st != WP_OK) goto fail;

    mem_free(c.scratch);
    return WP_OK;

fail:
    mem_free(c.scratch);
    wp_document_free(doc);
    return st;
}

wp_bool format_ws_looks_like(const u8 *bytes, u32 len)
{
    u32 i;
    wp_bool line_start = WP_TRUE;

    if (bytes == NULL || len == 0) return WP_FALSE;

    for (i = 0; i < len; i++) {
        u8 ch = bytes[i];

        /* A soft carriage return is decisive: nothing else produces it,
           and any document long enough to have wrapped contains one. */
        if (ch == WS_CR_SOFT) return WP_TRUE;

        if (line_start && ch == '.' && i + 1 < len) {
            u8 a = (u8)(bytes[i + 1] & 0x7Fu);
            u8 b = (i + 2 < len) ? (u8)(bytes[i + 2] & 0x7Fu) : 0u;
            /* ".xx" followed by a space or end of line -- the shape of a
               real dot command, not of a sentence that starts with an
               ellipsis. */
            if (a >= 'a' && a <= 'z' && b >= 'a' && b <= 'z') return WP_TRUE;
        }

        line_start = (wp_bool)(ch == WS_LF || ch == WS_CR_HARD);
    }
    return WP_FALSE;
}
