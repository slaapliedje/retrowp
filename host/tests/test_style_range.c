#include <string.h>
#include "test.h"
#include "editing.h"
#include "mem_host.h"

/* Styling an existing selection -- the thing "Ctrl+B with a word
   selected" has to do, and the reason wp_edit_style_range exists at all.
   Before it, WP_OP_SET_CSTYLE was a name in an enum that nothing
   produced and nothing consumed, and every shell's Bold action only
   changed the style of text typed NEXT. */

static void dump_para(wp_document *doc, wp_paragraph *para, char *out)
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

/* The flags actually in force at byte `byte` of a paragraph -- asked by
   byte rather than by run index because styling splits runs, so an index
   that was right before the call means nothing after it. */
static u16 flags_at_byte(wp_document *doc, wp_paragraph *para, u32 byte)
{
    u32 consumed = 0;
    u16 i;

    for (i = 0; i < para->run_count; i++) {
        u32 len = para->runs[i].text.length;
        if (byte < consumed + len) {
            const wp_char_style *cs = wp_document_cstyle_get(doc, para->runs[i].style);
            return cs ? cs->flags : 0;
        }
        consumed += len;
    }
    return 0;
}

static i32 size_at_byte(wp_document *doc, wp_paragraph *para, u32 byte)
{
    u32 consumed = 0;
    u16 i;

    for (i = 0; i < para->run_count; i++) {
        u32 len = para->runs[i].text.length;
        if (byte < consumed + len) {
            const wp_char_style *cs = wp_document_cstyle_get(doc, para->runs[i].style);
            return cs ? cs->size_twips : 0;
        }
        consumed += len;
    }
    return 0;
}

static wp_pos pos_at(u32 section, u32 paragraph, u32 run, u32 offset)
{
    wp_pos p;
    p.section = section; p.paragraph = paragraph; p.run = run; p.offset = offset;
    return p;
}

/* One paragraph, one run, "Hello World". */
static void build_one_run(wp_document *doc, wp_undo_journal *journal,
                           wp_paragraph **out_para, const char *text)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    wp_document_init(doc);
    wp_undo_journal_init(journal);
    wp_document_section_add(doc, &sec_idx);
    sec = wp_document_section_get(doc, sec_idx);
    wp_section_paragraph_add(sec, &para_idx);
    para = wp_section_paragraph_get(sec, para_idx);
    wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span);
    wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx);
    *out_para = para;
}

static wp_style_delta bold_on(void)
{
    wp_style_delta d;
    d.flags_set = WP_STYLE_BOLD; d.flags_clear = 0; d.size_twips = 0;
    return d;
}

