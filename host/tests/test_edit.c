#include <string.h>
#include "test.h"
#include "editing.h"

/* Builds a one-section, one-paragraph document with a single run
   containing `text`, and returns the paragraph pointer via *out_para. */
static void build_single_run_doc(wp_document *doc, const char *text,
                                  wp_paragraph **out_para)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    *out_para = wp_section_paragraph_get(sec, para_idx);

    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)text,
                                   (u32)strlen(text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(*out_para, span, doc->default_cstyle, &run_idx) == WP_OK);
}

static void dump_para_text(wp_document *doc, wp_paragraph *para, char *out)
{
    u16 i;
    u32 pos = 0;

    for (i = 0; i < para->run_count; i++) {
        const u8 *b = wp_strpool_get(&doc->strings, para->runs[i].text);
        memcpy(out + pos, b, para->runs[i].text.length);
        pos += para->runs[i].text.length;
    }
    out[pos] = '\0';
}

void test_edit_insert_at_start_of_paragraph(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "World", &para);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 0;
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, pos, (const u8 *)"Hello ", 6, &out_pos) == WP_OK);

    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_EQ_U32(out_pos.offset, 6);

    wp_document_free(&doc);
}

void test_edit_insert_at_end_of_paragraph(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello", &para);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5; /* end of "Hello" */
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, pos, (const u8 *)" World", 6, &out_pos) == WP_OK);

    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_document_free(&doc);
}

void test_edit_insert_mid_run_splits_correctly(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5; /* right after "Hello" */
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, pos, (const u8 *)",", 1, &out_pos) == WP_OK);

    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello, World");
    /* mid-run split produces exactly 3 runs: left, inserted, right */
    ASSERT_EQ_U32(para->run_count, 3);

    wp_document_free(&doc);
}

void test_edit_insert_into_empty_paragraph(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_pos pos, out_pos;
    char buf[64];

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 0;
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, pos, (const u8 *)"Hi", 2, &out_pos) == WP_OK);

    ASSERT_EQ_U32(para->run_count, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hi");
    ASSERT_EQ_U32(para->runs[0].style, doc.default_cstyle);

    wp_document_free(&doc);
}

void test_edit_insert_style_inheritance_at_boundary(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_char_style bold_style;
    wp_cstyle bold;
    wp_span span;
    u16 run_idx;
    wp_pos pos, out_pos;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    bold_style = *wp_document_cstyle_get(&doc, doc.default_cstyle);
    bold_style.flags = WP_STYLE_BOLD;
    ASSERT_TRUE(wp_document_cstyle_add(&doc, &bold_style, &bold) == WP_OK);

    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);

    /* run 0 = bold "Bold", run 1 = default "Plain" */
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Bold", 4, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, bold, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Plain", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    /* Inserting exactly at the boundary (run=1, offset=0) must inherit
       the PRECEDING run's style (bold), not the following run's. */
    pos.section = 0; pos.paragraph = 0; pos.run = 1; pos.offset = 0;
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, pos, (const u8 *)"X", 1, &out_pos) == WP_OK);

    ASSERT_EQ_U32(para->runs[1].style, bold); /* the new run inherits bold */

    wp_document_free(&doc);
}

void test_edit_delete_whole_run(void)
{
    wp_document doc;
    wp_paragraph *para;
    char buf[64];

    build_single_run_doc(&doc, "Hello", &para);

    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 0, 5) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "");

    wp_document_free(&doc);
}

void test_edit_delete_left_partial(void)
{
    wp_document doc;
    wp_paragraph *para;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);

    /* delete "Hello " (0..6), keep "World" */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 0, 6) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "World");

    wp_document_free(&doc);
}

void test_edit_delete_right_partial(void)
{
    wp_document doc;
    wp_paragraph *para;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);

    /* delete " World" (5..11), keep "Hello" */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 5, 11) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");

    wp_document_free(&doc);
}

void test_edit_delete_mid_run_splits(void)
{
    wp_document doc;
    wp_paragraph *para;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);

    /* delete " " (5..6), leaving "Hello" and "World" as two runs */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 5, 6) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 2);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloWorld");

    wp_document_free(&doc);
}

