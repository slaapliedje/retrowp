/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_prn.h"
#include "format_txt.h"
#include "platform.h"

/* format_prn's printouts, worked out by hand at 10 cpi and 6 lpi: 144
   twips a column, 240 a line. */

typedef struct {
    u8  buf[8192];
    u32 len;
} prn_sink;

static wp_status sink_write(void *user, const u8 *bytes, u32 len)
{
    prn_sink *k = (prn_sink *)user;
    if (k->len + len > sizeof(k->buf)) return WP_RANGE;
    memcpy(k->buf + k->len, bytes, len);
    k->len += len;
    return WP_OK;
}

/* A document of txt's paragraphs on a page `cols` columns wide and
   `lines` lines high, every margin `margin_cols`/`margin_lines`. */
static void make_doc(wp_document *doc, const char *txt, u32 cols, u32 lines,
                     u32 margin_cols, u32 margin_lines)
{
    wp_section *sec;

    ASSERT_TRUE(format_txt_import((const u8 *)txt, (u32)strlen(txt), doc) == WP_OK);
    sec = wp_document_section_get(doc, 0);
    sec->page_width = (i32)((cols + 2u * margin_cols) * 144u);
    sec->page_height = (i32)(lines * 240u);
    sec->margin_left = sec->margin_right = (i32)(margin_cols * 144u);
    sec->margin_top = sec->margin_bottom = (i32)(margin_lines * 240u);
}

static void set_para_style(wp_document *doc, u32 para, const wp_para_style *ps)
{
    wp_pstyle h;
    ASSERT_TRUE(wp_document_pstyle_find_or_add(doc, ps, &h) == WP_OK);
    wp_paragraph_pstyle_set(wp_section_paragraph_get(wp_document_section_get(doc, 0), para), h);
}

static wp_para_style base_style(const wp_document *doc)
{
    return *wp_document_pstyle_get(doc, doc->default_pstyle);
}

static void print_doc(const wp_document *doc, u16 codepage, prn_sink *k, u32 *pages)
{
    wp_prn_options opt;

    memset(&opt, 0, sizeof(opt));
    opt.codepage = codepage;
    k->len = 0;
    ASSERT_TRUE(format_prn_export(doc, &opt, sink_write, k, pages) == WP_OK);
}

static void assert_out(const prn_sink *k, const char *want)
{
    u32 n = (u32)strlen(want);
    ASSERT_EQ_U32(k->len, n);
    ASSERT_MEM_EQ(k->buf, want, n);
}

void test_format_prn_letter_page(void)
{
    wp_document doc;
    prn_sink k;
    u32 pages;

    /* Letter with inch margins: 65 columns from column 10, six lines of
       top margin, then the text, then the form feed. */
    make_doc(&doc, "Hello world\n", 65, 66, 10, 6);
    print_doc(&doc, 0, &k, &pages);
    ASSERT_EQ_U32(pages, 1);
    assert_out(&k, "\n\n\n\n\n\n          Hello world\n\f");
    wp_document_free(&doc);
}

void test_format_prn_crlf(void)
{
    wp_document doc;
    wp_prn_options opt;
    prn_sink k;

    make_doc(&doc, "a\nb\n", 20, 10, 0, 0);
    memset(&opt, 0, sizeof(opt));
    opt.crlf = WP_TRUE;
    k.len = 0;
    ASSERT_TRUE(format_prn_export(&doc, &opt, sink_write, &k, NULL) == WP_OK);
    assert_out(&k, "a\r\nb\r\n\f");
    wp_document_free(&doc);
}

void test_format_prn_wraps_at_words(void)
{
    wp_document doc;
    prn_sink k;

    make_doc(&doc, "the quick brown fox jumps over the lazy dog\n", 20, 10, 0, 0);
    print_doc(&doc, 0, &k, NULL);
    assert_out(&k, "the quick brown fox\njumps over the lazy\ndog\n\f");
    wp_document_free(&doc);
}

void test_format_prn_cuts_a_long_word(void)
{
    wp_document doc;
    prn_sink k;

    make_doc(&doc, "abcdefghijklmnopqrstuvwxy z\n", 10, 10, 0, 0);
    print_doc(&doc, 0, &k, NULL);
    assert_out(&k, "abcdefghij\nklmnopqrst\nuvwxy z\n\f");
    wp_document_free(&doc);
}

