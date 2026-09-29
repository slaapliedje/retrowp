/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_rtf.h"

void test_format_rtf_export_reports_size_on_overflow(void)
{
    wp_document doc;
    u8 sentinel[64];
    u32 needed = 0;
    u32 i;

    ASSERT_TRUE(format_rtf_import((const u8 *)"{\\rtf1\\ansi hi\\par}", 19, &doc) == WP_OK);

    ASSERT_TRUE(format_rtf_export(&doc, NULL, 0, &needed) == WP_RANGE);
    ASSERT_TRUE(needed > 0);

    for (i = 0; i < sizeof(sentinel); i++) sentinel[i] = 0xAA;
    ASSERT_TRUE(format_rtf_export(&doc, sentinel, 4, &needed) == WP_RANGE);
    /* Nothing written on failure -- format_rtf.h's documented contract. */
    for (i = 0; i < sizeof(sentinel); i++) ASSERT_TRUE(sentinel[i] == 0xAA);

    wp_document_free(&doc);
}

void test_format_rtf_roundtrip_plain_text(void)
{
    wp_document doc, doc2;
    u8 buf[512];
    u32 needed;
    u32 len1, len2;
    const u8 *bytes1;
    const u8 *bytes2;
    wp_section *sec;

    ASSERT_TRUE(format_rtf_import((const u8 *)"{\\rtf1\\ansi Hello world\\par}", 29, &doc) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, buf, sizeof(buf), &needed) == WP_OK);
    ASSERT_TRUE(format_rtf_import(buf, needed, &doc2) == WP_OK);

    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 1);
    bytes1 = wp_strpool_get(&doc.strings, wp_paragraph_run_get(wp_section_paragraph_get(sec, 0), 0)->text);
    len1 = wp_paragraph_run_get(wp_section_paragraph_get(sec, 0), 0)->text.length;

    sec = wp_document_section_get(&doc2, 0);
    ASSERT_EQ_U32(sec->para_count, 1);
    bytes2 = wp_strpool_get(&doc2.strings, wp_paragraph_run_get(wp_section_paragraph_get(sec, 0), 0)->text);
    len2 = wp_paragraph_run_get(wp_section_paragraph_get(sec, 0), 0)->text.length;

    ASSERT_EQ_U32(len1, len2);
    ASSERT_MEM_EQ(bytes1, bytes2, len1);

    wp_document_free(&doc);
    wp_document_free(&doc2);
}

/* Builds a document entirely from the SUPPORTED attribute subset
   (docs/formats/rtf.md) via the structural mutation API directly (not
   RTF import), so this proves the export side independently: two
   paragraphs, distinct alignment/indents/spacing, and three runs
   covering every character attribute including color. Exports,
   reimports, and asserts every supported attribute survives exactly --
   the literal "model->RTF->model" AC. */
