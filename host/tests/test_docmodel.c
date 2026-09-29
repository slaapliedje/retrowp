/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "docmodel.h"
#include "mem_host.h"

void test_docmodel_init_free_has_no_leaks(void)
{
    wp_document doc;

    mem_host_reset_counters();
    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_document_free(&doc);
    ASSERT_EQ_U32(mem_host_live_count(), 0);
}

void test_docmodel_init_sets_usable_defaults(void)
{
    wp_document doc;
    const wp_char_style *cs;
    const wp_para_style *ps;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    /* index 0 is the reserved sentinel in both tables -- init must not
       hand it out as a usable default. */
    ASSERT_TRUE(doc.default_cstyle != 0);
    ASSERT_TRUE(doc.default_pstyle != 0);

    cs = wp_document_cstyle_get(&doc, doc.default_cstyle);
    ASSERT_TRUE(cs != NULL);
    ASSERT_EQ_U32(cs->size_twips, 240);

    ps = wp_document_pstyle_get(&doc, doc.default_pstyle);
    ASSERT_TRUE(ps != NULL);
    ASSERT_EQ_U32(ps->align, WP_ALIGN_LEFT);

    /* handle 0 must always resolve to NULL, never a real style. */
    ASSERT_TRUE(wp_document_cstyle_get(&doc, 0) == NULL);
    ASSERT_TRUE(wp_document_pstyle_get(&doc, 0) == NULL);

    wp_document_free(&doc);
}

void test_docmodel_cstyle_add_survives_growth(void)
{
    wp_document doc;
    wp_char_style style;
    wp_cstyle handles[16];
    int i;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    for (i = 0; i < 16; i++) {
        style = *wp_document_cstyle_get(&doc, doc.default_cstyle);
        style.size_twips = 100 + i;
        ASSERT_TRUE(wp_document_cstyle_add(&doc, &style, &handles[i]) == WP_OK);
    }

    /* Every earlier handle must still resolve correctly after later adds
       forced the table to grow and relocate. */
    for (i = 0; i < 16; i++) {
        const wp_char_style *got = wp_document_cstyle_get(&doc, handles[i]);
        ASSERT_TRUE(got != NULL);
        ASSERT_EQ_U32(got->size_twips, 100 + i);
    }

    wp_document_free(&doc);
}

void test_docmodel_cstyle_find_or_add_reuses_identical_style(void)
{
    wp_document doc;
    wp_char_style style;
    wp_cstyle first, second, third;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    style = *wp_document_cstyle_get(&doc, doc.default_cstyle);
    style.flags |= WP_STYLE_BOLD;

    ASSERT_TRUE(wp_document_cstyle_find_or_add(&doc, &style, &first) == WP_OK);
    ASSERT_TRUE(first != doc.default_cstyle);

    /* Same field values, byte-for-byte, but a distinct local struct --
       must resolve to the SAME handle, not a new table entry (this is
       the whole point: repeated toggling must not grow the table). */
    ASSERT_TRUE(wp_document_cstyle_find_or_add(&doc, &style, &second) == WP_OK);
    ASSERT_EQ_U32(second, first);
    ASSERT_EQ_U32(doc.cstyle_count, 3); /* sentinel + default + this one, no dup */

    style.flags |= WP_STYLE_ITALIC;
    ASSERT_TRUE(wp_document_cstyle_find_or_add(&doc, &style, &third) == WP_OK);
    ASSERT_TRUE(third != first);
    ASSERT_EQ_U32(doc.cstyle_count, 4);

    wp_document_free(&doc);
}

void test_docmodel_pstyle_find_or_add_reuses_identical_style(void)
{
    wp_document doc;
    wp_para_style style;
    wp_pstyle first, second;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    style = *wp_document_pstyle_get(&doc, doc.default_pstyle);
    style.align = WP_ALIGN_CENTER;

    ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &style, &first) == WP_OK);
    ASSERT_TRUE(first != doc.default_pstyle);

    ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &style, &second) == WP_OK);
    ASSERT_EQ_U32(second, first);
    ASSERT_EQ_U32(doc.pstyle_count, 3);

    wp_document_free(&doc);
}

void test_docmodel_build_known_tree(void)
{
    wp_document doc;
    u32 sec_idx, para0_idx, para1_idx;
    wp_section *sec;
    wp_paragraph *para0, *para1;
    u16 run_idx;
    wp_span span;
    wp_run *run;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    ASSERT_EQ_U32(sec_idx, 0);
    ASSERT_EQ_U32(doc.section_count, 1);

    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(sec != NULL);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para0_idx) == WP_OK);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para1_idx) == WP_OK);
    ASSERT_EQ_U32(para0_idx, 0);
    ASSERT_EQ_U32(para1_idx, 1);
    ASSERT_EQ_U32(sec->para_count, 2);

    para0 = wp_section_paragraph_get(sec, para0_idx);
    para1 = wp_section_paragraph_get(sec, para1_idx);
    ASSERT_TRUE(para0 != NULL && para1 != NULL);

    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello, ", 7, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para0, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_EQ_U32(run_idx, 0);

    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"world!", 6, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para0, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_EQ_U32(run_idx, 1);
    ASSERT_EQ_U32(para0->run_count, 2);

    run = wp_paragraph_run_get(para0, 1);
    ASSERT_TRUE(run != NULL);
    ASSERT_MEM_EQ(wp_strpool_get(&doc.strings, run->text), "world!", 6);

    /* Second paragraph stays empty -- confirms paragraphs are independent. */
    ASSERT_EQ_U32(para1->run_count, 0);
    ASSERT_TRUE(wp_paragraph_run_get(para1, 0) == NULL);

    wp_document_free(&doc);
}

void test_docmodel_pstyle_and_cstyle_set(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_para_style ps_def;
    wp_char_style cs_def;
    wp_pstyle heading_ps;
    wp_cstyle bold_cs;
    wp_span span;
    u16 run_idx;
    wp_run *run;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    ps_def = *wp_document_pstyle_get(&doc, doc.default_pstyle);
    ps_def.align = WP_ALIGN_CENTER;
    ASSERT_TRUE(wp_document_pstyle_add(&doc, &ps_def, &heading_ps) == WP_OK);

    cs_def = *wp_document_cstyle_get(&doc, doc.default_cstyle);
    cs_def.flags = WP_STYLE_BOLD;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &cs_def, &bold_cs) == WP_OK);

    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    ASSERT_EQ_U32(para->layout_dirty, WP_FALSE);
    wp_paragraph_pstyle_set(para, heading_ps);
    ASSERT_EQ_U32(para->style, heading_ps);
    ASSERT_EQ_U32(para->layout_dirty, WP_TRUE);

    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Title", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    run = wp_paragraph_run_get(para, run_idx);

    /* run_add above already dirtied the paragraph; clear it so this
       asserts wp_run_cstyle_set's OWN invalidation, not a leftover. */
    para->layout_dirty = WP_FALSE;
    wp_run_cstyle_set(para, run_idx, bold_cs);
    ASSERT_EQ_U32(run->style, bold_cs);
    ASSERT_EQ_U32(para->layout_dirty, WP_TRUE);

    /* Out-of-range index is a no-op, not a wild write. */
    para->layout_dirty = WP_FALSE;
    wp_run_cstyle_set(para, (u16)(para->run_count + 5), bold_cs);
    ASSERT_EQ_U32(para->layout_dirty, WP_FALSE);

    wp_document_free(&doc);
}
