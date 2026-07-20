#include <string.h>
#include "test.h"
#include "wordstar.h"
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

/* Same script convention as host/tests/test_vimode.c's own feed_keys:
   any byte < 0x20 => Ctrl+<letter> (WordStar's own real convention --
   verified against primary sources, see wordstar.h -- happens to make
   this the most natural way to write a WordStar key script too, e.g.
   feed_keys(&vs, "\x0b" "B") is literally ^K then plain B). */
static void feed_keys(wp_wordstar_state *vs, const char *script)
{
    for (; *script; script++) {
        unsigned char c = (unsigned char)*script;
        wp_key_event key;

        key.codepoint = 0;
        key.special = (u16)WP_KEY_NONE;
        key.mods = 0;
        key._pad = 0;

        if (c == 0x7f) key.special = (u16)WP_KEY_BACKSPACE;
        else if (c == '\r') key.special = (u16)WP_KEY_ENTER;
        else if (c < 0x20) { key.codepoint = (u32)(c + ('a' - 1)); key.mods = (u8)WP_MOD_CTRL; }
        else key.codepoint = (u32)c;

        wp_wordstar_feed_key(vs, key);
    }
}

static wp_wordstar_action_kind feed_key_special(wp_wordstar_state *vs, wp_special_key special)
{
    wp_key_event key;
    key.codepoint = 0;
    key.special = (u16)special;
    key.mods = 0;
    key._pad = 0;
    return wp_wordstar_feed_key(vs, key);
}

