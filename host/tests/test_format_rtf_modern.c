/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_rtf.h"

/* What a modern writer (Word, WordPad, LibreOffice, TextEdit) puts in an
   RTF file and the importer used to get wrong or drop: Unicode, \plain,
   hidden text, headers, list numbers, tables, line spacing, tab stops,
   page setup, \bin -- and the exporter writing the same things back. */

static wp_status import(const char *rtf, wp_document *doc)
{
    return format_rtf_import((const u8 *)rtf, (u32)strlen(rtf), doc);
}

/* All of one paragraph's text, runs joined, NUL-terminated. */
static void para_all(wp_document *doc, u32 pi, char *out, u32 cap)
{
    wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(doc, 0), pi);
    u32 n = 0;
    u16 r;

    for (r = 0; p != NULL && r < p->run_count; r++) {
        u32 len = p->runs[r].text.length;
        if (n + len >= cap) len = cap - 1 - n;
        memcpy(out + n, wp_strpool_get(&doc->strings, p->runs[r].text), len);
        n += len;
    }
    out[n] = '\0';
}

static const wp_para_style *pstyle_of(wp_document *doc, u32 pi)
{
    wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(doc, 0), pi);
    return wp_document_pstyle_get(doc, p->style);
}

void test_rtf_modern_unicode_and_its_fallback(void)
{
    wp_document doc;
    char t[64];

    /* \uN then one fallback character (\uc1, the default), as a literal
       or as a \'hh -- the fallback is skipped either way. */
    ASSERT_TRUE(import("{\\rtf1\\ansi a\\u8364?b\\u1046\\'3fc\\par}", &doc) == WP_OK);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "a\xe2\x82\xac" "b\xd0\x96" "c");
    wp_document_free(&doc);

    /* \uc0 (TextEdit's curly quotes): no fallback to skip. Negative N is
       the signed 16-bit spelling of a high code point. */
    ASSERT_TRUE(import("{\\rtf1\\ansi\\uc0\\u8220 x\\u8221 \\u-3913 y\\par}", &doc) == WP_OK);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "\xe2\x80\x9cx\xe2\x80\x9d\xef\x82\xb7y");
    wp_document_free(&doc);

    /* The fallback never reaches past the group the \u was in. */
    ASSERT_TRUE(import("{\\rtf1\\ansi{\\u8364}x\\par}", &doc) == WP_OK);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "\xe2\x82\xacx");
    wp_document_free(&doc);

    /* \uc is scoped to its group. */
    ASSERT_TRUE(import("{\\rtf1\\ansi{\\uc2\\u8364 XY}\\u8364?z\\par}", &doc) == WP_OK);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "\xe2\x82\xac\xe2\x82\xacz");
    wp_document_free(&doc);
}

void test_rtf_modern_plain_resets_and_hidden_text_stays_out(void)
{
    wp_document doc;
    wp_paragraph *p;
    const wp_char_style *cs;
    char t[64];

    ASSERT_TRUE(import("{\\rtf1\\ansi\\b\\fs40 big\\par\\pard\\plain small{\\v secret} end\\par}", &doc) == WP_OK);
    p = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 1);
    cs = wp_document_cstyle_get(&doc, p->runs[0].style);
    ASSERT_TRUE(cs != NULL && (cs->flags & WP_STYLE_BOLD) == 0 && cs->size_twips == 240);
    para_all(&doc, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "small end");
    wp_document_free(&doc);
}

void test_rtf_modern_word_noise_stays_out_of_the_body(void)
{
    wp_document doc;
    char t[64];

    ASSERT_TRUE(import("{\\rtf1\\ansi{\\headerr Header}{\\footerl Footer}"
                       "{\\listtext 1.\\tab}Item\\par"
                       "{\\pntext\\f0 2.\\tab}Next\\par}", &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 2);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Item");
    para_all(&doc, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "Next");
    wp_document_free(&doc);

    /* \bin's payload is skipped whatever bytes it holds. */
    ASSERT_TRUE(import("{\\rtf1\\ansi a{\\*\\blob\\bin2 }}}b\\par}", &doc) == WP_OK);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "ab");
    wp_document_free(&doc);
}

