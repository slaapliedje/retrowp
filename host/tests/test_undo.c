/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include <stdlib.h>
#include "test.h"
#include "editing.h"
#include "format_txt.h"
#include "mem_host.h"

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

/* Locates the (run, offset) pair corresponding to logical byte position
   target_pos within a paragraph's concatenated text -- run layout can
   legitimately vary across edits and undo/redo (content-equivalent, not
   layout-identical -- see docs/editing.md), so tests must never hardcode
   run indices across a sequence of mutations. Prefers the START of the
   next run at an exact boundary (not the end of the current one) so the
   result is always a valid point to insert/delete *from*. */
static void find_pos_in_para(const wp_paragraph *para, u32 target_pos,
                              u16 *out_run, u32 *out_offset)
{
    u16 i;
    u32 consumed = 0;

    for (i = 0; i < para->run_count; i++) {
        u32 len = para->runs[i].text.length;
        if (target_pos < consumed + len || (u16)(i + 1) == para->run_count) {
            *out_run = i;
            *out_offset = target_pos - consumed;
            return;
        }
        consumed += len;
    }
    *out_run = 0;
    *out_offset = 0;
}

static u32 para_text_length(const wp_paragraph *para)
{
    u16 i;
    u32 total = 0;

    for (i = 0; i < para->run_count; i++) total += para->runs[i].text.length;
    return total;
}

void test_undo_insert_then_undo_then_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx, r;
    u32 o;
    wp_pos pos, out_pos;
    char buf[64];

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    find_pos_in_para(para, 5, &r, &o);
    pos.section = 0; pos.paragraph = 0; pos.run = r; pos.offset = o;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, pos, (const u8 *)",", 1, &out_pos) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello, World");

    ASSERT_TRUE(wp_undo_can_undo(&journal));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    ASSERT_TRUE(wp_undo_can_redo(&journal));
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello, World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Exercises all four structural delete cases (whole/left-partial/
   right-partial/split), each followed by undo then redo, verifying
   content at every step via dynamic position lookup. */
void test_undo_delete_all_cases_then_undo_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx, r;
    u32 o;
    char buf[64];

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    /* split: delete the space in the middle */
    find_pos_in_para(para, 5, &r, &o);
    ASSERT_TRUE(wp_edit_delete_range(&doc, &journal, 0, 0, r, o, o + 1) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloWorld");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloWorld");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);

    /* left-partial: delete "Hello " from the start */
    find_pos_in_para(para, 0, &r, &o);
    ASSERT_TRUE(wp_edit_delete_range(&doc, &journal, 0, 0, r, 0, 6) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "World");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "World");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);

    /* right-partial: delete " World" from the end */
    {
        u32 len = para_text_length(para);
        find_pos_in_para(para, 5, &r, &o);
        ASSERT_TRUE(wp_edit_delete_range(&doc, &journal, 0, 0, r, o, o + (len - 5)) == WP_OK);
    }
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);

    /* whole run removed */
    find_pos_in_para(para, 0, &r, &o);
    {
        u32 len = para_text_length(para);
        ASSERT_TRUE(wp_edit_delete_range(&doc, &journal, 0, 0, r, 0, len) == WP_OK);
    }
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_undo_new_edit_clears_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx, r;
    u32 o;
    wp_pos pos, out_pos;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hi", 2, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    find_pos_in_para(para, 0, &r, &o);
    pos.section = 0; pos.paragraph = 0; pos.run = r; pos.offset = o;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, pos, (const u8 *)"X", 1, &out_pos) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(wp_undo_can_redo(&journal));

    find_pos_in_para(para, 0, &r, &o);
    pos.run = r; pos.offset = o;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, pos, (const u8 *)"Y", 1, &out_pos) == WP_OK);
    ASSERT_TRUE(!wp_undo_can_redo(&journal));

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_undo_empty_journal_returns_error(void)
{
    wp_document doc;
    wp_undo_journal journal;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);

    ASSERT_TRUE(!wp_undo_can_undo(&journal));
    ASSERT_TRUE(!wp_undo_can_redo(&journal));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_ERR);
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_ERR);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Leak check (mirrors the T1.1 AC pattern): a mix of inserts, deletes,
   partial undo, and redo leaves both the undo and redo stacks non-empty
   with live wp_op nodes -- wp_undo_journal_free must reclaim all of
   them, and wp_document_free must reclaim everything else, with nothing
   left over. */