void test_edit_delete_rejects_invalid_range(void)
{
    wp_document doc;
    wp_paragraph *para;

    build_single_run_doc(&doc, "Hello", &para);

    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 3, 3) == WP_RANGE);  /* empty range */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 4, 3) == WP_RANGE);  /* start > end */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 0, 0, 99) == WP_RANGE); /* past run length */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, 5, 0, 1) == WP_RANGE);  /* bad run index */

    wp_document_free(&doc);
}

void test_edit_insert_then_delete_restores_content(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, pos, (const u8 *)",", 1, &out_pos) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello, World");

    /* Undo by hand: delete exactly the run insert created. */
    ASSERT_TRUE(wp_document_delete_range_raw(&doc, 0, 0, out_pos.run, 0, 1) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_document_free(&doc);
}

void test_edit_split_paragraph_mid_run(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_section *sec;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);
    sec = wp_document_section_get(&doc, 0);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_document_split_paragraph_raw(&doc, pos, &out_pos) == WP_OK);

    ASSERT_EQ_U32(out_pos.paragraph, 1);
    ASSERT_EQ_U32(out_pos.run, 0);
    ASSERT_EQ_U32(out_pos.offset, 0);
    ASSERT_EQ_U32(sec->para_count, 2);

    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");

    para = wp_section_paragraph_get(sec, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, " World");

    wp_document_free(&doc);
}

void test_edit_split_paragraph_at_start(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_section *sec;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello", &para);
    sec = wp_document_section_get(&doc, 0);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 0;
    ASSERT_TRUE(wp_document_split_paragraph_raw(&doc, pos, &out_pos) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);

    para = wp_section_paragraph_get(sec, 0);
    ASSERT_EQ_U32(para->run_count, 0); /* original becomes empty */

    para = wp_section_paragraph_get(sec, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello"); /* all content moved down */

    wp_document_free(&doc);
}

void test_edit_split_paragraph_at_end(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_section *sec;
    wp_pos pos, out_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello", &para);
    sec = wp_document_section_get(&doc, 0);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5; /* end of run == end of paragraph */
    ASSERT_TRUE(wp_document_split_paragraph_raw(&doc, pos, &out_pos) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);

    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");

    para = wp_section_paragraph_get(sec, 1);
    ASSERT_EQ_U32(para->run_count, 0); /* new paragraph empty */

    wp_document_free(&doc);
}

void test_edit_split_paragraph_of_empty_paragraph(void)
{
    wp_document doc;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_pos pos, out_pos;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 0;
    ASSERT_TRUE(wp_document_split_paragraph_raw(&doc, pos, &out_pos) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);
    para = wp_section_paragraph_get(sec, 0);
    ASSERT_EQ_U32(para->run_count, 0);
    para = wp_section_paragraph_get(sec, 1);
    ASSERT_EQ_U32(para->run_count, 0);

    wp_document_free(&doc);
}

void test_edit_merge_paragraph_basic(void)
{
    wp_document doc;
    wp_section *sec;
    wp_paragraph *para;
    wp_pos pos, split_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);
    sec = wp_document_section_get(&doc, 0);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_document_split_paragraph_raw(&doc, pos, &split_pos) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);

    ASSERT_TRUE(wp_document_merge_paragraph_raw(&doc, 0, 0, NULL) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 1);

    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_EQ_U32(para->run_count, 2); /* never coalesced back into one run */

    wp_document_free(&doc);
}

void test_edit_merge_paragraph_rejects_last_paragraph(void)
{
    wp_document doc;
    wp_paragraph *para;

    build_single_run_doc(&doc, "Hello", &para);

    ASSERT_TRUE(wp_document_merge_paragraph_raw(&doc, 0, 0, NULL) == WP_RANGE);
    ASSERT_TRUE(wp_document_merge_paragraph_raw(&doc, 0, 5, NULL) == WP_RANGE); /* bad index too */

    wp_document_free(&doc);
}

void test_edit_split_then_merge_roundtrips(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_section *sec;
    wp_pos pos, split_pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);
    sec = wp_document_section_get(&doc, 0);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_document_split_paragraph_raw(&doc, pos, &split_pos) == WP_OK);
    ASSERT_TRUE(wp_document_merge_paragraph_raw(&doc, 0, 0, NULL) == WP_OK);

    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_document_free(&doc);
}