void test_style_range_bolds_a_word_in_the_middle(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();
    char buf[64];

    build_one_run(&doc, &journal, &para, "Hello World");

    /* bytes 6..11 -- "World" */
    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 6),
                                     pos_at(0, 0, 0, 11), &delta) == WP_OK);

    dump_para(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");          /* not one byte moved */
    ASSERT_TRUE((flags_at_byte(&doc, para, 0) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 5) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 6) & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 10) & WP_STYLE_BOLD) != 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_splits_the_run_it_partially_covers(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");
    ASSERT_TRUE(para->run_count == 1);

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 6),
                                     pos_at(0, 0, 0, 11), &delta) == WP_OK);

    /* "Hello " and "World" cannot share a style handle, so the one run
       had to become two. */
    ASSERT_TRUE(para->run_count == 2);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_marks_the_paragraph_for_relayout(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");
    para->layout_dirty = WP_FALSE;

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 0),
                                     pos_at(0, 0, 0, 5), &delta) == WP_OK);

    /* Bold is wider than roman: a styled paragraph must re-wrap. */
    ASSERT_TRUE(para->layout_dirty == WP_TRUE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_marks_the_document_modified(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");
    wp_document_mark_saved(&doc);
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 0),
                                     pos_at(0, 0, 0, 5), &delta) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc));

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_undo_restores_every_run_then_redo_reapplies(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();
    char buf[64];

    build_one_run(&doc, &journal, &para, "Hello World");

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 0),
                                     pos_at(0, 0, 0, 11), &delta) == WP_OK);
    ASSERT_TRUE((flags_at_byte(&doc, para, 3) & WP_STYLE_BOLD) != 0);

    /* One user-facing step, however many runs it touched. */
    ASSERT_TRUE(wp_undo_can_undo(&journal));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE((flags_at_byte(&doc, para, 3) & WP_STYLE_BOLD) == 0);
    dump_para(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    ASSERT_TRUE(wp_undo_can_redo(&journal));
    ASSERT_TRUE(wp_edit_redo(&doc, &journal) == WP_OK);
    ASSERT_TRUE((flags_at_byte(&doc, para, 3) & WP_STYLE_BOLD) != 0);
    dump_para(&doc, para, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_undo_of_a_partial_range_restores_both_fragments(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");
    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 6),
                                     pos_at(0, 0, 0, 11), &delta) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);

    /* Undo restores the STYLE everywhere.  It does not un-split the run,
       and it does not have to: content-equivalent, not layout-identical
       (docs/editing.md). */
    ASSERT_TRUE((flags_at_byte(&doc, para, 0) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 6) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 10) & WP_STYLE_BOLD) == 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_keeps_each_runs_own_size(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_char_style big;
    wp_cstyle big_handle;
    wp_span span;
    u16 run_idx;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "small");

    /* A second run at a different size, so the range spans two sizes. */
    big = *wp_document_cstyle_get(&doc, doc.default_cstyle);
    big.size_twips = 480;
    wp_document_cstyle_find_or_add(&doc, &big, &big_handle);
    wp_strpool_append(&doc.strings, (const u8 *)"BIG", 3, &span);
    wp_paragraph_run_add(para, span, big_handle, &run_idx);

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 0),
                                     pos_at(0, 0, 1, 3), &delta) == WP_OK);

    /* Both ends bold, and each keeps the size it had: a delta, not a
       wholesale style replacement. */
    ASSERT_TRUE((flags_at_byte(&doc, para, 0) & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 6) & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE(size_at_byte(&doc, para, 6) == 480);
    ASSERT_TRUE(size_at_byte(&doc, para, 0) != 480);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_clear_turns_a_flag_back_off(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta on = bold_on();
    wp_style_delta off;

    off.flags_set = 0; off.flags_clear = WP_STYLE_BOLD; off.size_twips = 0;

    build_one_run(&doc, &journal, &para, "Hello World");
    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 0),
                                     pos_at(0, 0, 0, 11), &on) == WP_OK);
    ASSERT_TRUE((flags_at_byte(&doc, para, 3) & WP_STYLE_BOLD) != 0);

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 0),
                                     pos_at(0, 0, 0, 11), &off) == WP_OK);
    ASSERT_TRUE((flags_at_byte(&doc, para, 3) & WP_STYLE_BOLD) == 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_accepts_its_ends_in_either_order(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");

    /* Selecting backwards is the same selection. */
    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 11),
                                     pos_at(0, 0, 0, 6), &delta) == WP_OK);
    ASSERT_TRUE((flags_at_byte(&doc, para, 6) & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE((flags_at_byte(&doc, para, 0) & WP_STYLE_BOLD) == 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_empty_range_changes_nothing(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");
    wp_document_mark_saved(&doc);

    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 4),
                                     pos_at(0, 0, 0, 4), &delta) == WP_OK);

    ASSERT_TRUE(para->run_count == 1);
    ASSERT_TRUE((flags_at_byte(&doc, para, 4) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE(!wp_document_is_modified(&doc));
    ASSERT_TRUE(!wp_undo_can_undo(&journal));

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_style_range_spans_paragraphs(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *first, *second;
    wp_section *sec;
    u32 para_idx;
    wp_span span;
    u16 run_idx;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &first, "first");
    sec = wp_document_section_get(&doc, 0);
    wp_section_paragraph_add(sec, &para_idx);
    second = wp_section_paragraph_get(sec, para_idx);
    wp_strpool_append(&doc.strings, (const u8 *)"second", 6, &span);
    wp_paragraph_run_add(second, span, doc.default_cstyle, &run_idx);
    first = wp_section_paragraph_get(sec, 0);   /* the array may have moved */

    /* From the middle of the first paragraph to the middle of the second. */
    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 2),
                                     pos_at(0, 1, 0, 3), &delta) == WP_OK);

    first = wp_section_paragraph_get(sec, 0);
    second = wp_section_paragraph_get(sec, 1);
    ASSERT_TRUE((flags_at_byte(&doc, first, 0) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE((flags_at_byte(&doc, first, 4) & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE((flags_at_byte(&doc, second, 0) & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE((flags_at_byte(&doc, second, 5) & WP_STYLE_BOLD) == 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_range_has_flags_answers_the_toggle_question(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_paragraph *para;
    wp_style_delta delta = bold_on();

    build_one_run(&doc, &journal, &para, "Hello World");

    /* Nothing bold yet. */
    ASSERT_TRUE(!wp_document_range_has_flags(&doc, pos_at(0, 0, 0, 0),
                                              pos_at(0, 0, 0, 11), WP_STYLE_BOLD));

    /* Half bold is not bold -- Ctrl+B on a mixed selection should set,
       not clear. */
    ASSERT_TRUE(wp_edit_style_range(&doc, &journal, pos_at(0, 0, 0, 6),
                                     pos_at(0, 0, 0, 11), &delta) == WP_OK);
    ASSERT_TRUE(!wp_document_range_has_flags(&doc, pos_at(0, 0, 0, 0),
                                              pos_at(0, 0, 0, 11), WP_STYLE_BOLD));

    /* The bold part alone answers yes. */
    ASSERT_TRUE(wp_document_range_has_flags(&doc, pos_at(0, 0, 0, 6),
                                             pos_at(0, 0, 0, 11), WP_STYLE_BOLD));

    /* An empty range has nothing to be bold. */
    ASSERT_TRUE(!wp_document_range_has_flags(&doc, pos_at(0, 0, 0, 6),
                                              pos_at(0, 0, 0, 6), WP_STYLE_BOLD));

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}