void test_undo_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx, r;
    u32 o;
    wp_pos pos, out_pos;

    mem_host_reset_counters();

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    find_pos_in_para(para, 5, &r, &o);
    pos.section = 0; pos.paragraph = 0; pos.run = r; pos.offset = o;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, pos, (const u8 *)",", 1, &out_pos) == WP_OK);

    find_pos_in_para(para, 0, &r, &o);
    ASSERT_TRUE(wp_edit_delete_range(&doc, &journal, 0, 0, r, 0, 1) == WP_OK); /* delete "H" */

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK); /* undo the delete -- redo stack now has 1 */
    /* leave 1 entry on the undo stack (the insert) and 1 on redo (the delete) */

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}

/* T2.3 AC: random-op fuzz test -- after N ops then N undos, the model
   equals the start state. Verified via format_txt_export, which is a
   valid "deep compare" oracle for this milestone's op set (insert/
   delete never touch cstyle/pstyle -- see docs/editing.md). Fixed seed
   for a deterministic, reproducible test run. */
void test_undo_fuzz_n_ops_then_n_undos_restores_original(void)
{
    static const char *text =
        "First paragraph has some words in it.\n"
        "Second paragraph is a bit different.\n"
        "Third one is short.\n"
        "Fourth and final paragraph closes things out.\n";
    wp_document doc;
    wp_undo_journal journal;
    u8 original[2048];
    u32 original_len;
    u8 after[2048];
    u32 after_len;
    int n_ops = 60;
    int i;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_undo_journal_init(&journal);

    ASSERT_TRUE(format_txt_export(&doc, original, sizeof(original), &original_len) == WP_OK);

    srand(12345u);

    for (i = 0; i < n_ops; i++) {
        wp_section *sec = wp_document_section_get(&doc, 0);
        u32 para_idx = (u32)(rand() % (int)sec->para_count);
        wp_paragraph *para = wp_section_paragraph_get(sec, para_idx);
        u32 len = para_text_length(para);
        int do_insert = (rand() % 2 == 0) || len == 0;

        if (do_insert) {
            u32 target = (len == 0) ? 0 : (u32)(rand() % (int)(len + 1));
            u16 r; u32 o;
            wp_pos pos, out_pos;
            u8 letter = (u8)('a' + (rand() % 26));

            find_pos_in_para(para, target, &r, &o);
            pos.section = 0; pos.paragraph = para_idx; pos.run = r; pos.offset = o;
            ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, pos, &letter, 1, &out_pos) == WP_OK);
        } else {
            u32 a = (u32)(rand() % (int)len);
            u32 b = (u32)(rand() % (int)len);
            u32 start = (a < b) ? a : b;
            u32 end = (a < b) ? b : a;
            u16 r; u32 o;

            if (start == end) end = start + 1; /* ensure a non-empty range */
            find_pos_in_para(para, start, &r, &o);
            /* start/end must land within the SAME run for this
               milestone's single-run delete scope (docs/editing.md) --
               clamp end to this run's remaining length if needed. */
            if (o + (end - start) > para->runs[r].text.length) {
                end = start + (para->runs[r].text.length - o);
            }
            if (end > start) {
                ASSERT_TRUE(wp_edit_delete_range(&doc, &journal, 0, para_idx, r,
                                                  o, o + (end - start)) == WP_OK);
            }
        }
    }

    for (i = 0; i < n_ops; i++) {
        ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    }

    ASSERT_TRUE(!wp_undo_can_undo(&journal));
    ASSERT_TRUE(format_txt_export(&doc, after, sizeof(after), &after_len) == WP_OK);
    ASSERT_EQ_U32(after_len, original_len);
    ASSERT_MEM_EQ(after, original, original_len);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_undo_split_paragraph_then_undo_then_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_pos pos, out_pos;
    char buf[64];

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_edit_split_paragraph(&doc, &journal, pos, &out_pos) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);
    ASSERT_EQ_U32(out_pos.paragraph, 1);

    ASSERT_TRUE(wp_undo_can_undo(&journal));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    ASSERT_TRUE(wp_undo_can_redo(&journal));
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");
    para = wp_section_paragraph_get(sec, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, " World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Verifies the one genuinely tricky correctness detail from
   docs/editing.md's field-reuse design for WP_OP_MERGE_PARA: the
   absorbed paragraph's OWN pstyle (distinct from the survivor's) must
   come back after undo, not the survivor's style that a naive re-split
   would default to. */
