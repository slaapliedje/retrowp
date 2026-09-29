/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "vimode.h"
#include "mem_host.h"

/* Turns an ASCII script into synthetic wp_key_event feeds: 0x1b =>
   Escape, '\r' => Enter, 0x7f => Backspace, any other byte < 0x20 =>
   Ctrl+<letter> (mirroring how a real backend would translate a
   Ctrl-chord: byte 0x12 => {codepoint:'r', mods:CTRL}, i.e. Ctrl+R),
   everything else => a plain printable codepoint. Lets a test write
   feed_keys(&vs, "3dw") or feed_keys(&vs, ":wq\r") directly and assert
   on the resulting document/cursor/mode state -- no platform backend
   anywhere in the loop, per docs/vimode.md's testability requirement. */
static void feed_keys(wp_vimode_state *vs, const char *script)
{
    for (; *script; script++) {
        unsigned char c = (unsigned char)*script;
        wp_key_event key;

        key.codepoint = 0;
        key.special = (u16)WP_KEY_NONE;
        key.mods = 0;
        key._pad = 0;

        if (c == 0x1b) key.special = (u16)WP_KEY_ESCAPE;
        else if (c == '\r') key.special = (u16)WP_KEY_ENTER;
        else if (c == 0x7f) key.special = (u16)WP_KEY_BACKSPACE;
        else if (c < 0x20) { key.codepoint = (u32)(c + ('a' - 1)); key.mods = (u8)WP_MOD_CTRL; }
        else key.codepoint = (u32)c;

        wp_vimode_feed_key(vs, key);
    }
}

static wp_vimode_action feed_key_special(wp_vimode_state *vs, wp_special_key special)
{
    wp_key_event key;
    key.codepoint = 0;
    key.special = (u16)special;
    key.mods = 0;
    key._pad = 0;
    return wp_vimode_feed_key(vs, key);
}

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

/* Builds a section of `count` one-run paragraphs, for the multi-line
   operator cases below -- build_doc_two_paras cannot express "delete
   three of these and check two survive". */
static void build_doc_paras(wp_document *doc, const char *const *texts, u32 count)
{
    u32 sec_idx, para_idx, i;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);

    for (i = 0; i < count; i++) {
        ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
        para = wp_section_paragraph_get(sec, para_idx);
        ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)texts[i],
                                       (u32)strlen(texts[i]), &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
    }
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

void test_vimode_insert_then_escape(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);
    feed_keys(&vs, "iHello\x1b");
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_hjkl_and_word_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;

    build_doc_two_paras(&doc, "Hello World", "Second");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "w"); /* -> start of "World" */
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 6);

    feed_keys(&vs, "j"); /* -> paragraph 1 */
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    feed_keys(&vs, "k"); /* back to paragraph 0 */
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_count_prefix_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;

    build_doc_two_paras(&doc, "one two three four", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "3w"); /* one -> two -> three -> four */
    ASSERT_EQ_U32(vs.cursor.offset, 14); /* start of "four" */

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_dw_deletes_word(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "dw");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "World");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* The classic vim nuance explicitly called out as in-scope: dw at the
   last word of a paragraph must NOT delete across the paragraph break
   (which a bare 'w' legitimately crosses -- see
   test_motion_word_forward_crosses_paragraph_boundary). */
void test_vimode_dw_does_not_eat_paragraph_break(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_two_paras(&doc, "Hello", "World");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "dw");
    ASSERT_EQ_U32(sec->para_count, 2); /* paragraph break survives */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "");
    dump_para_text(&doc, 1, buf);
    ASSERT_EQ_STR(buf, "World");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* COUNTED OPERATORS. dd and dw were already covered; the counted forms
   were not, and they are a different code path: the count reaching
   apply_linewise_current_lines is effective_op_count * effective_count,
   so "3dd", "d3d" and even "1d3d" all have to arrive at the same three
   lines. Worth pinning down precisely because the multiplication makes
   a plausible-looking off-by-one silently delete the wrong amount. */
void test_vimode_count_dd_deletes_multiple_lines(void)
{
    static const char *const texts[5] = { "One", "Two", "Three", "Four", "Five" };
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_paras(&doc, texts, 5);
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "3dd");

    ASSERT_EQ_U32(sec->para_count, 2);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Four");
    dump_para_text(&doc, 1, buf);
    ASSERT_EQ_STR(buf, "Five");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* The count AFTER the operator, applied to a motion rather than to a
   doubled operator: d2j is "delete this line and the two below". */