void test_format_prn_alignment(void)
{
    wp_document doc;
    wp_para_style ps;
    prn_sink k;

    make_doc(&doc, "mid\nend\none two three four\nlast line\n", 12, 20, 0, 0);
    ps = base_style(&doc);
    ps.align = WP_ALIGN_CENTER;  set_para_style(&doc, 0, &ps);
    ps.align = WP_ALIGN_RIGHT;   set_para_style(&doc, 1, &ps);
    ps.align = WP_ALIGN_JUSTIFY; set_para_style(&doc, 2, &ps);
    set_para_style(&doc, 3, &ps);
    print_doc(&doc, 0, &k, NULL);
    /* "one two" is stretched to 12 by its one gap; a paragraph's last
       line, and a one-line paragraph, are never stretched. */
    assert_out(&k, "    mid\n         end\none      two\nthree four\nlast line\n\f");
    wp_document_free(&doc);
}

void test_format_prn_indents_and_tabs(void)
{
    wp_document doc;
    wp_para_style ps;
    prn_sink k;

    make_doc(&doc, "aa bb cc dd ee\nx\ty\n", 10, 20, 0, 0);
    ps = base_style(&doc);
    ps.indent_left = 2 * 144;
    ps.indent_first = 2 * 144;
    ps.indent_right = 1 * 144;
    set_para_style(&doc, 0, &ps);
    ps = base_style(&doc);
    ps.tab_count = 1;
    ps.tab_kind[0] = WP_TAB_LEFT;
    ps.tab_pos[0] = 7 * 144;
    set_para_style(&doc, 1, &ps);
    print_doc(&doc, 0, &k, NULL);
    /* First line from column 4, the rest from 2, all ending by 9. */
    assert_out(&k, "    aa bb\n  cc dd\n  ee\nx      y\n\f");
    wp_document_free(&doc);
}

void test_format_prn_spacing(void)
{
    wp_document doc;
    wp_para_style ps;
    prn_sink k;

    make_doc(&doc, "one\ntwo\nthree\n", 20, 20, 0, 0);
    ps = base_style(&doc);
    ps.space_after = 240;        /* a line after "one" */
    set_para_style(&doc, 0, &ps);
    ps = base_style(&doc);
    ps.line_spacing = 480;       /* double: a line below each */
    set_para_style(&doc, 1, &ps);
    print_doc(&doc, 0, &k, NULL);
    assert_out(&k, "one\n\ntwo\n\nthree\n\f");
    wp_document_free(&doc);
}

void test_format_prn_pages_header_footer(void)
{
    wp_document doc;
    wp_section *sec;
    wp_span span;
    prn_sink k;
    u32 pages;

    /* Six lines a page: one of top margin, four of text, one of bottom. */
    make_doc(&doc, "1\n2\n3\n4\n5\n", 12, 6, 0, 1);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Doc|#", 5, &span) == WP_OK);
    sec->header = span;
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"- # -", 5, &span) == WP_OK);
    sec->footer = span;
    print_doc(&doc, 0, &k, &pages);
    ASSERT_EQ_U32(pages, 2);
    assert_out(&k,
        "Doc        1\n1\n2\n3\n4\n   - 1 -\n\f"
        "Doc        2\n5\n\n\n\n   - 2 -\n\f");
    wp_document_free(&doc);
}

void test_format_prn_page_break_and_top_space(void)
{
    wp_document doc;
    wp_para_style ps;
    prn_sink k;
    u32 pages;

    make_doc(&doc, "a\nb\n", 10, 10, 0, 0);
    ps = base_style(&doc);
    ps.page_break_before = 1;
    ps.space_before = 480;       /* dropped at the top of the new page */
    set_para_style(&doc, 1, &ps);
    print_doc(&doc, 0, &k, &pages);
    ASSERT_EQ_U32(pages, 2);
    assert_out(&k, "a\n\fb\n\f");
    wp_document_free(&doc);
}

void test_format_prn_encodings(void)
{
    wp_document doc;
    prn_sink k;
    /* café “ok”… € */
    const char *txt = "caf\xc3\xa9 \xe2\x80\x9cok\xe2\x80\x9d\xe2\x80\xa6 \xe2\x82\xac\n";

    make_doc(&doc, txt, 40, 10, 0, 0);
    print_doc(&doc, 0, &k, NULL);
    assert_out(&k, "caf? \"ok\"... ?\n\f");
    print_doc(&doc, 437, &k, NULL);
    assert_out(&k, "caf\x82 \"ok\"... ?\n\f");
    /* 1252 has the euro at 0x80, which a Latin-1 printer takes for a
       control code: the nearest ASCII instead. */
    print_doc(&doc, 1252, &k, NULL);
    assert_out(&k, "caf\xe9 \"ok\"... ?\n\f");
    wp_document_free(&doc);
}