void test_format_rtf_roundtrip_full_attribute_set(void)
{
    wp_document doc, doc2;
    u8 buf[1024];
    u32 needed;
    wp_section *sec;
    u32 para1_idx, para2_idx;
    wp_paragraph *para1, *para2;
    wp_pstyle pstyle1, pstyle2;
    wp_cstyle cstyle_bold, cstyle_colored, cstyle_plain;
    wp_para_style ps;
    wp_char_style cs;
    wp_span span;
    u16 run_idx;
    u32 sec_idx;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para1_idx) == WP_OK);
    para1 = wp_section_paragraph_get(sec, para1_idx);

    memset(&ps, 0, sizeof(ps));
    ps.align = WP_ALIGN_CENTER;
    ps.indent_left = 720;
    ps.indent_right = 360;
    ps.indent_first = -180;
    ps.space_before = 240;
    ps.space_after = 120;
    ASSERT_TRUE(wp_document_pstyle_add(&doc, &ps, &pstyle1) == WP_OK);
    wp_paragraph_pstyle_set(para1, pstyle1);

    memset(&cs, 0, sizeof(cs));
    cs.flags = (u16)(WP_STYLE_BOLD | WP_STYLE_ITALIC | WP_STYLE_UNDERLINE);
    cs.size_twips = 360;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &cs, &cstyle_bold) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"bold italic underline", 21, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para1, span, cstyle_bold, &run_idx) == WP_OK);

    memset(&cs, 0, sizeof(cs));
    cs.flags = WP_STYLE_STRIKE;
    cs.size_twips = 240;
    cs.color_r = 200; cs.color_g = 40; cs.color_b = 90;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &cs, &cstyle_colored) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"struck and colored", 18, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para1, span, cstyle_colored, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para2_idx) == WP_OK);
    para2 = wp_section_paragraph_get(sec, para2_idx);

    memset(&ps, 0, sizeof(ps));
    ps.align = WP_ALIGN_JUSTIFY;
    ASSERT_TRUE(wp_document_pstyle_add(&doc, &ps, &pstyle2) == WP_OK);
    wp_paragraph_pstyle_set(para2, pstyle2);

    memset(&cs, 0, sizeof(cs));
    cs.size_twips = 240;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &cs, &cstyle_plain) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"second paragraph", 16, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para2, span, cstyle_plain, &run_idx) == WP_OK);

    ASSERT_TRUE(format_rtf_export(&doc, buf, sizeof(buf), &needed) == WP_OK);
    ASSERT_TRUE(format_rtf_import(buf, needed, &doc2) == WP_OK);

    sec = wp_document_section_get(&doc2, 0);
    ASSERT_EQ_U32(sec->para_count, 2);

    {
        wp_paragraph *p = wp_section_paragraph_get(sec, 0);
        const wp_para_style *rps = wp_document_pstyle_get(&doc2, p->style);
        wp_run *r;
        const wp_char_style *rcs;
        u32 len;
        const u8 *bytes;

        ASSERT_EQ_U32(p->run_count, 2);
        ASSERT_EQ_U32(rps->align, WP_ALIGN_CENTER);
        ASSERT_EQ_U32(rps->indent_left, 720);
        ASSERT_EQ_U32(rps->indent_right, 360);
        ASSERT_TRUE(rps->indent_first == -180);
        ASSERT_EQ_U32(rps->space_before, 240);
        ASSERT_EQ_U32(rps->space_after, 120);

        r = wp_paragraph_run_get(p, 0);
        rcs = wp_document_cstyle_get(&doc2, r->style);
        ASSERT_EQ_U32(rcs->flags, WP_STYLE_BOLD | WP_STYLE_ITALIC | WP_STYLE_UNDERLINE);
        ASSERT_EQ_U32(rcs->size_twips, 360);
        len = r->text.length;
        bytes = wp_strpool_get(&doc2.strings, r->text);
        ASSERT_EQ_U32(len, 21);
        ASSERT_MEM_EQ(bytes, "bold italic underline", 21);

        r = wp_paragraph_run_get(p, 1);
        rcs = wp_document_cstyle_get(&doc2, r->style);
        ASSERT_EQ_U32(rcs->flags, WP_STYLE_STRIKE);
        ASSERT_EQ_U32(rcs->color_r, 200);
        ASSERT_EQ_U32(rcs->color_g, 40);
        ASSERT_EQ_U32(rcs->color_b, 90);
        len = r->text.length;
        bytes = wp_strpool_get(&doc2.strings, r->text);
        ASSERT_EQ_U32(len, 18);
        ASSERT_MEM_EQ(bytes, "struck and colored", 18);
    }

    {
        wp_paragraph *p = wp_section_paragraph_get(sec, 1);
        const wp_para_style *rps = wp_document_pstyle_get(&doc2, p->style);
        wp_run *r;
        u32 len;
        const u8 *bytes;

        ASSERT_EQ_U32(rps->align, WP_ALIGN_JUSTIFY);
        r = wp_paragraph_run_get(p, 0);
        len = r->text.length;
        bytes = wp_strpool_get(&doc2.strings, r->text);
        ASSERT_EQ_U32(len, 16);
        ASSERT_MEM_EQ(bytes, "second paragraph", 16);
    }

    wp_document_free(&doc);
    wp_document_free(&doc2);
}