void test_vimode_d_count_motion_deletes_multiple_lines(void)
{
    static const char *const texts[5] = { "One", "Two", "Three", "Four", "Five" };
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_paras(&doc, texts, 5);
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "d2j");

    ASSERT_EQ_U32(sec->para_count, 2);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Four");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* A multi-line delete must undo as ONE step, not as three. Insert
   sessions and cw already have this guarantee pinned by their own tests
   (see test_vimode_insert_session_undoes_as_one_step); a counted delete
   is the same promise and would be just as annoying to get wrong --
   three presses of u to walk back one command teaches the writer not to
   trust undo. */
void test_vimode_count_dd_undoes_as_one_step(void)
{
    static const char *const texts[4] = { "One", "Two", "Three", "Four" };
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_paras(&doc, texts, 4);
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "3dd");
    ASSERT_EQ_U32(sec->para_count, 1);

    feed_keys(&vs, "u");

    ASSERT_EQ_U32(sec->para_count, 4);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "One");
    dump_para_text(&doc, 3, buf);
    ASSERT_EQ_STR(buf, "Four");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_dollar_deletes_to_end_of_line(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "keep this", "Second");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "llllld$");

    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "keep ");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_dd_deletes_whole_line(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "dd");
    ASSERT_EQ_U32(sec->para_count, 1);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Second");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* dd on the LAST paragraph must merge upward (into the paragraph
   above), not just empty itself and leave an orphaned blank paragraph
   behind -- see normalize_linewise_selection's backward-extend branch. */
void test_vimode_dd_on_last_paragraph_merges_upward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "j"); /* move to the last paragraph */
    feed_keys(&vs, "dd");

    ASSERT_EQ_U32(sec->para_count, 1);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "First");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_yy_p_duplicates_line_below(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "yyp");
    ASSERT_EQ_U32(sec->para_count, 3);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "First");
    dump_para_text(&doc, 1, buf); ASSERT_EQ_STR(buf, "First");
    dump_para_text(&doc, 2, buf); ASSERT_EQ_STR(buf, "Second");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_cw_change_word(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "cwGoodbye\x1b");
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Goodbye World");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* The real payoff of undo grouping: an entire Insert-mode typing
   session (open the mode, type several characters, Escape) undoes as
   ONE step, and cw's leading delete nests correctly inside that. */