void test_format_prn_empty_document(void)
{
    wp_document doc;
    prn_sink k;
    u32 pages = 99;

    make_doc(&doc, "", 20, 10, 0, 0);
    print_doc(&doc, 0, &k, &pages);
    ASSERT_EQ_U32(pages, 0);
    ASSERT_EQ_U32(k.len, 0);
    wp_document_free(&doc);
}

/* ---- Drivers ---------------------------------------------------------- */

/* Appends a run of `text` in `flags` (WP_STYLE_*) to paragraph 0. */
static void add_run(wp_document *doc, const char *text, u16 flags)
{
    wp_char_style cs = *wp_document_cstyle_get(doc, doc->default_cstyle);
    wp_cstyle h;
    wp_span span;
    u16 ri;

    cs.flags = flags;
    ASSERT_TRUE(wp_document_cstyle_find_or_add(doc, &cs, &h) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(wp_section_paragraph_get(wp_document_section_get(doc, 0), 0),
                                     span, h, &ri) == WP_OK);
}

static void print_with(const wp_document *doc, u8 driver, u8 sides, prn_sink *k)
{
    wp_prn_options opt;

    memset(&opt, 0, sizeof(opt));
    opt.driver = driver;
    opt.sides = sides;
    k->len = 0;
    ASSERT_TRUE(format_prn_export(doc, &opt, sink_write, k, NULL) == WP_OK);
}

/* "a " then a bold "b": each driver's own codes around the bold, back to
   none by the line's end. */
void test_format_prn_epson(void)
{
    wp_document doc;
    prn_sink k;

    make_doc(&doc, "a \n", 20, 10, 0, 0);
    add_run(&doc, "b", WP_STYLE_BOLD);
    add_run(&doc, "c", (u16)(WP_STYLE_ITALIC | WP_STYLE_UNDERLINE));
    print_with(&doc, WP_PRN_EPSON, 0, &k);
    /* reset, 1/6" lines, 10 cpi, page length 10 lines, then the line */
    assert_out(&k, "\033@\0332\033P\033C\012a \033Eb\033F\0334\033-1c\0335\033-0\n\f");
    wp_document_free(&doc);
}

void test_format_prn_pcl(void)
{
    wp_document doc;
    prn_sink k;

    make_doc(&doc, "a \n", 20, 30, 0, 0);     /* taller than wide: portrait */
    add_run(&doc, "b", WP_STYLE_BOLD);
    print_with(&doc, WP_PRN_PCL, WP_PRN_DUPLEX_LONG, &k);
    assert_out(&k, "\033E\033&l1S\033&l0L\033&l6D\033(10U\033(s0p10h0s0b3T"
                   "\033&l0O\033&l0E"
                   "a \033(s3Bb\033(s0B\n\f\033E");
    wp_document_free(&doc);
}

/* PCL's paper code follows the section's page: Letter is 2, A4 26, and a
   page wider than tall is landscape. */
void test_format_prn_pcl_paper(void)
{
    wp_document doc;
    wp_section *sec;
    prn_sink k;

    make_doc(&doc, "a\n", 20, 10, 0, 0);
    sec = wp_document_section_get(&doc, 0);
    sec->page_width = 16838;  sec->page_height = 11906;     /* A4, on its side */
    print_with(&doc, WP_PRN_PCL, 0, &k);
    k.buf[k.len] = 0;
    ASSERT_TRUE(strstr((const char *)k.buf, "\033&l26A\033&l1O") != NULL);
    ASSERT_TRUE(strstr((const char *)k.buf, "\033&l0S") != NULL);
    wp_document_free(&doc);
}

void test_format_prn_amiga(void)
{
    wp_document doc;
    prn_sink k;

    make_doc(&doc, "a \n", 20, 10, 0, 0);
    add_run(&doc, "b", WP_STYLE_BOLD);
    print_with(&doc, WP_PRN_AMIGA, 0, &k);
    assert_out(&k, "\033#1\033[0w\033[1z" "a \033[1mb\033[22m\n\f");
    wp_document_free(&doc);
}

/* PostScript: a page description on the same grid -- Courier faces,
   each line moved to its place, duplex through setpagedevice, a page
   per showpage. Checked by its parts, not byte for byte. */