void test_undo_merge_paragraph_then_undo_then_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para0_idx, para1_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_pstyle center_style;
    wp_para_style ps;
    wp_pos out_pos;
    char buf[64];

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para0_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para0_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para1_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para1_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"World", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    /* Give the SECOND paragraph a distinct style from the (default-
       styled) first, so undo restoring the wrong style is observable. */
    memset(&ps, 0, sizeof(ps));
    ps.align = WP_ALIGN_CENTER;
    ASSERT_TRUE(wp_document_pstyle_add(&doc, &ps, &center_style) == WP_OK);
    wp_paragraph_pstyle_set(para, center_style);

    ASSERT_TRUE(wp_edit_merge_paragraph(&doc, &journal, 0, 0, &out_pos) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloWorld");

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 2);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");
    para = wp_section_paragraph_get(sec, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "World");
    ASSERT_EQ_U32(para->style, center_style); /* the real correctness check */

    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloWorld");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_undo_split_merge_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_pos pos, out_pos;

    mem_host_reset_counters();

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_edit_split_paragraph(&doc, &journal, pos, &out_pos) == WP_OK);
    ASSERT_TRUE(wp_edit_merge_paragraph(&doc, &journal, 0, 0, NULL) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK); /* undo the merge -- redo stack now has 1 */

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}

/* ------------------------------------------------------------------ */
/* The unsaved-changes flag                                             */
/* ------------------------------------------------------------------ */

/* The boundary that makes this flag mean anything: a document that came
   out of an IMPORTER is not modified -- it is exactly the file it was
   built from -- while a document a user has edited is. Importers use the
   raw structural functions; user edits go through the journaled
   wp_edit_* family. Get this backwards and either every freshly opened
   file nags on exit, or real unsaved work is discarded without a word. */
void test_document_import_is_not_modified(void)
{
    static const char text[] = "Imported from disk.\n";
    wp_document doc;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)(sizeof(text) - 1), &doc) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_FALSE);

    wp_document_free(&doc);
}

void test_journaled_edit_marks_document_modified(void)
{
    static const char text[] = "Hello\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_pos at;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)(sizeof(text) - 1), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_FALSE);

    at.section = 0; at.paragraph = 0; at.run = 0; at.offset = 0;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, at, (const u8 *)"X", 1, NULL) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_TRUE);

    /* A save makes it match disk again... */
    wp_document_mark_saved(&doc);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_FALSE);

    /* ...and the next edit dirties it again. */
    at.section = 0; at.paragraph = 0; at.run = 0; at.offset = 0;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, at, (const u8 *)"Y", 1, NULL) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_TRUE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* A rejected undo changed nothing, so it must not dirty a clean
   document -- otherwise pressing u on a freshly opened file would invent
   unsaved changes out of nothing and prompt on the way out. */