void test_vimode_insert_session_undoes_as_one_step(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "iHello World\x1b");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    ASSERT_TRUE(wp_undo_can_undo(&journal));
    feed_keys(&vs, "u");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, ""); /* the WHOLE sentence undone in one 'u' */
    ASSERT_TRUE(!wp_undo_can_undo(&journal));

    feed_keys(&vs, "\x12"); /* Ctrl+R */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_cw_undoes_as_one_step(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "cwGoodbye\x1b");
    feed_keys(&vs, "u"); /* one undo restores the ENTIRE original word, not just the typed part */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello World");
    ASSERT_TRUE(!wp_undo_can_undo(&journal));

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_visual_mode_delete(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "vwwd"); /* select from start through "World"'s start, twice-w then delete */
    dump_para_text(&doc, 0, buf);
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);
    /* two 'w' from offset 0 of "Hello World" lands at doc end (11);
       visual delete [0,11) removes everything */
    ASSERT_EQ_STR(buf, "");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_visual_line_mode_delete_spans_paragraphs(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    char buf[64];

    build_doc_two_paras(&doc, "First", "Second");
    sec_idx = 0;
    sec = wp_document_section_get(&doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Third", 5, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "Vjd"); /* Visual-Line, extend down one, delete -- removes First+Second */
    ASSERT_EQ_U32(sec->para_count, 1);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Third");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_ex_command_save_quit(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_vimode_action action;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":w");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);

    feed_keys(&vs, ":wq");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE_QUIT);

    feed_keys(&vs, ":q");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_QUIT);

    feed_keys(&vs, ":bogus");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_BEEP);

    /* T172: the bare forms above must keep meaning exactly what they
       meant before filename arguments existed -- an argument is opt-in,
       never a change to an existing command's behavior. Asserted right
       here rather than in a separate test so a regression shows up
       against the original expectations, not beside them. */
    feed_keys(&vs, ":w");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(action.arg_len == 0);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_search_forward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;

    build_doc_two_paras(&doc, "Hello World", "Goodbye Moon");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "/Moon");
    feed_key_special(&vs, WP_KEY_ENTER);

    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    ASSERT_EQ_U32(vs.cursor.offset, 8); /* start of "Moon" within "Goodbye Moon" */

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_search_backward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;

    build_doc_two_paras(&doc, "Hello World", "Goodbye Moon");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    vs.cursor.paragraph = 1; vs.cursor.offset = 12; /* end of "Goodbye Moon" */

    feed_keys(&vs, "?Hello");
    feed_key_special(&vs, WP_KEY_ENTER);

    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_n_and_capital_n_respect_search_direction(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;

    build_doc_two_paras(&doc, "aXaXaX", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    /* cursor starts at doc start (offset 0); search is strictly-after
       `from`, so "/a" lands on the SECOND 'a' (offset 2), not the one
       already under the cursor. */
    feed_keys(&vs, "/a");
    feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_EQ_U32(vs.cursor.offset, 2);

    feed_keys(&vs, "n");
    ASSERT_EQ_U32(vs.cursor.offset, 4); /* 'n' repeats forward */

    feed_keys(&vs, "N");
    ASSERT_EQ_U32(vs.cursor.offset, 2); /* 'N' reverses to backward */

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_current_paragraph_replaces_first_match_only(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo foo foo", "foo");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/");
    feed_key_special(&vs, WP_KEY_ENTER);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar foo foo");
    dump_para_text(&doc, 1, buf); ASSERT_EQ_STR(buf, "foo");
    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_NORMAL);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_global_flag_replaces_every_match_in_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo foo foo", "foo");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/g");
    feed_key_special(&vs, WP_KEY_ENTER);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar bar bar");
    dump_para_text(&doc, 1, buf); ASSERT_EQ_STR(buf, "foo");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_percent_scopes_to_whole_section(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo", "foo foo");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":%s/foo/bar/");
    feed_key_special(&vs, WP_KEY_ENTER);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar");
    dump_para_text(&doc, 1, buf); ASSERT_EQ_STR(buf, "bar foo"); /* first match per paragraph only */

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_percent_and_global_replaces_everywhere(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo", "foo foo");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":%s/foo/bar/g");
    feed_key_special(&vs, WP_KEY_ENTER);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar");
    dump_para_text(&doc, 1, buf); ASSERT_EQ_STR(buf, "bar bar");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_ignore_case_flag(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Foo FOO", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/gi");
    feed_key_special(&vs, WP_KEY_ENTER);

    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar bar");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_confirm_y_replaces_n_skips(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo foo foo", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/gc");
    feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_SUBSTITUTE_CONFIRM);
    ASSERT_EQ_U32(vs.subst_match.offset, 0);

    feed_keys(&vs, "y"); /* replace match 1, pause at match 2 */
    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_SUBSTITUTE_CONFIRM);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar foo foo");
    /* subst_match.offset is no longer checked here -- replacing match 1
       splits the paragraph into multiple runs (insert never coalesces,
       see editing.h), so wp_pos.offset is now relative to whichever run
       subst_match.run names, not the flattened paragraph byte offset
       the pre-edit offset-0 check above relied on. */

    feed_keys(&vs, "n"); /* skip match 2, pause at match 3 */
    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_SUBSTITUTE_CONFIRM);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar foo foo");

    feed_keys(&vs, "y"); /* replace match 3, nothing left */
    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_NORMAL);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar foo bar");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_confirm_a_replaces_all_remaining(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo foo foo", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/gc");
    feed_key_special(&vs, WP_KEY_ENTER);

    feed_keys(&vs, "y"); /* replace match 1 */
    feed_keys(&vs, "a"); /* replace matches 2 and 3 without further confirmation */
    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_NORMAL);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar bar bar");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_confirm_q_stops_without_replacing(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo foo foo", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/gc");
    feed_key_special(&vs, WP_KEY_ENTER);
    feed_keys(&vs, "q");

    ASSERT_TRUE(vs.mode == (u8)WP_VIMODE_NORMAL);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "foo foo foo");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_malformed_command_falls_through_to_beep(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_vimode_action action;

    build_doc_two_paras(&doc, "foo", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":ssomething");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == (u8)WP_VIACTION_BEEP);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_substitute_global_undoes_as_one_step(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "foo foo foo", "unused");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, ":s/foo/bar/g");
    feed_key_special(&vs, WP_KEY_ENTER);
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "bar bar bar");

    feed_keys(&vs, "u");
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "foo foo foo");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_o_opens_line_below(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_section *sec;
    char buf[64];

    build_doc_two_paras(&doc, "First", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);
    sec = wp_document_section_get(&doc, 0);

    feed_keys(&vs, "oSecond\x1b");
    ASSERT_EQ_U32(sec->para_count, 3); /* First / Second / (the original empty second paragraph) */
    dump_para_text(&doc, 0, buf); ASSERT_EQ_STR(buf, "First");
    dump_para_text(&doc, 1, buf); ASSERT_EQ_STR(buf, "Second");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_x_deletes_char_forward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "x");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "ello");

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* M8.5 Phase 9 regression: a Ctrl+<letter> chord must never be treated
   as its bare letter. Found integrating the Atari retrofit -- app-level
   shortcuts (Ctrl+B for Bold, Ctrl+X for Cut, ...) reuse the same
   codepoint wp_key_event gives a plain letter (see keyevent.h), so
   without an explicit mods check, feeding a host's raw Ctrl+B straight
   into vi-mode silently moved the cursor backward a word ('b') instead
   of leaving it for the caller to treat as an app shortcut. Covers
   Normal (motion + operator-entry letters), Insert (would otherwise
   insert the literal letter), and Visual (operator letters) -- the
   three modes with their own codepoint-only switches. Ctrl+R (redo) is
   the one deliberate exception and is already covered by
   test_vimode_no_leaks's own "\x12" feed elsewhere in this file. */
