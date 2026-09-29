/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "editing.h"
#include "mem_host.h"

/* wp_selection's first real consumers (M8.5) -- wp_pos_compare,
   wp_document_extract_text, and wp_edit_delete_selection. Builds a
   two-paragraph document ("Hello World" / "Goodbye Moon") shared by
   most cases below. */
static void build_two_para_doc(wp_document *doc)
{
    u32 sec_idx, p0, p1;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &p0) == WP_OK);
    para = wp_section_paragraph_get(sec, p0);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"Hello World", 11, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &p1) == WP_OK);
    para = wp_section_paragraph_get(sec, p1);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"Goodbye Moon", 12, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
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

void test_selection_pos_compare_orders_correctly(void)
{
    wp_pos a, b;

    a.section = 0; a.paragraph = 0; a.run = 0; a.offset = 3;
    b.section = 0; b.paragraph = 0; b.run = 0; b.offset = 5;
    ASSERT_TRUE(wp_pos_compare(a, b) < 0);
    ASSERT_TRUE(wp_pos_compare(b, a) > 0);
    ASSERT_TRUE(wp_pos_compare(a, a) == 0);

    b.paragraph = 1; b.offset = 0; /* later paragraph outranks a smaller offset in an earlier one */
    ASSERT_TRUE(wp_pos_compare(a, b) < 0);
}

void test_selection_extract_text_single_paragraph(void)
{
    wp_document doc;
    wp_selection sel;
    u8 buf[64];
    u32 len;

    build_two_para_doc(&doc);

    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 0;
    sel.caret.section = 0; sel.caret.paragraph = 0; sel.caret.run = 0; sel.caret.offset = 5;
    ASSERT_TRUE(wp_document_extract_text(&doc, sel, buf, sizeof(buf), &len) == WP_OK);
    ASSERT_EQ_U32(len, 5);
    buf[len] = '\0';
    ASSERT_EQ_STR((const char *)buf, "Hello");

    /* order-independence: swap anchor/caret, same result */
    { wp_pos t = sel.anchor; sel.anchor = sel.caret; sel.caret = t; }
    ASSERT_TRUE(wp_document_extract_text(&doc, sel, buf, sizeof(buf), &len) == WP_OK);
    ASSERT_EQ_U32(len, 5);

    wp_document_free(&doc);
}

void test_selection_extract_text_multi_paragraph(void)
{
    wp_document doc;
    wp_selection sel;
    u8 buf[64];
    u32 len;

    build_two_para_doc(&doc);

    /* "World" (para 0, from offset 6 to end) through "Goodbye" (para 1, start to offset 7) */
    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 6;
    sel.caret.section = 0; sel.caret.paragraph = 1; sel.caret.run = 0; sel.caret.offset = 7;
    ASSERT_TRUE(wp_document_extract_text(&doc, sel, buf, sizeof(buf), &len) == WP_OK);
    buf[len] = '\0';
    ASSERT_EQ_STR((const char *)buf, "World\nGoodbye");

    wp_document_free(&doc);
}

void test_selection_extract_text_reports_size_on_overflow(void)
{
    wp_document doc;
    wp_selection sel;
    u8 buf[2];
    u32 len;

    build_two_para_doc(&doc);

    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 0;
    sel.caret.section = 0; sel.caret.paragraph = 0; sel.caret.run = 0; sel.caret.offset = 5;
    ASSERT_TRUE(wp_document_extract_text(&doc, sel, buf, sizeof(buf), &len) == WP_RANGE);
    ASSERT_EQ_U32(len, 5); /* reports the real size even though it didn't fit */

    wp_document_free(&doc);
}

