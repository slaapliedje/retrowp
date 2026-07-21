#include <string.h>
#include "test.h"
#include "emacs.h"
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

/* Same convention as test_wordstar.c's own feed_keys: any byte < 0x20
   => Ctrl+<letter>. A control byte immediately followed by a plain
   letter in the SAME string literal is a real C footgun (a `\xHH`
   escape greedily consumes trailing hex-digit characters -- see M10
   Phase 1's own account in docs/keymaps.md) -- every call site below
   that needs a control byte immediately followed by a plain letter
   splits into two adjacent string literals to stay unambiguous,
   whether or not that particular letter happens to be a hex digit. */
static void feed_keys(wp_emacs_state *vs, const char *script)
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

        wp_emacs_feed_key(vs, key);
    }
}

static wp_emacs_action_kind feed_key_special(wp_emacs_state *vs, wp_special_key special)
{
    wp_key_event key;
    key.codepoint = 0;
    key.special = (u16)special;
    key.mods = 0;
    key._pad = 0;
    return wp_emacs_feed_key(vs, key);
}

static wp_emacs_action_kind feed_key_full(wp_emacs_state *vs, u32 codepoint, u16 special, u8 mods)
{
    wp_key_event key;
    key.codepoint = codepoint;
    key.special = special;
    key.mods = mods;
    key._pad = 0;
    return wp_emacs_feed_key(vs, key);
}