void test_vimode_ctrl_chord_is_not_bare_letter(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_key_event ctrl_b;
    char buf[64];

    ctrl_b.codepoint = 'b';
    ctrl_b.special = (u16)WP_KEY_NONE;
    ctrl_b.mods = (u8)WP_MOD_CTRL;
    ctrl_b._pad = 0;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "$"); /* end of "Hello World" -- a bare 'b' from here would move the cursor */
    ASSERT_TRUE(wp_vimode_feed_key(&vs, ctrl_b).kind == (u8)WP_VIACTION_BEEP);
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);

    feed_keys(&vs, "i");
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_INSERT);
    wp_vimode_feed_key(&vs, ctrl_b); /* must NOT insert a literal 'b' */
    feed_keys(&vs, "\x1b");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    feed_keys(&vs, "v");
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_VISUAL);
    ASSERT_TRUE(wp_vimode_feed_key(&vs, ctrl_b).kind == (u8)WP_VIACTION_BEEP);
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_VISUAL); /* not swallowed as an operator letter */

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* M10 Phase 2: the Alt-flavored sibling of
   test_vimode_ctrl_chord_is_not_bare_letter, added once both backends
   started actually setting WP_MOD_ALT (previously always 0, so this
   path was never reachable with a real modifier bit -- see
   platform/atari/app_shell.c's atari_translate_key and platform/amiga/
   app_shell.c's translate_rawkey). The same `mods != 0` guard that
   already protects Ctrl+<letter> app shortcuts is not Ctrl-specific --
   it rejects ANY nonzero mods -- so this proves that holds for Alt too,
   the same three modes (Normal/Insert/Visual), rather than assuming it
   from the Ctrl case alone. */
void test_vimode_alt_chord_is_not_bare_letter(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_key_event alt_b;
    char buf[64];

    alt_b.codepoint = 'b';
    alt_b.special = (u16)WP_KEY_NONE;
    alt_b.mods = (u8)WP_MOD_ALT;
    alt_b._pad = 0;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "$"); /* end of "Hello World" -- a bare 'b' from here would move the cursor */
    ASSERT_TRUE(wp_vimode_feed_key(&vs, alt_b).kind == (u8)WP_VIACTION_BEEP);
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_NORMAL);

    feed_keys(&vs, "i");
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_INSERT);
    wp_vimode_feed_key(&vs, alt_b); /* must NOT insert a literal 'b' */
    feed_keys(&vs, "\x1b");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    feed_keys(&vs, "v");
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_VISUAL);
    ASSERT_TRUE(wp_vimode_feed_key(&vs, alt_b).kind == (u8)WP_VIACTION_BEEP);
    ASSERT_TRUE(wp_vimode_get_mode(&vs) == WP_VIMODE_VISUAL); /* not swallowed as an operator letter */

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;

    mem_host_reset_counters();

    build_doc_two_paras(&doc, "Hello World", "Second Line");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    feed_keys(&vs, "dwyyp3wx");
    feed_keys(&vs, "u");
    feed_keys(&vs, "\x12"); /* Ctrl+R */
    feed_keys(&vs, "iMore text\x1b");
    feed_keys(&vs, ":w");
    feed_key_special(&vs, WP_KEY_ENTER);
    feed_keys(&vs, "/Line");
    feed_key_special(&vs, WP_KEY_ENTER);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}

