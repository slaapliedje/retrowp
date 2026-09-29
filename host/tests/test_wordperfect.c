/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "wordperfect.h"
#include "mem_host.h"

static void build_doc_two_paras(wp_document *doc, const char *p0, const char *p1)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)p0, (u32)strlen(p0), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)p1, (u32)strlen(p1), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
}

static void dump_para_text(wp_document *doc, u32 index, char *out)
{
    wp_section *sec = wp_document_section_get(doc, 0);
    wp_paragraph *para = wp_section_paragraph_get(sec, index);
    u16 i;
    u32 pos = 0;

    for (i = 0; i < para->run_count; i++) {
        const u8 *b = wp_strpool_get(&doc->strings, para->runs[i].text);
        memcpy(out + pos, b, para->runs[i].text.length);
        pos += para->runs[i].text.length;
    }
    out[pos] = '\0';
}

/* This scheme has no prefix-key mechanism at all -- every command is
   one keystroke plus modifiers (see wordperfect.h's own header
   comment) -- so unlike test_wordstar.c/test_emacs.c, there's no
   Ctrl-then-plain-letter sequence to encode and no C string hex-escape
   hazard to worry about (M10 Phase 1's own real bug, see
   docs/keymaps.md). Plain typed text still uses a simple feed_keys. */
static void feed_keys(wp_wordperfect_state *vs, const char *text)
{
    for (; *text; text++) {
        wp_key_event key;
        key.codepoint = (u32)(unsigned char)*text;
        key.special = (u16)WP_KEY_NONE;
        key.mods = 0;
        key._pad = 0;
        wp_wordperfect_feed_key(vs, key);
    }
}

static wp_wordperfect_action_kind feed_special(wp_wordperfect_state *vs, wp_special_key special, u8 mods)
{
    wp_key_event key;
    key.codepoint = 0;
    key.special = (u16)special;
    key.mods = mods;
    key._pad = 0;
    return wp_wordperfect_feed_key(vs, key);
}

