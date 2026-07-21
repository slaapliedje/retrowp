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