/* ------------------------------------------------------------------ */
/* T172: ex-command filename arguments (":w name", ":e name")          */
/* ------------------------------------------------------------------ */

static wp_vimode_action run_ex(wp_vimode_state *vs, const char *cmdline)
{
    feed_keys(vs, cmdline);
    return feed_key_special(vs, WP_KEY_ENTER);
}

static wp_bool arg_equals(const wp_vimode_action *a, const char *expect)
{
    u32 i, n = 0;

    while (expect[n] != '\0') n++;
    if (a->arg_len != n) return WP_FALSE;
    for (i = 0; i < n; i++) {
        if (a->arg[i] != (u8)expect[i]) return WP_FALSE;
    }
    return WP_TRUE;
}

void test_vimode_ex_save_with_filename(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_vimode_action action;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    action = run_ex(&vs, ":w C:\\FOO.TXT");
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(arg_equals(&action, "C:\\FOO.TXT"));

    /* ":wq" and ":x" are the same action and must carry the name too --
       a writer who types ":wq notes.txt" is naming a file, not quitting
       without saving it. */
    action = run_ex(&vs, ":wq DH0:notes.txt");
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE_QUIT);
    ASSERT_TRUE(arg_equals(&action, "DH0:notes.txt"));

    action = run_ex(&vs, ":x Unix:NOTES.TXT");
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE_QUIT);
    ASSERT_TRUE(arg_equals(&action, "Unix:NOTES.TXT"));

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_ex_open_command(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_vimode_action action;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    action = run_ex(&vs, ":e C:\\NOTES.TXT");
    ASSERT_TRUE(action.kind == WP_VIACTION_OPEN);
    ASSERT_TRUE(arg_equals(&action, "C:\\NOTES.TXT"));

    /* Bare ":e" is legal and means "open however you normally would" --
       the backend raises its own native picker, exactly as bare ":w"
       already means "save however you normally would". It must NOT beep. */
    action = run_ex(&vs, ":e");
    ASSERT_TRUE(action.kind == WP_VIACTION_OPEN);
    ASSERT_TRUE(action.arg_len == 0);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_vimode_ex_filename_edge_cases(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_vimode_state vs;
    wp_vimode_action action;
    u32 i;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_vimode_init(&vs, &doc, &journal);

    /* Extra separating spaces are a typo, not a different filename. */
    action = run_ex(&vs, ":w    spaced.txt");
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(arg_equals(&action, "spaced.txt"));

    /* Trailing spaces likewise -- a name with a trailing blank is almost
       certainly not what was meant, and on 8.3 targets cannot exist. */
    action = run_ex(&vs, ":w trail.txt   ");
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(arg_equals(&action, "trail.txt"));

    /* Whitespace only after the command is the bare command. */
    action = run_ex(&vs, ":w   ");
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(action.arg_len == 0);

    /* ":q" takes no argument. Real vim rejects ":q foo" rather than
       quietly ignoring the tail, and so does this -- someone who typed
       ":q report.txt" plainly meant something else. */
    action = run_ex(&vs, ":q report.txt");
    ASSERT_TRUE(action.kind == WP_VIACTION_BEEP);

    /* An over-long path is REJECTED, never truncated: a truncated path
       names a different file, and on a save that is silent data loss. */
    feed_keys(&vs, ":w ");
    for (i = 0; i < (u32)WP_VIMODE_ACTION_ARG_MAX + 1; i++) feed_keys(&vs, "a");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_BEEP);
    ASSERT_TRUE(action.arg_len == 0);

    /* Exactly at the cap still works -- an off-by-one here would reject
       a legal path. */
    feed_keys(&vs, ":w ");
    for (i = 0; i < (u32)WP_VIMODE_ACTION_ARG_MAX; i++) feed_keys(&vs, "b");
    action = feed_key_special(&vs, WP_KEY_ENTER);
    ASSERT_TRUE(action.kind == WP_VIACTION_SAVE);
    ASSERT_TRUE(action.arg_len == (u32)WP_VIMODE_ACTION_ARG_MAX);

    /* An unknown command with an argument is still unknown -- the
       argument parsing must not make ":bogus foo" look valid. */
    action = run_ex(&vs, ":bogus foo.txt");
    ASSERT_TRUE(action.kind == WP_VIACTION_BEEP);

    /* A command word that merely STARTS with a valid one is not that
       command: ":we foo" is not ":w". */
    action = run_ex(&vs, ":we foo.txt");
    ASSERT_TRUE(action.kind == WP_VIACTION_BEEP);

    wp_vimode_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}
