/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "msword.h"
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

/* No prefix-key mechanism at all (see msword.h's own header comment),
   so like test_wordperfect.c there's no Ctrl-then-plain-letter
   sequence to encode and no C string hex-escape hazard to worry about. */
static void feed_keys(wp_msword_state *vs, const char *text)
{
    for (; *text; text++) {
        wp_key_event key;
        key.codepoint = (u32)(unsigned char)*text;
        key.special = (u16)WP_KEY_NONE;
        key.mods = 0;
        key._pad = 0;
        wp_msword_feed_key(vs, key);
    }
}

static wp_msword_action_kind feed_special(wp_msword_state *vs, wp_special_key special, u8 mods)
{
    wp_key_event key;
    key.codepoint = 0;
    key.special = (u16)special;
    key.mods = mods;
    key._pad = 0;
    return wp_msword_feed_key(vs, key);
}

static wp_msword_action_kind feed_full(wp_msword_state *vs, u32 codepoint, u8 mods)
{
    wp_key_event key;
    key.codepoint = codepoint;
    key.special = (u16)WP_KEY_NONE;
    key.mods = mods;
    key._pad = 0;
    return wp_msword_feed_key(vs, key);
}

void test_msword_arrow_char_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, 0);
    feed_special(&vs, WP_KEY_RIGHT, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 2);
    feed_special(&vs, WP_KEY_LEFT, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 1);
    ASSERT_TRUE(vs.selecting == WP_FALSE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_shift_arrow_extends_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_SHIFT);
    ASSERT_TRUE(vs.selecting == WP_TRUE);
    ASSERT_EQ_U32(vs.anchor.offset, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 1);

    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_SHIFT);
    ASSERT_EQ_U32(vs.anchor.offset, 0); /* anchor stays fixed while extending */
    ASSERT_EQ_U32(vs.cursor.offset, 2);

    feed_special(&vs, WP_KEY_RIGHT, 0); /* motion without Shift drops the selection */
    ASSERT_TRUE(vs.selecting == WP_FALSE);
    ASSERT_EQ_U32(vs.cursor.offset, 3);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_ctrl_arrow_word_motion_and_shift_ctrl_extends_by_word(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    /* Three words, all in one paragraph -- wp_pos_word_forward crosses
       INTO the next paragraph once it runs out of words in this one
       (the same cross-paragraph behavior wp_pos_next/prev document for
       themselves), so a two-word fixture with an empty second
       paragraph right after would make the second Ctrl-Right's landing
       ambiguous for this test's actual purpose (proving Shift extends
       by word, not pinning down that unrelated cross-paragraph edge). */
    build_doc_two_paras(&doc, "Hello World Foo", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.offset, 6);
    ASSERT_TRUE(vs.selecting == WP_FALSE);

    feed_special(&vs, WP_KEY_RIGHT, (u8)(WP_MOD_CTRL | WP_MOD_SHIFT));
    ASSERT_TRUE(vs.selecting == WP_TRUE);
    ASSERT_EQ_U32(vs.anchor.offset, 6);
    ASSERT_EQ_U32(vs.cursor.offset, 12);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_home_end_and_ctrl_home_end(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_END, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 5);
    feed_special(&vs, WP_KEY_HOME, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    feed_special(&vs, WP_KEY_END, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    feed_special(&vs, WP_KEY_HOME, (u8)WP_MOD_CTRL);
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_plain_typing_is_non_modal(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hello");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_typing_over_selection_replaces_it(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)(WP_MOD_CTRL | WP_MOD_SHIFT)); /* select "Hello " */
    ASSERT_TRUE(vs.selecting == WP_TRUE);

    feed_full(&vs, 'X', 0); /* typing a plain char replaces the selection */
    ASSERT_TRUE(vs.selecting == WP_FALSE);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "XWorld");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_delete_and_backspace_without_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_DELETE, 0);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "ello");

    feed_special(&vs, WP_KEY_RIGHT, 0);
    feed_special(&vs, WP_KEY_BACKSPACE, 0);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "llo");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_delete_with_active_selection_deletes_the_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)(WP_MOD_CTRL | WP_MOD_SHIFT)); /* select "Hello " */
    ASSERT_TRUE(feed_special(&vs, WP_KEY_DELETE, 0) == WP_MSACTION_NONE);
    ASSERT_TRUE(vs.selecting == WP_FALSE);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_ctrl_delete_and_backspace_delete_whole_words(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_DELETE, (u8)WP_MOD_CTRL); /* deletes "Hello " (word_forward eats trailing space) */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "World");

    feed_special(&vs, WP_KEY_END, 0);
    feed_special(&vs, WP_KEY_BACKSPACE, (u8)WP_MOD_CTRL);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_enter_with_selection_deletes_it_first(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)(WP_MOD_CTRL | WP_MOD_SHIFT)); /* select "Hello " */
    feed_special(&vs, WP_KEY_ENTER, 0);
    ASSERT_TRUE(vs.selecting == WP_FALSE);
    ASSERT_EQ_U32(doc.sections[0].para_count, 3); /* the empty split + the pre-existing second paragraph */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "");
    dump_para_text(&doc, 1, buf);
    ASSERT_EQ_STR(buf, "World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_ctrl_a_selects_all(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, 0);
    ASSERT_TRUE(feed_full(&vs, 'a', (u8)WP_MOD_CTRL) == WP_MSACTION_NONE);
    ASSERT_TRUE(vs.selecting == WP_TRUE);
    ASSERT_EQ_U32(vs.anchor.paragraph, 0);
    ASSERT_EQ_U32(vs.anchor.offset, 0);
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_ctrl_z_undo_and_ctrl_y_redo(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hi");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hi");

    ASSERT_TRUE(feed_full(&vs, 'z', (u8)WP_MOD_CTRL) == WP_MSACTION_NONE); /* Ctrl-Z: undo */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "H");

    ASSERT_TRUE(feed_full(&vs, 'y', (u8)WP_MOD_CTRL) == WP_MSACTION_NONE); /* Ctrl-Y: redo */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hi");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_ctrl_letter_action_bindings(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_full(&vs, 'b', (u8)WP_MOD_CTRL) == WP_MSACTION_FORMAT_BOLD);
    ASSERT_TRUE(feed_full(&vs, 'i', (u8)WP_MOD_CTRL) == WP_MSACTION_FORMAT_ITALIC);
    ASSERT_TRUE(feed_full(&vs, 'u', (u8)WP_MOD_CTRL) == WP_MSACTION_FORMAT_UNDERLINE);
    ASSERT_TRUE(feed_full(&vs, 's', (u8)WP_MOD_CTRL) == WP_MSACTION_SAVE);
    ASSERT_TRUE(feed_full(&vs, 'o', (u8)WP_MOD_CTRL) == WP_MSACTION_OPEN);
    ASSERT_TRUE(feed_full(&vs, 'c', (u8)WP_MOD_CTRL) == WP_MSACTION_COPY);
    ASSERT_TRUE(feed_full(&vs, 'x', (u8)WP_MOD_CTRL) == WP_MSACTION_CUT);
    ASSERT_TRUE(feed_full(&vs, 'v', (u8)WP_MOD_CTRL) == WP_MSACTION_PASTE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_unbound_ctrl_letter_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    /* 'q' used to be the unbound letter this asserted on; it is now a
       real Quit binding (see WP_MSACTION_QUIT). 'k' is genuinely unbound
       in handle_ctrl_letter. */
    ASSERT_TRUE(feed_full(&vs, 'k', (u8)WP_MOD_CTRL) == WP_MSACTION_BEEP);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Ctrl-Q had no binding in this scheme at all, which left the DOS build
   -- the only backend with no menu bar to fall back on -- with no way to
   exit from the keyboard under keymap=msword, while its title bar
   advertised "Ctrl+Q: Quit" regardless. */
void test_msword_ctrl_q_quits(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_full(&vs, 'q', (u8)WP_MOD_CTRL) == WP_MSACTION_QUIT);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_find_jumps_to_match(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "Hello World", "Goodbye Moon");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'f', (u8)WP_MOD_CTRL); /* Ctrl+F */
    feed_keys(&vs, "Moon");
    feed_special(&vs, WP_KEY_ENTER, 0);

    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    ASSERT_EQ_U32(vs.cursor.offset, 8);
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_NONE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_find_wraps_around_document(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "Hello World", "Goodbye Moon");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);
    vs.cursor.paragraph = 1; vs.cursor.offset = 12; /* end of doc -- "Hello" is only reachable by wrapping */

    feed_full(&vs, 'f', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "Hello");
    feed_special(&vs, WP_KEY_ENTER, 0);

    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_find_not_found_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    wp_msword_action_kind action;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'f', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "xyz");
    action = feed_special(&vs, WP_KEY_ENTER, 0);

    ASSERT_TRUE(action == WP_MSACTION_BEEP);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_find_escape_cancels_prompt(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'f', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "Wor");
    feed_special(&vs, WP_KEY_ESCAPE, 0);

    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_NONE);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_shift_f4_repeats_last_find(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    /* "z" prefix keeps the doc-start cursor from coinciding with the
       first 'a' -- wp_document_find_text searches STRICTLY AFTER
       `from`, same reasoning test_wordstar.c's own analogous fixtures
       document. */
    build_doc_two_paras(&doc, "zaXaXaX", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'f', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "a");
    feed_special(&vs, WP_KEY_ENTER, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 1);

    feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_SHIFT);
    ASSERT_EQ_U32(vs.cursor.offset, 3);

    feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_SHIFT);
    ASSERT_EQ_U32(vs.cursor.offset, 5);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_shift_f4_without_prior_find_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    wp_msword_action_kind action;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    action = feed_special(&vs, WP_KEY_F4, (u8)WP_MOD_SHIFT);
    ASSERT_TRUE(action == WP_MSACTION_BEEP);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_replace_r_and_n(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "zzz foo foo foo", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'h', (u8)WP_MOD_CTRL); /* Ctrl+H */
    feed_keys(&vs, "foo");
    feed_special(&vs, WP_KEY_ENTER, 0); /* -> replace text field */
    feed_keys(&vs, "bar");
    feed_special(&vs, WP_KEY_ENTER, 0); /* begins search, pauses at match 1 (offset 4) */

    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_REPLACE_CONFIRM);

    feed_keys(&vs, "r"); /* replace match 1, advance to match 2 (offset 8) */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar foo foo");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_REPLACE_CONFIRM);

    feed_keys(&vs, "n"); /* skip match 2, advance to match 3 (offset 12) */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar foo foo");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_REPLACE_CONFIRM);

    feed_keys(&vs, "r"); /* replace match 3, nothing left */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar foo bar");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_NONE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_replace_all_replaces_silently(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "zzz foo foo foo", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'h', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "foo");
    feed_special(&vs, WP_KEY_ENTER, 0);
    feed_keys(&vs, "bar");
    feed_special(&vs, WP_KEY_ENTER, 0);

    feed_keys(&vs, "a"); /* Replace All */

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz bar bar bar");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_NONE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_replace_confirm_escape_closes(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "zzz foo foo", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_full(&vs, 'h', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "foo");
    feed_special(&vs, WP_KEY_ENTER, 0);
    feed_keys(&vs, "bar");
    feed_special(&vs, WP_KEY_ENTER, 0);

    feed_special(&vs, WP_KEY_ESCAPE, 0);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "zzz foo foo");
    ASSERT_TRUE(vs.prompt_stage == (u8)WP_MS_PROMPT_NONE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_msword_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_msword_state vs;

    mem_host_reset_counters();

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_msword_init(&vs, &doc, &journal);

    feed_special(&vs, WP_KEY_RIGHT, (u8)(WP_MOD_CTRL | WP_MOD_SHIFT));
    feed_special(&vs, WP_KEY_DELETE, 0);
    feed_full(&vs, 'z', (u8)WP_MOD_CTRL);
    feed_full(&vs, 'y', (u8)WP_MOD_CTRL);

    feed_full(&vs, 'h', (u8)WP_MOD_CTRL);
    feed_keys(&vs, "World");
    feed_special(&vs, WP_KEY_ENTER, 0);
    feed_keys(&vs, "Moon");
    feed_special(&vs, WP_KEY_ENTER, 0);
    feed_keys(&vs, "a");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
