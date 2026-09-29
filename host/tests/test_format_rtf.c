/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_rtf.h"
#include "mem_host.h"

static const u8 *para_text(wp_document *doc, u32 section, u32 para, u16 run, u32 *out_len)
{
    wp_section *sec = wp_document_section_get(doc, section);
    wp_paragraph *p = wp_section_paragraph_get(sec, para);
    wp_run *r = wp_paragraph_run_get(p, run);
    *out_len = r->text.length;
    return wp_strpool_get(&doc->strings, r->text);
}

void test_format_rtf_import_rejects_bad_header(void)
{
    wp_document doc;
    static const char *not_rtf = "hello world";

    ASSERT_TRUE(format_rtf_import((const u8 *)not_rtf, (u32)strlen(not_rtf), &doc) == WP_BADFMT);
}

void test_format_rtf_import_rejects_unsupported_charset(void)
{
    wp_document doc;
    static const char *mac_rtf = "{\\rtf1\\mac Hello\\par}";
    static const char *bad_cpg = "{\\rtf1\\ansicpg1250 Hello\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)mac_rtf, (u32)strlen(mac_rtf), &doc) == WP_UNSUPPORTED);
    ASSERT_TRUE(format_rtf_import((const u8 *)bad_cpg, (u32)strlen(bad_cpg), &doc) == WP_UNSUPPORTED);
}

void test_format_rtf_import_simple_paragraph(void)
{
    wp_document doc;
    wp_section *sec;
    static const char *text = "{\\rtf1\\ansi Hello world\\par}";
    u32 len;
    const u8 *bytes;

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);

    ASSERT_EQ_U32(doc.section_count, 1);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 1);

    bytes = para_text(&doc, 0, 0, 0, &len);
    ASSERT_EQ_U32(len, 11);
    ASSERT_MEM_EQ(bytes, "Hello world", 11);

    wp_document_free(&doc);
}

void test_format_rtf_import_no_trailing_empty_paragraph(void)
{
    wp_document doc;
    wp_section *sec;
    static const char *text = "{\\rtf1\\ansi one\\par two\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 2);

    wp_document_free(&doc);
}

void test_format_rtf_import_blank_line_is_empty_paragraph(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *p;

    static const char *text = "{\\rtf1\\ansi one\\par\\par two\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 3);

    p = wp_section_paragraph_get(sec, 1);
    ASSERT_EQ_U32(p->run_count, 0);

    wp_document_free(&doc);
}

void test_format_rtf_import_bold_italic_underline(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *p;
    wp_run *r;
    const wp_char_style *cs;
    u32 len;
    const u8 *bytes;

    static const char *text = "{\\rtf1\\ansi plain \\b bold \\i0 \\b0 \\i not-italic-again\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    p = wp_section_paragraph_get(sec, 0);

    /* "plain " (no flags), "bold " (BOLD), "not-italic-again" (ITALIC only) */
    ASSERT_EQ_U32(p->run_count, 3);

    r = wp_paragraph_run_get(p, 0);
    cs = wp_document_cstyle_get(&doc, r->style);
    ASSERT_EQ_U32(cs->flags, 0);
    bytes = para_text(&doc, 0, 0, 0, &len);
    ASSERT_EQ_U32(len, 6);
    ASSERT_MEM_EQ(bytes, "plain ", 6);

    r = wp_paragraph_run_get(p, 1);
    cs = wp_document_cstyle_get(&doc, r->style);
    ASSERT_EQ_U32(cs->flags, WP_STYLE_BOLD);
    bytes = para_text(&doc, 0, 0, 1, &len);
    ASSERT_EQ_U32(len, 5);
    ASSERT_MEM_EQ(bytes, "bold ", 5);

    r = wp_paragraph_run_get(p, 2);
    cs = wp_document_cstyle_get(&doc, r->style);
    ASSERT_EQ_U32(cs->flags, WP_STYLE_ITALIC);
    bytes = para_text(&doc, 0, 0, 2, &len);
    ASSERT_EQ_U32(len, 16);
    ASSERT_MEM_EQ(bytes, "not-italic-again", 16);

    wp_document_free(&doc);
}

void test_format_rtf_import_font_size(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *p;
    wp_run *r;
    const wp_char_style *cs;

    /* \fs48 = 24pt = half-points*10 = 480 twips */
    static const char *text = "{\\rtf1\\ansi\\fs48 big\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    p = wp_section_paragraph_get(sec, 0);
    r = wp_paragraph_run_get(p, 0);
    cs = wp_document_cstyle_get(&doc, r->style);
    ASSERT_EQ_U32(cs->size_twips, 480);

    wp_document_free(&doc);
}

void test_format_rtf_import_alignment_and_indents(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *p;
    const wp_para_style *ps;

    static const char *text =
        "{\\rtf1\\ansi\\qc\\li720\\ri360\\fi-360\\sb120\\sa60 centered\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    p = wp_section_paragraph_get(sec, 0);
    ps = wp_document_pstyle_get(&doc, p->style);

    ASSERT_EQ_U32(ps->align, WP_ALIGN_CENTER);
    ASSERT_EQ_U32(ps->indent_left, 720);
    ASSERT_EQ_U32(ps->indent_right, 360);
    ASSERT_TRUE(ps->indent_first == -360);
    ASSERT_EQ_U32(ps->space_before, 120);
    ASSERT_EQ_U32(ps->space_after, 60);

    wp_document_free(&doc);
}