void test_failed_undo_does_not_mark_modified(void)
{
    static const char text[] = "Untouched.\n";
    wp_document doc;
    wp_undo_journal journal;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)(sizeof(text) - 1), &doc) == WP_OK);
    wp_undo_journal_init(&journal);

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_ERR);   /* nothing to undo */
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_FALSE);

    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_ERR);   /* nothing to redo */
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_FALSE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* A real undo DOES mark modified, even though it may have landed back on
   exactly what is saved. Documented conservatism, pinned so nobody
   "fixes" it into silence without deciding to: the journal has no record
   of which node was the save point. */
void test_real_undo_marks_modified(void)
{
    static const char text[] = "Hello\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_pos at;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)(sizeof(text) - 1), &doc) == WP_OK);
    wp_undo_journal_init(&journal);

    at.section = 0; at.paragraph = 0; at.run = 0; at.offset = 0;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, at, (const u8 *)"X", 1, NULL) == WP_OK);
    wp_document_mark_saved(&doc);

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc) == WP_TRUE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

static void doc_text(const wp_document *doc, char *out, u32 cap)
{
    u32 len = 0;
    if (format_txt_export(doc, (u8 *)out, cap - 1, &len) != WP_OK) len = 0;
    out[len] = '\0';
}

/* An insert in the middle of a run, failing at every allocation it
   makes in turn: the paragraph is either as it was or fully edited,
   never short of its tail (the run used to be cut first). */
void test_undo_insert_mid_run_out_of_memory_keeps_the_text(void)
{
    i32 n;

    for (n = 0; n < 12; n++) {
        wp_document doc;
        wp_undo_journal journal;
        wp_pos at;
        wp_status st;
        char text[64];

        ASSERT_TRUE(format_txt_import((const u8 *)"Hello world\n", 12, &doc) == WP_OK);
        wp_undo_journal_init(&journal);
        at.section = 0; at.paragraph = 0; at.run = 0; at.offset = 2;
        mem_host_fail_after(n);
        st = wp_edit_insert_text(&doc, &journal, at, (const u8 *)"XX", 2, NULL);
        mem_host_fail_after(-1);
        doc_text(&doc, text, sizeof(text));
        if (st == WP_OK) {
            ASSERT_EQ_STR(text, "HeXXllo world\n");
        } else {
            ASSERT_EQ_STR(text, "Hello world\n");
        }
        wp_undo_journal_free(&journal);
        wp_document_free(&doc);
    }
}

/* Group markers when memory runs out: whichever allocation fails, Undo
   stays on the stack (it walked past the bottom and crashed when a
   BEGIN was lost and its END was not), and every edit comes undone. */
void test_undo_group_out_of_memory_never_walks_off_the_stack(void)
{
    i32 n;

    for (n = 0; n < 3; n++) {
        wp_document doc;
        wp_undo_journal journal;
        wp_pos at;
        char text[64];

        ASSERT_TRUE(format_txt_import((const u8 *)"Hello\n", 6, &doc) == WP_OK);
        wp_undo_journal_init(&journal);
        at.section = 0; at.paragraph = 0; at.run = 0; at.offset = 5;
        ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, at, (const u8 *)"!", 1, NULL) == WP_OK);

        mem_host_fail_after(n);
        wp_undo_group_begin(&journal);
        mem_host_fail_after(-1);
        at.offset = 0;
        ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, at, (const u8 *)"A", 1, NULL) == WP_OK);
        wp_undo_group_end(&journal);
        ASSERT_EQ_U32(journal.group_depth, 0);
        ASSERT_EQ_U32(journal.lost_from, 0);

        while (wp_undo_can_undo(&journal)) ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
        doc_text(&doc, text, sizeof(text));
        ASSERT_EQ_STR(text, "Hello\n");
        wp_undo_journal_free(&journal);
        wp_document_free(&doc);
    }
}

static void type_str(wp_document *doc, wp_undo_journal *journal, wp_pos *cursor, const char *s)
{
    for (; *s != '\0'; s++) {
        wp_pos out;
        ASSERT_TRUE(wp_edit_type_text(doc, journal, *cursor, (const u8 *)s, 1, &out) == WP_OK);
        *cursor = out;
    }
}