void test_rtf_modern_table_rows_become_tabbed_paragraphs(void)
{
    wp_document doc;
    char t[64];

    ASSERT_TRUE(import("{\\rtf1\\ansi\\trowd\\cellx1000\\cellx2000 \\intbl a\\cell b\\cell\\row"
                       "\\pard after\\par}", &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 2);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "a\tb");
    para_all(&doc, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "after");
    wp_document_free(&doc);
}

void test_rtf_modern_line_spacing_tabs_and_page_setup(void)
{
    wp_document doc;
    const wp_para_style *ps;
    const wp_section *sec;

    ASSERT_TRUE(import("{\\rtf1\\ansi\\paperw11906\\paperh16838\\margl1134\\margr1134\\margt1417\\margb1417 "
                       "\\pard\\sl360\\slmult1 a\\par"
                       "\\pard\\sl240\\slmult1 b\\par"
                       "\\pard\\sl400\\slmult0 c\\par"
                       "\\pard\\sl-300 d\\par"
                       "\\pard\\tqr\\tx4000\\tx1000\\tqc\\tx2500 e\\par}", &doc) == WP_OK);
    sec = &doc.sections[0];
    ASSERT_EQ_U32((u32)sec->page_width, 11906);
    ASSERT_EQ_U32((u32)sec->page_height, 16838);
    ASSERT_EQ_U32((u32)sec->margin_left, 1134);
    ASSERT_EQ_U32((u32)sec->margin_top, 1417);

    ASSERT_EQ_U32((u32)pstyle_of(&doc, 0)->line_spacing, 360);
    ASSERT_EQ_U32((u32)pstyle_of(&doc, 1)->line_spacing, 0);       /* exactly single */
    ASSERT_TRUE(pstyle_of(&doc, 2)->line_spacing == -400);          /* at least */
    ASSERT_TRUE(pstyle_of(&doc, 3)->line_spacing == -300);          /* "exactly", read as at least */

    ps = pstyle_of(&doc, 4);
    ASSERT_EQ_U32(ps->tab_count, 3);                                 /* sorted, kinds kept */
    ASSERT_EQ_U32((u32)ps->tab_pos[0], 1000);
    ASSERT_EQ_U32(ps->tab_kind[0], (u32)WP_TAB_LEFT);
    ASSERT_EQ_U32((u32)ps->tab_pos[1], 2500);
    ASSERT_EQ_U32(ps->tab_kind[1], (u32)WP_TAB_CENTER);
    ASSERT_EQ_U32((u32)ps->tab_pos[2], 4000);
    ASSERT_EQ_U32(ps->tab_kind[2], (u32)WP_TAB_RIGHT);
    wp_document_free(&doc);
}