void test_emacs_basic_char_and_line_motion(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "Hello", "World");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x02\x02\x02"); /* C-b C-b C-b -- char backward, clamped at doc start */
    ASSERT_EQ_U32(vs.cursor.offset, 0); /* backward from offset 0 stays at 0 */

    feed_keys(&vs, "\x06\x06"); /* C-f C-f -- char forward x2 */
    ASSERT_EQ_U32(vs.cursor.offset, 2);
    feed_keys(&vs, "\x02"); /* C-b -- char backward */
    ASSERT_EQ_U32(vs.cursor.offset, 1);

    feed_keys(&vs, "\x0e"); /* C-n -- next line/paragraph */
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    feed_keys(&vs, "\x10"); /* C-p -- previous line/paragraph */
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_line_start_and_end(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x05"); /* C-e -- end of line */
    ASSERT_EQ_U32(vs.cursor.offset, 11);
    feed_keys(&vs, "\x01"); /* C-a -- start of line */
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_meta_word_motion_via_alt_bit(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    /* M-f lands at the END of "Hello" (offset 5, right before the
       space) -- real Emacs's own forward-word landing, NOT vim's 'w'
       (which would skip the trailing space too, landing at 6). See
       kill_word_forward's own comment in emacs.c for the full account
       of this real, verified-the-hard-way distinction. */
    ASSERT_TRUE(feed_key_full(&vs, 'f', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-f */
    ASSERT_EQ_U32(vs.cursor.offset, 5);
    ASSERT_TRUE(feed_key_full(&vs, 'b', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-b */
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_meta_via_escape_prefix_fallback(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    /* Bare Escape means "await Meta", not cancel, in this scheme --
       see emacs.h's own file comment. ESC then plain 'f' == M-f. */
    ASSERT_TRUE(feed_key_special(&vs, WP_KEY_ESCAPE) == WP_EMACSACTION_NONE);
    ASSERT_TRUE(feed_key_full(&vs, 'f', (u16)WP_KEY_NONE, 0) == WP_EMACSACTION_NONE);
    ASSERT_EQ_U32(vs.cursor.offset, 5); /* end of "Hello" -- see test_emacs_meta_word_motion_via_alt_bit's own comment */

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_meta_doc_start_and_end(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "First", "Second");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, '>', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-> */
    ASSERT_EQ_U32(vs.cursor.paragraph, 1);
    ASSERT_TRUE(feed_key_full(&vs, '<', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-< */
    ASSERT_EQ_U32(vs.cursor.paragraph, 0);
    ASSERT_EQ_U32(vs.cursor.offset, 0);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_plain_typing_is_non_modal(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hello");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_ctrl_d_and_backspace_delete(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x04"); /* C-d -- delete char forward */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "ello");

    feed_key_special(&vs, WP_KEY_BACKSPACE); /* no-op at offset 0 */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "ello");

    feed_keys(&vs, "\x06"); /* C-f */
    feed_key_special(&vs, WP_KEY_BACKSPACE);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "llo");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_meta_d_kills_word_forward_and_ctrl_y_yanks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, 'd', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-d */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, " World");

    feed_keys(&vs, "\x05"); /* C-e -- end of line */
    ASSERT_TRUE(feed_key_full(&vs, 'y', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-y -- yank */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, " WorldHello");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_meta_backspace_kills_word_backward(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x05"); /* C-e -- end of line */
    ASSERT_TRUE(feed_key_full(&vs, 0, (u16)WP_KEY_BACKSPACE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-DEL */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello ");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_ctrl_k_kills_to_line_end_then_merges_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "Second");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x06\x06\x06\x06\x06"); /* C-f x5 -- cursor after "Hello" */
    feed_keys(&vs, "\x0b"); /* C-k -- kill to line end ("_World" gone) */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello");

    ASSERT_EQ_U32(doc.sections[0].para_count, 2);
    feed_keys(&vs, "\x0b"); /* C-k again, now AT line end -- merges with next paragraph */
    ASSERT_EQ_U32(doc.sections[0].para_count, 1);
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "HelloSecond");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_mark_and_kill_region(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    /* C-SPC is real Emacs's own binding but isn't reachable through
       either shipped backend's key translation (see emacs.h's own
       file comment) -- still fully host-testable, since this harness
       constructs the wp_key_event directly rather than going through
       a real backend's translation. */
    ASSERT_TRUE(feed_key_full(&vs, 0, (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-SPC */
    ASSERT_TRUE(vs.mark_set == WP_TRUE);
    ASSERT_EQ_U32(vs.mark.offset, 0);

    feed_keys(&vs, "\x06\x06\x06\x06\x06"); /* C-f x5 -- cursor after "Hello" */
    ASSERT_TRUE(feed_key_full(&vs, 'w', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-w -- kill region */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, " World");
    ASSERT_TRUE(vs.mark_set == WP_FALSE); /* deactivated after killing, matches real Emacs */

    ASSERT_TRUE(feed_key_full(&vs, 'y', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-y -- yank it back */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello World");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_kill_region_without_mark_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "Hello", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, 'w', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_BEEP);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_meta_w_copies_region_without_moving_cursor(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, 0, (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-SPC */
    feed_keys(&vs, "\x06\x06\x06\x06\x06"); /* C-f x5 */
    ASSERT_TRUE(feed_key_full(&vs, 'w', (u16)WP_KEY_NONE, (u8)WP_MOD_ALT) == WP_EMACSACTION_NONE); /* M-w */
    ASSERT_EQ_U32(vs.cursor.offset, 5); /* cursor unchanged by copy */

    feed_keys(&vs, "\x05"); /* C-e */
    ASSERT_TRUE(feed_key_full(&vs, 'y', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-y */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hello WorldHello");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_exchange_point_and_mark(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, 0, (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-SPC at offset 0 */
    feed_keys(&vs, "\x06\x06\x06\x06\x06"); /* C-f x5 -- cursor at offset 5 */

    feed_keys(&vs, "\x18"); /* C-x */
    ASSERT_TRUE(feed_key_full(&vs, 'x', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-x C-x */
    ASSERT_EQ_U32(vs.cursor.offset, 0);
    ASSERT_EQ_U32(vs.mark.offset, 5);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_ctrl_x_prefix_save_open_quit(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x18"); /* C-x */
    ASSERT_TRUE(feed_key_full(&vs, 's', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_SAVE); /* C-x C-s */

    feed_keys(&vs, "\x18"); /* C-x */
    ASSERT_TRUE(feed_key_full(&vs, 'f', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_OPEN); /* C-x C-f */

    feed_keys(&vs, "\x18"); /* C-x */
    ASSERT_TRUE(feed_key_full(&vs, 'c', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_QUIT); /* C-x C-c */

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_ctrl_x_u_undoes(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "Hi");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "Hi");

    feed_keys(&vs, "\x18"); /* C-x */
    ASSERT_TRUE(feed_key_full(&vs, 'u', (u16)WP_KEY_NONE, 0) == WP_EMACSACTION_NONE); /* C-x u -- plain 'u', no Ctrl */
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "H");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_ctrl_c_prefix_format_bindings(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x03"); /* C-c */
    ASSERT_TRUE(feed_key_full(&vs, 'b', (u16)WP_KEY_NONE, 0) == WP_EMACSACTION_FORMAT_BOLD);

    feed_keys(&vs, "\x03");
    ASSERT_TRUE(feed_key_full(&vs, 'i', (u16)WP_KEY_NONE, 0) == WP_EMACSACTION_FORMAT_ITALIC);

    feed_keys(&vs, "\x03");
    ASSERT_TRUE(feed_key_full(&vs, 'u', (u16)WP_KEY_NONE, 0) == WP_EMACSACTION_FORMAT_UNDERLINE);

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_ctrl_g_cancels_pending_prefix(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;
    char buf[64];

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    feed_keys(&vs, "\x18"); /* C-x */
    ASSERT_EQ_U32(vs.pending_prefix, 'x');
    feed_keys(&vs, "\x07"); /* C-g -- cancel */
    ASSERT_EQ_U32(vs.pending_prefix, 0);

    /* the pending C-x is fully gone -- a plain 's' now just types */
    feed_keys(&vs, "s");
    dump_para_text(&doc, 0, buf);
    ASSERT_EQ_STR(buf, "s");

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_unbound_chord_beeps(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    build_doc_two_paras(&doc, "", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, 'z', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_BEEP); /* C-z unbound */

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_emacs_no_leaks(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_emacs_state vs;

    mem_host_reset_counters();

    build_doc_two_paras(&doc, "Hello World", "");
    wp_undo_journal_init(&journal);
    wp_emacs_init(&vs, &doc, &journal);

    ASSERT_TRUE(feed_key_full(&vs, 0, (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-SPC */
    feed_keys(&vs, "\x06\x06\x06\x06\x06"); /* C-f x5 */
    ASSERT_TRUE(feed_key_full(&vs, 'w', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-w */
    ASSERT_TRUE(feed_key_full(&vs, 'y', (u16)WP_KEY_NONE, (u8)WP_MOD_CTRL) == WP_EMACSACTION_NONE); /* C-y */

    wp_emacs_free(&vs);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
