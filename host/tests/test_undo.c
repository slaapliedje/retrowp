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