void test_rtf_modern_alike_paragraphs_share_one_style(void)
{
    wp_document doc;
    wp_section *sec;

    ASSERT_TRUE(import("{\\rtf1\\ansi\\pard\\li720 a\\par\\pard\\li720 b\\par}", &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(sec->paras[0].style == sec->paras[1].style);
    wp_document_free(&doc);
}

void test_rtf_modern_export_round_trips_the_new_properties(void)
{
    wp_document doc, back;
    u8 out[2048];
    u32 len = 0;
    const wp_para_style *ps;
    char t[64];

    ASSERT_TRUE(import("{\\rtf1\\ansi\\paperw11906\\paperh16838\\margl1134 "
                       "\\pard\\sl480\\slmult1\\tqr\\tx4000 x\\tab \\u1046?\\u8364?\\par}", &doc) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = 0;
    ASSERT_TRUE(strstr((const char *)out, "\\u1046?") != NULL);   /* Cyrillic: no cp1252 byte */
    ASSERT_TRUE(strstr((const char *)out, "\\'80") != NULL);      /* euro: cp1252 has it */
    ASSERT_TRUE(strstr((const char *)out, "\\tab ") != NULL);
    ASSERT_TRUE(strstr((const char *)out, "\\paperw11906") != NULL);

    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    ps = pstyle_of(&back, 0);
    ASSERT_EQ_U32((u32)ps->line_spacing, 480);
    ASSERT_EQ_U32(ps->tab_count, 1);
    ASSERT_EQ_U32(ps->tab_kind[0], (u32)WP_TAB_RIGHT);
    ASSERT_EQ_U32((u32)ps->tab_pos[0], 4000);
    ASSERT_EQ_U32((u32)back.sections[0].page_width, 11906);
    ASSERT_EQ_U32((u32)back.sections[0].margin_left, 1134);
    para_all(&back, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "x\t\xd0\x96\xe2\x82\xac");

    wp_document_free(&back);
    wp_document_free(&doc);
}

void test_rtf_modern_page_breaks_both_ways(void)
{
    wp_document doc, back;
    u8 out[1024];
    u32 len = 0;
    char t[32];

    /* Word's "\par\page\pard": no empty paragraph, and the break survives
       the \pard; \pagebb is the same thing said as a property. */
    ASSERT_TRUE(import("{\\rtf1\\ansi one\\par\\page\\pard two\\par\\pard\\pagebb three\\par four\\par}", &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);
    para_all(&doc, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "two");
    ASSERT_TRUE(!pstyle_of(&doc, 0)->page_break_before);
    ASSERT_TRUE(pstyle_of(&doc, 1)->page_break_before);
    ASSERT_TRUE(pstyle_of(&doc, 2)->page_break_before);
    ASSERT_TRUE(pstyle_of(&doc, 3)->page_break_before);   /* \pagebb carries until \pard, as in RTF */

    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = 0;
    ASSERT_TRUE(strstr((const char *)out, "\\pagebb") != NULL);
    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    ASSERT_TRUE(pstyle_of(&back, 1)->page_break_before);
    ASSERT_TRUE(!pstyle_of(&back, 0)->page_break_before);
    wp_document_free(&back);
    wp_document_free(&doc);

    /* A break mid-paragraph ends it there. */
    ASSERT_TRUE(import("{\\rtf1\\ansi one\\page two\\par}", &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 2);
    ASSERT_TRUE(pstyle_of(&doc, 1)->page_break_before);
    wp_document_free(&doc);
}

static void span_text(wp_document *doc, wp_span sp, char *out, u32 cap)
{
    u32 n = (sp.length < cap - 1) ? sp.length : cap - 1;
    if (n > 0) memcpy(out, wp_strpool_get(&doc->strings, sp), n);
    out[n] = '\0';
}

void test_rtf_modern_headers_and_footers_both_ways(void)
{
    wp_document doc, back;
    u8 out[2048];
    u32 len = 0;
    char t[64];

    /* Word's shape: parts on tab stops, the page number as a PAGE field;
       the left-page header is not the one taken; body untouched. */
    ASSERT_TRUE(import("{\\rtf1\\ansi"
                       "{\\headerl Left pages}"
                       "{\\header \\pard\\plain\\tqc\\tx4680\\tqr\\tx9360 Report\\tab\\tab Page "
                       "{\\field{\\*\\fldinst { PAGE }}{\\fldrslt {1}}}\\par}"
                       "{\\footer \\pard\\qc - \\chpgn  -\\par}"
                       "\\pard Body\\par}", &doc) == WP_OK);
    span_text(&doc, doc.sections[0].header, t, sizeof(t));
    ASSERT_EQ_STR(t, "Report||Page #");
    span_text(&doc, doc.sections[0].footer, t, sizeof(t));
    ASSERT_EQ_STR(t, "- # -");
    ASSERT_EQ_U32(doc.sections[0].para_count, 1);
    para_all(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Body");

    /* And back: \header with \chpgn and tabs reads as the same text. */
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = 0;
    ASSERT_TRUE(strstr((const char *)out, "{\\header") != NULL);
    ASSERT_TRUE(strstr((const char *)out, "\\chpgn") != NULL);
    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    span_text(&back, back.sections[0].header, t, sizeof(t));
    ASSERT_EQ_STR(t, "Report||Page #");
    span_text(&back, back.sections[0].footer, t, sizeof(t));
    ASSERT_EQ_STR(t, "- # -");
    ASSERT_EQ_U32(back.sections[0].para_count, 1);
    wp_document_free(&back);
    wp_document_free(&doc);
}