void test_format_prn_postscript(void)
{
    wp_document doc;
    prn_sink k;
    const char *t;

    make_doc(&doc, "a (x)\n", 20, 10, 1, 1);
    add_run(&doc, "b", WP_STYLE_BOLD);
    add_run(&doc, "u", WP_STYLE_UNDERLINE);
    print_with(&doc, WP_PRN_POSTSCRIPT, WP_PRN_DUPLEX_SHORT, &k);
    ASSERT_TRUE(k.len < sizeof(k.buf));
    k.buf[k.len] = 0;
    t = (const char *)k.buf;
    ASSERT_TRUE(strncmp(t, "%!PS-Adobe-3.0\n", 15) == 0);
    ASSERT_TRUE(strstr(t, "/F1n /Courier-Bold RE") != NULL);
    ASSERT_TRUE(strstr(t, "<< /Duplex true /Tumble true >> setpagedevice") != NULL);
    /* 22 columns of 7.2 pt by 10 lines of 12 pt */
    ASSERT_TRUE(strstr(t, "%%Page: 1 1\n<< /PageSize [158.4 120.0] >> setpagedevice") != NULL);
    /* a left margin of one column (7.2 pt); one line of top margin, then
       the baseline 9 pt into its 12 pt line: 120 - 12 - 9 = 99 */
    ASSERT_TRUE(strstr(t, "F0 7.2 99.0 M (a \\(x\\)) S\n") != NULL);
    ASSERT_TRUE(strstr(t, "F1 43.2 99.0 M (b) S\n") != NULL);
    ASSERT_TRUE(strstr(t, "7.2 50.4 97.5 U F0 50.4 99.0 M (u) S\n") != NULL);
    ASSERT_TRUE(strstr(t, "showpage\n%%EOF\n") != NULL);
    wp_document_free(&doc);
}

/* No pages, no bytes -- not even a driver's reset. */
void test_format_prn_driver_empty(void)
{
    wp_document doc;
    prn_sink k;

    make_doc(&doc, "", 20, 10, 0, 0);
    print_with(&doc, WP_PRN_PCL, 0, &k);
    ASSERT_EQ_U32(k.len, 0);
    wp_document_free(&doc);
}

static wp_status file_write(void *user, const u8 *bytes, u32 len)
{
    return plat_file_write((plat_file *)user, bytes, len);
}

/* A Letter page in PostScript, kept as build/test_format_prn.ps for an
   interpreter to render (Ghostscript checks it as a real program). */
void test_format_prn_postscript_file(void)
{
    wp_document doc;
    wp_section *sec;
    wp_para_style ps;
    wp_span span;
    wp_prn_options opt;
    plat_file *f;
    u32 pages = 0;

    make_doc(&doc, "RetroWP\nThis line has \n", 65, 66, 10, 6);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"RetroWP|PostScript test", 23, &span) == WP_OK);
    sec->header = span;
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Page #", 6, &span) == WP_OK);
    sec->footer = span;
    ps = base_style(&doc);
    ps.align = WP_ALIGN_CENTER;
    set_para_style(&doc, 0, &ps);
    {
        /* paragraph 0 bold; paragraph 1 gets bold, italic, underline runs */
        wp_paragraph *p0 = wp_section_paragraph_get(sec, 0);
        wp_char_style cs = *wp_document_cstyle_get(&doc, doc.default_cstyle);
        wp_cstyle h;
        wp_paragraph *p1 = wp_section_paragraph_get(sec, 1);
        u16 ri;
        cs.flags = WP_STYLE_BOLD;
        ASSERT_TRUE(wp_document_cstyle_find_or_add(&doc, &cs, &h) == WP_OK);
        wp_run_cstyle_set(p0, 0, h);
        ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"bold", 4, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(p1, span, h, &ri) == WP_OK);
        ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)", ", 2, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(p1, span, doc.default_cstyle, &ri) == WP_OK);
        cs.flags = WP_STYLE_ITALIC;
        ASSERT_TRUE(wp_document_cstyle_find_or_add(&doc, &cs, &h) == WP_OK);
        ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"italic", 6, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(p1, span, h, &ri) == WP_OK);
        ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)" and ", 5, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(p1, span, doc.default_cstyle, &ri) == WP_OK);
        cs.flags = WP_STYLE_UNDERLINE;
        ASSERT_TRUE(wp_document_cstyle_find_or_add(&doc, &cs, &h) == WP_OK);
        ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"underlined words", 16, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(p1, span, h, &ri) == WP_OK);
        ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)" -- caf\xc3\xa9.", 10, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(p1, span, doc.default_cstyle, &ri) == WP_OK);
    }
    memset(&opt, 0, sizeof(opt));
    opt.driver = WP_PRN_POSTSCRIPT;
    ASSERT_TRUE(plat_file_open("build/test_format_prn.ps", PLAT_FILE_WRITE, &f) == WP_OK);
    ASSERT_TRUE(format_prn_export(&doc, &opt, file_write, f, &pages) == WP_OK);
    plat_file_close(f);
    ASSERT_EQ_U32(pages, 1);
    wp_document_free(&doc);
}