static wp_paragraph *one_para_doc(wp_document *doc, wp_undo_journal *journal, const char *text)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    if (wp_document_init(doc) != WP_OK) return NULL;
    wp_undo_journal_init(journal);
    if (wp_document_section_add(doc, &sec_idx) != WP_OK) return NULL;
    sec = wp_document_section_get(doc, sec_idx);
    if (wp_section_paragraph_add(sec, &para_idx) != WP_OK) return NULL;
    para = wp_section_paragraph_get(sec, para_idx);
    if (wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) != WP_OK) return NULL;
    if (wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) != WP_OK) return NULL;
    return para;
}

/* Typing makes the run before the caret longer, not a run per key (a
   recovery file was one RTF group per keystroke, and the ST paid ten
   bytes of run for each byte typed); each key still undoes and redoes
   on its own. */
void test_undo_typing_lengthens_one_run(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_pos cursor, out;
    char buf[64];
    u32 i;

    para = one_para_doc(&doc, &journal, "Hello");
    ASSERT_TRUE(para != NULL);
    memset(&cursor, 0, sizeof(cursor));
    cursor.offset = 5;
    type_str(&doc, &journal, &cursor, " world");
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello world");
    ASSERT_EQ_U32(para->run_count, 1);
    ASSERT_EQ_U32(cursor.run, 0);
    ASSERT_EQ_U32(cursor.offset, 11);

    /* An insert that is not typing keeps its run of its own: notes.c and
       the armed input style restyle out_pos->run straight after. */
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, cursor, (const u8 *)"!", 1, &out) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 2);
    ASSERT_EQ_U32(out.run, 1);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 1);

    for (i = 0; i < 6; i++) {
        ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
        ASSERT_EQ_U32(para_text_length(para), 10u - i);
    }
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");
    ASSERT_EQ_U32(para->run_count, 1);
    for (i = 0; i < 6; i++) ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello world");
    ASSERT_EQ_U32(para->run_count, 1);

    /* Mid-run, the first key splits as ever; the keys after it lengthen
       the run it made. */
    cursor.run = 0;
    cursor.offset = 2;
    type_str(&doc, &journal, &cursor, "XYZ");
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HeXYZllo world");
    ASSERT_EQ_U32(para->run_count, 3);
    for (i = 0; i < 3; i++) ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello world");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* The armed input style (Bold with nothing selected) restyles only what
   was typed: detach splits it off the run it lengthened. Redo has to
   rebuild the runs the edits journaled after it name -- typed 'c',
   detached, typed 'd' into c's run, all undone and redone. */
void test_undo_detach_typed_then_undo_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_pos cursor;
    char buf[64];

    para = one_para_doc(&doc, &journal, "ab");
    ASSERT_TRUE(para != NULL);
    memset(&cursor, 0, sizeof(cursor));
    cursor.offset = 2;
    type_str(&doc, &journal, &cursor, "c");
    ASSERT_EQ_U32(para->run_count, 1);
    ASSERT_TRUE(wp_edit_detach_typed(&doc, &journal, &cursor) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 2);
    ASSERT_EQ_U32(para->runs[0].text.length, 2);
    ASSERT_EQ_U32(para->runs[1].text.length, 1);
    ASSERT_EQ_U32(cursor.run, 1);
    ASSERT_EQ_U32(cursor.offset, 1);
    /* Already alone: nothing more. */
    ASSERT_TRUE(wp_edit_detach_typed(&doc, &journal, &cursor) == WP_OK);
    ASSERT_EQ_U32(para->run_count, 2);

    type_str(&doc, &journal, &cursor, "d");
    ASSERT_EQ_U32(para->run_count, 2);
    ASSERT_EQ_U32(para->runs[1].text.length, 2);

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "ab");
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "abcd");
    ASSERT_EQ_U32(para->run_count, 2);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}
