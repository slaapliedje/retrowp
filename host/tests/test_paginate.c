#include <string.h>
#include "test.h"
#include "layout.h"
#include "format_txt.h"
#include "mem_host.h"

void test_paginate_known_page_size_yields_expected_page_count(void)
{
    wp_document doc;
    wp_section *sec;
    wp_pagination pg;
    static const char *text = "one\ntwo\nthree\nfour\nfive\nsix\n"; /* 6 one-line paragraphs */

    ASSERT_TRUE(format_txt_import((const u8 *)text, 28, &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);

    /* Each paragraph is exactly 1 line at height 300 (default cstyle,
       host stub metrics). Content height 600 -> exactly 2 lines/page. */
    sec->page_width = 10000; sec->margin_left = 0; sec->margin_right = 0;
    sec->page_height = 600; sec->margin_top = 0; sec->margin_bottom = 0;

    ASSERT_TRUE(wp_document_paginate(&doc, &pg) == WP_OK);

    ASSERT_EQ_U32(pg.page_count, 3); /* 6 paragraphs, 2/page */
    ASSERT_EQ_U32(pg.pages[0].line_count, 2);
    ASSERT_EQ_U32(pg.pages[1].line_count, 2);
    ASSERT_EQ_U32(pg.pages[2].line_count, 2);
    ASSERT_EQ_U32(pg.pages[0].lines[0].paragraph, 0);
    ASSERT_EQ_U32(pg.pages[0].lines[1].paragraph, 1);
    ASSERT_EQ_U32(pg.pages[1].lines[0].paragraph, 2);
    ASSERT_EQ_U32(pg.pages[2].lines[1].paragraph, 5);

    wp_pagination_free(&pg);
    wp_document_free(&doc);
}

void test_paginate_new_section_always_starts_new_page(void)
{
    wp_document doc;
    u32 sec0_idx, sec1_idx;
    wp_section *sec0, *sec1;
    u32 para_idx;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_pagination pg;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    ASSERT_TRUE(wp_document_section_add(&doc, &sec0_idx) == WP_OK);
    sec0 = wp_document_section_get(&doc, sec0_idx);
    sec0->page_width = 10000; sec0->margin_left = 0; sec0->margin_right = 0;
    sec0->page_height = 10000; sec0->margin_top = 0; sec0->margin_bottom = 0;
    ASSERT_TRUE(wp_section_paragraph_add(sec0, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec0, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"hi", 2, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    wp_paragraph_pstyle_set(para, doc.default_pstyle);

    ASSERT_TRUE(wp_document_section_add(&doc, &sec1_idx) == WP_OK);
    sec1 = wp_document_section_get(&doc, sec1_idx);
    sec1->page_width = 10000; sec1->margin_left = 0; sec1->margin_right = 0;
    sec1->page_height = 10000; sec1->margin_top = 0; sec1->margin_bottom = 0;
    ASSERT_TRUE(wp_section_paragraph_add(sec1, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec1, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"bye", 3, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    wp_paragraph_pstyle_set(para, doc.default_pstyle);

    /* Both sections' single paragraph would easily fit on one shared
       page by height alone (10000 twips of room) -- but a section
       boundary must still force a new page. */
    ASSERT_TRUE(wp_document_paginate(&doc, &pg) == WP_OK);
    ASSERT_EQ_U32(pg.page_count, 2);
    ASSERT_EQ_U32(pg.pages[0].lines[0].section, 0);
    ASSERT_EQ_U32(pg.pages[1].lines[0].section, 1);

    wp_pagination_free(&pg);
    wp_document_free(&doc);
}

void test_paginate_no_leaks(void)
{
    wp_document doc;
    wp_section *sec;
    wp_pagination pg;
    static const char *text = "one\ntwo\nthree\n";

    mem_host_reset_counters();

    ASSERT_TRUE(format_txt_import((const u8 *)text, 14, &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->page_width = 10000; sec->margin_left = 0; sec->margin_right = 0;
    sec->page_height = 400; sec->margin_top = 0; sec->margin_bottom = 0;

    ASSERT_TRUE(wp_document_paginate(&doc, &pg) == WP_OK);

    wp_pagination_free(&pg);
    wp_document_free(&doc); /* frees each paragraph's layout too */

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