void test_selection_delete_single_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_section *sec;
    wp_paragraph *para;
    wp_selection sel;
    wp_pos out_pos;
    char buf[64];

    build_two_para_doc(&doc);
    wp_undo_journal_init(&journal);
    sec = wp_document_section_get(&doc, 0);

    /* delete " World" from paragraph 0 */
    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 5;
    sel.caret.section = 0; sel.caret.paragraph = 0; sel.caret.run = 0; sel.caret.offset = 11;
    ASSERT_TRUE(wp_edit_delete_selection(&doc, &journal, sel, &out_pos) == WP_OK);

    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello");
    ASSERT_EQ_U32(out_pos.paragraph, 0);
    ASSERT_EQ_U32(sec->para_count, 2); /* paragraph 1 untouched */

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_selection_delete_empty_selection_is_noop(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_section *sec;
    wp_paragraph *para;
    wp_selection sel;
    wp_pos out_pos;
    char buf[64];

    build_two_para_doc(&doc);
    wp_undo_journal_init(&journal);
    sec = wp_document_section_get(&doc, 0);

    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 3;
    sel.caret = sel.anchor;
    ASSERT_TRUE(wp_edit_delete_selection(&doc, &journal, sel, &out_pos) == WP_OK);
    ASSERT_TRUE(!wp_undo_can_undo(&journal)); /* no-op pushed nothing */

    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* The real payoff of undo grouping: a delete spanning a whole paragraph
   boundary (merging two paragraphs into one) undoes and redoes as
   exactly ONE wp_edit_undo/wp_edit_redo call, even though it pushed
   several wp_op nodes internally. */
void test_selection_delete_multi_paragraph_then_undo_then_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_section *sec;
    wp_paragraph *para;
    wp_selection sel;
    wp_pos out_pos;
    char buf[64];

    build_two_para_doc(&doc);
    wp_undo_journal_init(&journal);
    sec = wp_document_section_get(&doc, 0);

    /* delete " World\nGoodbye " -- from para0 offset 5 through para1 offset 8 */
    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 5;
    sel.caret.section = 0; sel.caret.paragraph = 1; sel.caret.run = 0; sel.caret.offset = 8;
    ASSERT_TRUE(wp_edit_delete_selection(&doc, &journal, sel, &out_pos) == WP_OK);

    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloMoon");
    ASSERT_EQ_U32(out_pos.paragraph, 0); /* out_pos's exact (run, offset) is checked
                                             separately, in isolation, below --
                                             see test_selection_delete_out_pos_lands_at_delete_start */

    /* ONE undo call restores everything -- both paragraphs, all deleted text. */
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(!wp_undo_can_undo(&journal)); /* the whole group was exactly one step */
    ASSERT_EQ_U32(sec->para_count, 2);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    para = wp_section_paragraph_get(sec, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Goodbye Moon");

    /* ONE redo call reapplies the whole delete again. */
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(!wp_undo_can_redo(&journal));
    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloMoon");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Isolated from the undo/redo test above deliberately -- verifying
   out_pos by inserting at it mutates the document outside the journal,
   which would desync the undo/redo test's own journal-driven
   expectations if done in the same test. Checks USABILITY of out_pos
   (inserting there lands exactly between "Hello" and "Moon") rather
   than a hardcoded (run, offset) pair, since the merged paragraph's
   exact run layout is an implementation detail (see test_undo.c's
   find_pos_in_para comment on why run-array shape isn't part of the
   correctness contract here). */
void test_selection_delete_out_pos_lands_at_delete_start(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_selection sel;
    wp_pos out_pos, insert_out;
    char buf[64];

    build_two_para_doc(&doc);
    wp_undo_journal_init(&journal);

    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 5;
    sel.caret.section = 0; sel.caret.paragraph = 1; sel.caret.run = 0; sel.caret.offset = 8;
    ASSERT_TRUE(wp_edit_delete_selection(&doc, &journal, sel, &out_pos) == WP_OK);

    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(wp_document_insert_text_raw(&doc, out_pos, (const u8 *)"|", 1, &insert_out) == WP_OK);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello|Moon");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Three paragraphs, deleting a range that fully swallows the middle one
   -- exercises the "empty out every whole paragraph strictly between,
   then merge forward" path, not just the two-paragraph adjacent case. */
void test_selection_delete_swallows_whole_middle_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    u32 sec_idx, p2;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_selection sel;
    wp_pos out_pos;
    char buf[64];

    build_two_para_doc(&doc);
    wp_undo_journal_init(&journal);
    sec_idx = 0;
    sec = wp_document_section_get(&doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &p2) == WP_OK);
    para = wp_section_paragraph_get(sec, p2);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Third", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_EQ_U32(sec->para_count, 3);

    /* delete everything from para0 offset 5 through para2 offset 0 --
       swallows all of "Goodbye Moon" (paragraph 1) entirely. */
    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 5;
    sel.caret.section = 0; sel.caret.paragraph = 2; sel.caret.run = 0; sel.caret.offset = 0;
    ASSERT_TRUE(wp_edit_delete_selection(&doc, &journal, sel, &out_pos) == WP_OK);

    ASSERT_EQ_U32(sec->para_count, 1);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "HelloThird");

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(!wp_undo_can_undo(&journal));
    ASSERT_EQ_U32(sec->para_count, 3);
    para = wp_section_paragraph_get(sec, 0);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    para = wp_section_paragraph_get(sec, 1);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Goodbye Moon");
    para = wp_section_paragraph_get(sec, 2);
    dump_para_text(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Third");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_selection_delete_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_selection sel;
    wp_pos out_pos;

    mem_host_reset_counters();

    build_two_para_doc(&doc);
    wp_undo_journal_init(&journal);

    sel.anchor.section = 0; sel.anchor.paragraph = 0; sel.anchor.run = 0; sel.anchor.offset = 5;
    sel.caret.section = 0; sel.caret.paragraph = 1; sel.caret.run = 0; sel.caret.offset = 8;
    ASSERT_TRUE(wp_edit_delete_selection(&doc, &journal, sel, &out_pos) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