void test_wordstar_diamond_char_motions(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x04\x04\x04"); /* ^D ^D ^D -- char right x3 */
    ASSERT_EQ_U32(vs.cursor.offset, 3);
    feed_keys(&vs, "\x13"); /* ^S -- char left */
    ASSERT_EQ_U32(vs.cursor.offset, 2);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_diamond_word_motions(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x06"); /* ^F -- word right */
    ASSERT_EQ_U32(vs.cursor.offset, 6);
    feed_keys(&vs, "\x01"); /* ^A -- word left */
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_diamond_paragraph_motions(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x18"); /* ^X -- line/paragraph down */
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    feed_keys(&vs, "\x05"); /* ^E -- line/paragraph up */
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_arrow_keys_also_move(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_key_special(&vs, WP_KEY_RIGHT);
    ASSERT_EQ_U32(vs.cursor.offset, 1);
    feed_key_special(&vs, WP_KEY_LEFT);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_plain_typing_is_non_modal(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hello"); /* no mode switch needed, unlike vi's 'i' */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_ctrl_g_deletes_char_forward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x07"); /* ^G */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "ello");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_ctrl_t_deletes_word_forward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x14"); /* ^T */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "World");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_ctrl_y_deletes_current_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x19"); /* ^Y */
    ASSERT_EQ_U32(doc.sections[0].para_count, 1);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Second");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_block_copy_appends_at_cursor(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[128];

    build_doc_two_paras(&doc, "Hello ", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    vs.cursor.offset = 0;
    feed_keys(&vs, "\x0b" "B"); /* ^KB at offset 0 */
    vs.cursor.offset = 5; /* "Hello" -- end of word, before trailing space */
    feed_keys(&vs, "\x0bK"); /* ^KK marks block end -- block is "Hello" */

    vs.cursor = wp_pos_line_end(&doc, vs.cursor); /* end of paragraph */
    feed_keys(&vs, "\x0b" "C"); /* ^KC -- copy block to cursor */

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello Hello");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_block_move_cursor_before_block(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[128];
    wp_pos begin;

    build_doc_two_paras(&doc, "AAA BBB", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    begin = vs.cursor; /* offset 0 */
    vs.cursor.offset = 4; /* start of "BBB" */
    vs.block_begin = vs.cursor;
    vs.block_begin_set = WP_TRUE;
    vs.cursor.offset = 7; /* end of "BBB" */
    vs.block_end = vs.cursor;
    vs.block_end_set = WP_TRUE;

    vs.cursor = begin; /* destination: before the block */
    feed_keys(&vs, "\x0bV"); /* ^KV -- move block */

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "BBBAAA ");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_block_move_cursor_after_block(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[128];

    build_doc_two_paras(&doc, "AAA BBB CCC", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    vs.cursor.offset = 0;
    vs.block_begin = vs.cursor;
    vs.block_begin_set = WP_TRUE;
    vs.cursor.offset = 3; /* end of "AAA" */
    vs.block_end = vs.cursor;
    vs.block_end_set = WP_TRUE;

    vs.cursor.offset = 11; /* end of "CCC", well after the block */
    feed_keys(&vs, "\x0bV"); /* ^KV -- move block */

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, " BBB CCCAAA");

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_block_move_refuses_when_cursor_inside_block(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[128];

    build_doc_two_paras(&doc, "AAA BBB CCC", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    vs.cursor.offset = 0;
    vs.block_begin = vs.cursor;
    vs.block_begin_set = WP_TRUE;
    vs.cursor.offset = 7;
    vs.block_end = vs.cursor;
    vs.block_end_set = WP_TRUE;

    vs.cursor.offset = 5; /* inside the marked block */
    feed_keys(&vs, "\x0bV");

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "AAA BBB CCC"); /* unchanged -- refused, not silently wrong */

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_block_delete(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    char buf[128];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    vs.cursor.offset = 0;
    vs.block_begin = vs.cursor;
    vs.block_begin_set = WP_TRUE;
    vs.cursor.offset = 6;
    vs.block_end = vs.cursor;
    vs.block_end_set = WP_TRUE;

    feed_keys(&vs, "\x0bY"); /* ^KY -- delete block */

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "World");
    ASSERT_TRUE(vs.block_begin_set == WP_FALSE);
    ASSERT_TRUE(vs.block_end_set == WP_FALSE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_kx_returns_save_quit_and_kq_returns_quit(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    wp_key_event key;
    wp_wordstar_action_kind action;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    key.special = (u16)WP_KEY_NONE; key.mods = (u8)WP_MOD_CTRL; key.codepoint = 'k'; key._pad = 0;
    wp_wordstar_feed_key(&vs, key);
    key.mods = 0; key.codepoint = 'd';
    action = wp_wordstar_feed_key(&vs, key);
    ASSERT_TRUE(action == WP_WSACTION_SAVE);

    key.mods = (u8)WP_MOD_CTRL; key.codepoint = 'k';
    wp_wordstar_feed_key(&vs, key);
    key.mods = 0; key.codepoint = 'x';
    action = wp_wordstar_feed_key(&vs, key);
    ASSERT_TRUE(action == WP_WSACTION_SAVE_QUIT);

    key.mods = (u8)WP_MOD_CTRL; key.codepoint = 'k';
    wp_wordstar_feed_key(&vs, key);
    key.mods = 0; key.codepoint = 'q';
    action = wp_wordstar_feed_key(&vs, key);
    ASSERT_TRUE(action == WP_WSACTION_QUIT);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_unbound_prefix_subkey_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    wp_wordstar_action_kind action;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x0b"); /* ^K prefix */
    action = feed_key_special(&vs, WP_KEY_F1); /* not a bound ^K subcommand */
    ASSERT_TRUE(action == WP_WSACTION_BEEP);
    ASSERT_EQ_U32(vs.pending_prefix, 0); /* consumed, not left dangling */

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_format_prefix_returns_correct_kinds(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;
    wp_key_event key;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    key.special = (u16)WP_KEY_NONE; key._pad = 0;

    key.mods = (u8)WP_MOD_CTRL; key.codepoint = 'p';
    wp_wordstar_feed_key(&vs, key);
    key.mods = 0; key.codepoint = 'b';
    ASSERT_TRUE(wp_wordstar_feed_key(&vs, key) == WP_WSACTION_FORMAT_BOLD);

    key.mods = (u8)WP_MOD_CTRL; key.codepoint = 'p';
    wp_wordstar_feed_key(&vs, key);
    key.mods = 0; key.codepoint = 'y';
    ASSERT_TRUE(wp_wordstar_feed_key(&vs, key) == WP_WSACTION_FORMAT_ITALIC);

    key.mods = (u8)WP_MOD_CTRL; key.codepoint = 'p';
    wp_wordstar_feed_key(&vs, key);
    key.mods = 0; key.codepoint = 's';
    ASSERT_TRUE(wp_wordstar_feed_key(&vs, key) == WP_WSACTION_FORMAT_UNDERLINE);

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_wordstar_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_wordstar_state vs;

    mem_host_reset_counters();

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_wordstar_init(&vs, &doc, &journal);

    vs.cursor.offset = 0;
    vs.block_begin = vs.cursor;
    vs.block_begin_set = WP_TRUE;
    vs.cursor.offset = 5;
    vs.block_end = vs.cursor;
    vs.block_end_set = WP_TRUE;
    vs.cursor.offset = 11;
    feed_keys(&vs, "\x0b" "C"); /* ^KC -- copy block (transient mem_alloc/mem_free internally) */

    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
