/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "docmodel.h"
#include "editing.h"
#include "mem_host.h"

/* M15 Phase 1: the document-model image side table, wp_edit_insert_image,
   and its undo/redo round trip. New file rather than additions to
   test_docmodel.c/test_edit.c/test_undo.c so no pre-existing test file
   needs to change (this milestone's own regression-safety bar -- see
   docs/milestones.md's M15 section). */

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

static void fill_test_desc(wp_image_desc *desc, i32 width_px, i32 height_px)
{
    memset(desc, 0, sizeof(*desc));
    desc->width_px = width_px;
    desc->height_px = height_px;
    desc->display_width_twips = width_px * 15;  /* arbitrary but deterministic */
    desc->display_height_twips = height_px * 15;
    desc->palette_count = 2;
    desc->palette[0][0] = 0;   desc->palette[0][1] = 0;   desc->palette[0][2] = 0;
    desc->palette[1][0] = 255; desc->palette[1][1] = 255; desc->palette[1][2] = 255;
}

void test_image_add_get_roundtrip(void)
{
    wp_document doc;
    wp_image_desc desc, out_desc;
    wp_image handle;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    /* handle 0 is always the reserved sentinel, never a real image. */
    ASSERT_TRUE(wp_document_image_get(&doc, 0) == NULL);

    fill_test_desc(&desc, 160, 100);
    ASSERT_TRUE(wp_document_image_add(&doc, &desc, &handle) == WP_OK);
    ASSERT_TRUE(handle != 0);

    out_desc = *wp_document_image_get(&doc, handle);
    ASSERT_EQ_U32(out_desc.width_px, 160);
    ASSERT_EQ_U32(out_desc.height_px, 100);
    ASSERT_EQ_U32(out_desc.display_width_twips, 2400);
    ASSERT_EQ_U32(out_desc.palette_count, 2);
    ASSERT_EQ_U32(out_desc.palette[1][0], 255);

    /* an out-of-range handle must resolve to NULL, same as cstyle/pstyle. */
    ASSERT_TRUE(wp_document_image_get(&doc, (wp_image)(handle + 1)) == NULL);

    wp_document_free(&doc);
}

void test_image_add_survives_growth(void)
{
    wp_document doc;
    wp_image_desc desc;
    wp_image handles[16];
    int i;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);

    for (i = 0; i < 16; i++) {
        fill_test_desc(&desc, 10 + i, 20 + i);
        ASSERT_TRUE(wp_document_image_add(&doc, &desc, &handles[i]) == WP_OK);
    }

    /* Every earlier handle must still resolve correctly after later adds
       forced the table to grow and relocate. */
    for (i = 0; i < 16; i++) {
        const wp_image_desc *got = wp_document_image_get(&doc, handles[i]);
        ASSERT_TRUE(got != NULL);
        ASSERT_EQ_U32(got->width_px, 10 + i);
        ASSERT_EQ_U32(got->height_px, 20 + i);
    }

    wp_document_free(&doc);
}

void test_edit_insert_image_splits_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_section *sec;
    wp_image_desc desc;
    wp_pos pos;
    char buf[64];

    build_single_run_doc(&doc, "Hello World", &para);
    wp_undo_journal_init(&journal);
    sec = wp_document_section_get(&doc, 0);

    fill_test_desc(&desc, 80, 40);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_edit_insert_image(&doc, &journal, pos, &desc) == WP_OK);

    ASSERT_EQ_U32(sec->para_count, 3);

    para = wp_section_paragraph_get(sec, 0);
    ASSERT_EQ_U32(para->kind, WP_PARA_TEXT);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");

    para = wp_section_paragraph_get(sec, 1);
    ASSERT_EQ_U32(para->kind, WP_PARA_IMAGE);
    ASSERT_EQ_U32(para->run_count, 0);
    ASSERT_TRUE(wp_document_image_get(&doc, para->image) != NULL);
    ASSERT_EQ_U32(wp_document_image_get(&doc, para->image)->width_px, 80);

    para = wp_section_paragraph_get(sec, 2);
    ASSERT_EQ_U32(para->kind, WP_PARA_TEXT);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, " World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_edit_insert_image_undo_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_section *sec;
    wp_image_desc desc;
    wp_pos pos;
    char buf[64];

    mem_host_reset_counters();

    build_single_run_doc(&doc, "Hello World", &para);
    wp_undo_journal_init(&journal);
    sec = wp_document_section_get(&doc, 0);

    fill_test_desc(&desc, 80, 40);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_edit_insert_image(&doc, &journal, pos, &desc) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 3);

    ASSERT_TRUE(wp_undo_can_undo(&journal));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    ASSERT_EQ_U32(para->kind, WP_PARA_TEXT);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    /* the registered descriptor itself must survive undo -- doc->images
       is append-only, same as cstyles/pstyles; only the paragraph
       referencing it goes away. */
    ASSERT_EQ_U32(doc.image_count, 2); /* sentinel + the one registered */

    ASSERT_TRUE(wp_undo_can_redo(&journal));
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 3);

    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");

    para = wp_section_paragraph_get(sec, 1);
    ASSERT_EQ_U32(para->kind, WP_PARA_IMAGE);
    ASSERT_TRUE(wp_document_image_get(&doc, para->image) != NULL);
    ASSERT_EQ_U32(wp_document_image_get(&doc, para->image)->width_px, 80);

    para = wp_section_paragraph_get(sec, 2);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, " World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