void test_wordperfect_arrow_char_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, 0);
    feed_special(&vs, WP_KEY_RIGHT, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 2);
    feed_special(&vs, WP_KEY_LEFT, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 1);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_ctrl_arrow_word_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.offset, 6);
    feed_special(&vs, WP_KEY_LEFT, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_up_down_paragraph_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_DOWN, 0);
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    feed_special(&vs, WP_KEY_UP, 0);
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_home_end(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_END, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 11);
    feed_special(&vs, WP_KEY_HOME, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_ctrl_home_goes_to_doc_start(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_DOWN, 0);
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    feed_special(&vs, WP_KEY_HOME, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_plain_typing_is_non_modal(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hello");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello");

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_delete_forward_and_backward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_DELETE, 0);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "ello");

    feed_special(&vs, WP_KEY_RIGHT, 0);
    feed_special(&vs, WP_KEY_BACKSPACE, 0);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "llo");

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_ctrl_backspace_deletes_word_backward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_END, 0);
    feed_special(&vs, WP_KEY_BACKSPACE, (u8)WP_MOD_CTRL);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello ");

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_ctrl_end_deletes_to_line_end(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_CTRL); /* Ctrl-Right -- word forward, cursor at 6 */
    feed_special(&vs, WP_KEY_END, (u8)WP_MOD_CTRL); /* Ctrl-End -- delete to line end */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello ");

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_f1_undelete_is_single_level_undo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hi");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hi");

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F1, 0) == WP_WPACTION_NONE);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "H");

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_f6_f8_f7_f10_return_correct_kinds(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F6, 0) == WP_WPACTION_FORMAT_BOLD);
    ASSERT_TRUE(feed_special(&vs, WP_KEY_F8, 0) == WP_WPACTION_FORMAT_UNDERLINE);
    ASSERT_TRUE(feed_special(&vs, WP_KEY_F10, 0) == WP_WPACTION_SAVE);
    ASSERT_TRUE(feed_special(&vs, WP_KEY_F7, 0) == WP_WPACTION_QUIT);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_block_toggle_extends_by_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_ALT) == WP_WPACTION_NONE); /* Alt-F4: block on */
    ASSERT_TRUE(vs.block_active == WP_TRUE);
    ASSERT_EQ_U32(vs.block_start.offset, 0);

    /* real anchor+extend selection: cursor motion while blocked
       extends the region, matching vi Visual mode's own test shape --
       block_start stays put, only vs.cursor (the region's other end)
       moves. */
    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.offset, 6);
    ASSERT_EQ_U32(vs.block_start.offset, 0);
    ASSERT_TRUE(vs.block_active == WP_TRUE);

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_ALT) == WP_WPACTION_NONE); /* Alt-F4 again: cancel */
    ASSERT_TRUE(vs.block_active == WP_FALSE);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_move_block_then_retrieve(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_ALT); /* block on at offset 0 */
    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_CTRL); /* extend to offset 6 -- covers "Hello " */

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_CTRL) == WP_WPACTION_NONE); /* Ctrl-F4: Move */
    ASSERT_TRUE(vs.block_active == WP_FALSE);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "World");

    feed_special(&vs, WP_KEY_END, 0);
    ASSERT_TRUE(feed_special(&vs, WP_KEY_F10, (u8)WP_MOD_SHIFT) == WP_WPACTION_NONE); /* Shift-F10: Retrieve */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "WorldHello ");

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_ctrl_f4_without_block_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_CTRL) == WP_WPACTION_BEEP);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_shift_f10_with_empty_register_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_special(&vs, WP_KEY_F10, (u8)WP_MOD_SHIFT) == WP_WPACTION_BEEP);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_search_forward_jumps_to_match(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello World", "Goodbye Moon");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, 0);
    feed_keys(&vs, "Moon");
    feed_special(&vs, WP_KEY_F2, 0);

    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    ASSERT_EQ_U32(vs.cursor.offset, 8);
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_NONE);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_search_backward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello World", "Goodbye Moon");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);
    vs.cursor.paragraph = 1; vs.cursor.offset = 12;

    feed_special(&vs, WP_KEY_F2, (u8)WP_MOD_SHIFT);
    feed_keys(&vs, "Hello");
    feed_special(&vs, WP_KEY_F2, 0); /* terminates regardless of mods, see wordperfect.c */

    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_search_empty_text_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    wp_wordperfect_action_kind action;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, 0);
    action = feed_special(&vs, WP_KEY_F2, 0);

    ASSERT_TRUE(action == WP_WPACTION_BEEP);
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_NONE);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_search_not_found_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    wp_wordperfect_action_kind action;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, 0);
    feed_keys(&vs, "xyz");
    action = feed_special(&vs, WP_KEY_F2, 0);

    ASSERT_TRUE(action == WP_WPACTION_BEEP);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_search_escape_cancels_prompt(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, 0);
    feed_keys(&vs, "Wor");
    feed_special(&vs, WP_KEY_ESCAPE, 0);

    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_NONE);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_search_f2_twice_repeats_last_search(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    /* "z" prefix keeps the doc-start cursor from coinciding with the
       first 'a' -- wp_document_find_text searches STRICTLY AFTER
       `from`, same reasoning test_wordstar.c's own analogous fixtures
       document. */
    build_doc_two_paras(&doc, "zaXaXaX", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, 0);
    feed_keys(&vs, "a");
    feed_special(&vs, WP_KEY_F2, 0); /* first search -> offset 1 */
    ASSERT_EQ_U32(vs.cursor.offset, 1);

    feed_special(&vs, WP_KEY_F2, 0); /* enter stage, buffer preserved */
    feed_special(&vs, WP_KEY_F2, 0); /* terminate with zero new keystrokes -> repeat */
    ASSERT_EQ_U32(vs.cursor.offset, 3);

    feed_special(&vs, WP_KEY_F2, 0);
    feed_special(&vs, WP_KEY_F2, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 5);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_replace_confirm_y_and_n(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "zzz foo foo foo", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, (u8)WP_MOD_ALT); /* Alt-F2 */
    feed_keys(&vs, "y"); /* w/Confirm? Y */
    feed_keys(&vs, "foo");
    feed_special(&vs, WP_KEY_F2, 0); /* -> REPLACE_TEXT */
    feed_keys(&vs, "bar");
    feed_special(&vs, WP_KEY_F2, 0); /* begins search, pauses at match 1 (offset 4) */

    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_CONFIRM_MATCH);

    feed_keys(&vs, "y"); /* replace match 1, pause at match 2 (offset 8) */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar foo foo");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_CONFIRM_MATCH);

    feed_keys(&vs, "n"); /* skip match 2, pause at match 3 (offset 12) */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar foo foo");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_CONFIRM_MATCH);

    feed_keys(&vs, "y"); /* replace match 3, nothing left */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar foo bar");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_NONE);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_replace_no_confirm_replaces_silently(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "zzz foo foo foo", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F2, (u8)WP_MOD_ALT);
    feed_keys(&vs, "n"); /* w/Confirm? N */
    feed_keys(&vs, "foo");
    feed_special(&vs, WP_KEY_F2, 0);
    feed_keys(&vs, "bar");
    feed_special(&vs, WP_KEY_F2, 0);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar bar bar");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_WP_PROMPT_NONE);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordperfect_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordperfect_state vs;

    mem_host_reset_counters();

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordperfect_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_ALT);
    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_CTRL);
    feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_CTRL);
    feed_special(&vs, WP_KEY_F10, (u8)WP_MOD_SHIFT);

    feed_special(&vs, WP_KEY_F2, (u8)WP_MOD_ALT);
    feed_keys(&vs, "n");
    feed_keys(&vs, "World");
    feed_special(&vs, WP_KEY_F2, 0);
    feed_keys(&vs, "Moon");
    feed_special(&vs, WP_KEY_F2, 0);

    wp_wordperfect_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