void test_format_rtf_import_color_table(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *p;
    wp_run *r;
    const wp_char_style *cs;

    static const char *text =
        "{\\rtf1\\ansi{\\colortbl;\\red255\\green0\\blue0;\\red0\\green0\\blue255;}"
        "\\cf1 red\\cf2 blue\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    p = wp_section_paragraph_get(sec, 0);
    ASSERT_EQ_U32(p->run_count, 2);

    r = wp_paragraph_run_get(p, 0);
    cs = wp_document_cstyle_get(&doc, r->style);
    ASSERT_EQ_U32(cs->color_r, 255);
    ASSERT_EQ_U32(cs->color_g, 0);
    ASSERT_EQ_U32(cs->color_b, 0);

    r = wp_paragraph_run_get(p, 1);
    cs = wp_document_cstyle_get(&doc, r->style);
    ASSERT_EQ_U32(cs->color_r, 0);
    ASSERT_EQ_U32(cs->color_g, 0);
    ASSERT_EQ_U32(cs->color_b, 255);

    wp_document_free(&doc);
}

void test_format_rtf_import_hex_escape_decodes_cp1252(void)
{
    wp_document doc;
    u32 len;
    const u8 *bytes;

    /* \'e9 is cp1252 0xE9 = U+00E9 (e-acute) = UTF-8 C3 A9 */
    static const char *text = "{\\rtf1\\ansi caf\\'e9\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    bytes = para_text(&doc, 0, 0, 0, &len);
    ASSERT_EQ_U32(len, 5); /* c a f 0xC3 0xA9 */
    ASSERT_MEM_EQ(bytes, "caf\xC3\xA9", 5);

    wp_document_free(&doc);
}

void test_format_rtf_import_special_chars(void)
{
    wp_document doc;
    u32 len;
    const u8 *bytes;

    static const char *text = "{\\rtf1\\ansi a\\tab b\\emdash c\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    bytes = para_text(&doc, 0, 0, 0, &len);
    /* The single space after each no-parameter control word is its
       delimiter and is consumed, not emitted as text (standard RTF
       rule -- docs/formats/rtf.md) -- so "a", TAB, "b", em-dash (3
       bytes UTF-8), "c" with no surrounding spaces surviving. */
    ASSERT_EQ_U32(len, 7);
    ASSERT_MEM_EQ(bytes, "a\tb\xE2\x80\x94""c", 7);

    wp_document_free(&doc);
}

void test_format_rtf_import_skips_fonttbl_and_ignorable_destination(void)
{
    wp_document doc;
    wp_section *sec;
    u32 len;
    const u8 *bytes;

    static const char *text =
        "{\\rtf1\\ansi{\\fonttbl{\\f0 Times New Roman;}}"
        "{\\*\\generator Some Fancy Writer 3.0;}"
        "Actual text\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 1);

    bytes = para_text(&doc, 0, 0, 0, &len);
    ASSERT_EQ_U32(len, 11);
    ASSERT_MEM_EQ(bytes, "Actual text", 11);

    wp_document_free(&doc);
}

void test_format_rtf_import_unrecognized_control_word_is_ignored(void)
{
    wp_document doc;
    u32 len;
    const u8 *bytes;

    /* \nonexistentcontrolword must be silently ignored, not an error,
       and the text following it must still import (RTF forward-
       compatibility rule -- see docs/formats/rtf.md). */
    static const char *text = "{\\rtf1\\ansi\\nonexistentcontrolword text\\par}";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    bytes = para_text(&doc, 0, 0, 0, &len);
    ASSERT_EQ_U32(len, 4);
    ASSERT_MEM_EQ(bytes, "text", 4);

    wp_document_free(&doc);
}

void test_format_rtf_import_truncated_hex_escape_is_badfmt(void)
{
    wp_document doc;
    static const char *text = "{\\rtf1\\ansi bad\\'e\\par}"; /* only 1 hex digit */

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_BADFMT);
}

void test_format_rtf_import_exceeds_max_nesting_depth_is_range(void)
{
    wp_document doc;
    char text[8192];
    u32 i, pos = 0;
    static const char prefix[] = "{\\rtf1\\ansi ";

    memcpy(text, prefix, strlen(prefix));
    pos = (u32)strlen(prefix);
    /* 100 nested groups is well past RTF_MAX_DEPTH (64). */
    for (i = 0; i < 100; i++) text[pos++] = '{';
    for (i = 0; i < 100; i++) text[pos++] = '}';
    text[pos++] = '}';

    ASSERT_TRUE(format_rtf_import((const u8 *)text, pos, &doc) == WP_RANGE);
}

/* Truncated/unbalanced input (no top-level closing brace ever found) is
   recovered leniently, not rejected: whatever content was parsed before
   truncation is kept as a best-effort partial import. This is a
   deliberate design choice (real-world word processors are typically
   forgiving of malformed RTF) and is tested here so it stays an
   intentional, documented contract rather than an untested accident. */
void test_format_rtf_import_truncated_input_recovers_leniently(void)
{
    wp_document doc;
    wp_section *sec;
    u32 len;
    const u8 *bytes;
    static const char *text = "{\\rtf1\\ansi partial text with no closing brace";

    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 1);
    bytes = para_text(&doc, 0, 0, 0, &len);
    ASSERT_EQ_U32(len, 34);
    ASSERT_MEM_EQ(bytes, "partial text with no closing brace", 34);

    wp_document_free(&doc);
}

void test_format_rtf_import_no_leaks(void)
{
    wp_document doc;

    static const char *text =
        "{\\rtf1\\ansi{\\colortbl;\\red255\\green0\\blue0;}"
        "\\b bold\\b0 \\cf1 red\\par plain\\par}";

    mem_host_reset_counters();
    ASSERT_TRUE(format_rtf_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_document_free(&doc);
    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
