#include <stddef.h>
#include "wordperfect.h"
#include "edit_internal.h"
#include "platform.h"
#include "utf8.h"

/* See wordperfect.h for the full design account. */

void wp_wordperfect_init(wp_wordperfect_state *vs, wp_document *doc, wp_undo_journal *journal)
{
    vs->doc = doc;
    vs->journal = journal;
    vs->cursor = wp_pos_doc_start(doc);
    vs->block_active = WP_FALSE;
    vs->block_bytes = NULL;
    vs->block_len = 0;
    vs->block_cap = 0;
}

void wp_wordperfect_free(wp_wordperfect_state *vs)
{
    mem_free(vs->block_bytes);
    vs->block_bytes = NULL;
    vs->block_len = 0;
    vs->block_cap = 0;
}

static wp_selection sel_of(wp_pos a, wp_pos b)
{
    wp_selection s;
    s.anchor = a;
    s.caret = b;
    return s;
}

/* Mirrors wp_emacs_state's own save_kill/wp_vimode_state's own
   yank_selection exactly -- see either's own comment on the "size,
   grow if needed, copy" two-call wp_document_extract_text idiom. */
static void save_block(wp_wordperfect_state *vs, wp_selection sel)
{
    u32 needed;

    wp_document_extract_text(vs->doc, sel, NULL, 0, &needed);
    if (needed > vs->block_cap) {
        u8 *new_buf = (u8 *)mem_alloc(needed);
        if (new_buf == NULL) return; /* OOM: register left unchanged */
        mem_free(vs->block_bytes);
        vs->block_bytes = new_buf;
        vs->block_cap = needed;
    }
    wp_document_extract_text(vs->doc, sel, vs->block_bytes, vs->block_cap, &vs->block_len);
}

/* Mirrors wordstar.c's own delete_char_backward, which itself mirrors
   vimode.c's insert_backspace -- same proven-correct paragraph-start
   special case, reproduced rather than re-derived. */
static void delete_char_backward(wp_wordperfect_state *vs)
{
    if (vs->cursor.run == 0 && vs->cursor.offset == 0 && vs->cursor.paragraph > 0) {
        wp_pos out;
        wp_edit_merge_paragraph(vs->doc, vs->journal, vs->cursor.section, vs->cursor.paragraph - 1, &out);
        vs->cursor = out;
    } else {
        wp_pos prev = wp_pos_prev(vs->doc, vs->cursor);
        if (wp_pos_compare(prev, vs->cursor) != 0) {
            wp_pos out;
            wp_edit_delete_selection(vs->doc, vs->journal, sel_of(prev, vs->cursor), &out);
            vs->cursor = out;
        }
    }
}

static void delete_chars_forward(wp_wordperfect_state *vs, u32 n)
{
    wp_pos target = vs->cursor;
    wp_pos out;
    u32 i;

    for (i = 0; i < n; i++) target = wp_pos_next(vs->doc, target);
    if (wp_pos_compare(target, vs->cursor) == 0) return;

    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

static void delete_word_backward(wp_wordperfect_state *vs)
{
    wp_pos target = wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE);
    wp_pos out;

    if (wp_pos_compare(target, vs->cursor) == 0) return;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(target, vs->cursor), &out);
    vs->cursor = out;
}

/* Ctrl-End: real WP 5.1's own binding for "delete to end of line" --
   NOT document-end (see wordperfect.h's own file comment on why
   document-end has no binding here at all). */
static void delete_to_line_end(wp_wordperfect_state *vs)
{
    wp_pos end = wp_pos_line_end(vs->doc, vs->cursor);
    wp_pos out;

    if (wp_pos_compare(end, vs->cursor) == 0) return;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, end), &out);
    vs->cursor = out;
}

/* Ctrl-F4: the flattened "Move" block operation -- deletes the active
   block into the register (like a cut) and deactivates block mode.
   See wordperfect.h's own file comment on why this is the one
   flattened action standing in for real WP's own Copy/Move/Delete
   3-way menu. */
static void move_block(wp_wordperfect_state *vs)
{
    wp_pos begin, end, out;

    if (wp_pos_compare(vs->block_start, vs->cursor) <= 0) { begin = vs->block_start; end = vs->cursor; }
    else { begin = vs->cursor; end = vs->block_start; }
    if (wp_pos_compare(begin, end) == 0) { vs->block_active = WP_FALSE; return; }

    save_block(vs, sel_of(begin, end));
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(begin, end), &out);
    vs->cursor = out;
    vs->block_active = WP_FALSE;
}

/* Shift-F10: Retrieve -- pastes the register at the cursor, completing
   the Move workflow (mark block, Ctrl-F4 to cut, move cursor,
   Shift-F10 to paste). Mirrors wp_emacs_state's own yank exactly. */
static void retrieve(wp_wordperfect_state *vs)
{
    wp_pos out;
    wp_edit_paste_text(vs->doc, vs->journal, vs->cursor, vs->block_bytes, vs->block_len, &out);
    vs->cursor = out;
}

static void insert_char(wp_wordperfect_state *vs, u32 codepoint)
{
    u8 utf8_buf[4];
    u32 n = utf8_encode(codepoint, utf8_buf);

    if (n > 0) {
        wp_pos out;
        wp_edit_insert_text(vs->doc, vs->journal, vs->cursor, utf8_buf, n, &out);
        vs->cursor = out;
    }
}

wp_wordperfect_action_kind wp_wordperfect_feed_key(wp_wordperfect_state *vs, wp_key_event key)
{
    switch (key.special) {
    case WP_KEY_F1: /* Undelete -- single-level, see wordperfect.h's own file comment */
        wp_edit_undo(vs->doc, vs->journal);
        vs->cursor = wp_pos_clamp_to_document(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_F4:
        if (key.mods & (u8)WP_MOD_CTRL) {
            if (!vs->block_active) return WP_WPACTION_BEEP;
            move_block(vs);
            return WP_WPACTION_NONE;
        }
        if (key.mods & (u8)WP_MOD_ALT) {
            /* toggle: off->on marks the anchor here; on->off cancels,
               no operation performed -- real WP's own toggle semantic,
               distinct from vi's modal enter/exit and Emacs's
               set-and-persist (see wordperfect.h's own struct comment) */
            if (vs->block_active) {
                vs->block_active = WP_FALSE;
            } else {
                vs->block_start = vs->cursor;
                vs->block_active = WP_TRUE;
            }
            return WP_WPACTION_NONE;
        }
        return WP_WPACTION_BEEP;
    case WP_KEY_F6:
        if (key.mods == 0) return WP_WPACTION_FORMAT_BOLD;
        return WP_WPACTION_BEEP;
    case WP_KEY_F7:
        if (key.mods == 0) return WP_WPACTION_QUIT;
        return WP_WPACTION_BEEP;
    case WP_KEY_F8:
        if (key.mods == 0) return WP_WPACTION_FORMAT_UNDERLINE;
        return WP_WPACTION_BEEP;
    case WP_KEY_F10:
        if (key.mods == 0) return WP_WPACTION_SAVE;
        if (key.mods & (u8)WP_MOD_SHIFT) { /* Shift-F10: Retrieve */
            if (vs->block_len == 0) return WP_WPACTION_BEEP;
            retrieve(vs);
            return WP_WPACTION_NONE;
        }
        return WP_WPACTION_BEEP;
    case WP_KEY_ENTER: {
        wp_pos out;
        wp_edit_split_paragraph(vs->doc, vs->journal, vs->cursor, &out);
        vs->cursor = out;
        return WP_WPACTION_NONE;
    }
    case WP_KEY_BACKSPACE:
        if (key.mods & (u8)WP_MOD_CTRL) { delete_word_backward(vs); return WP_WPACTION_NONE; }
        delete_char_backward(vs);
        return WP_WPACTION_NONE;
    case WP_KEY_DELETE:
        delete_chars_forward(vs, 1);
        return WP_WPACTION_NONE;
    case WP_KEY_LEFT:
        vs->cursor = (key.mods & (u8)WP_MOD_CTRL)
            ? wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE)
            : wp_pos_prev(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_RIGHT:
        vs->cursor = (key.mods & (u8)WP_MOD_CTRL)
            ? wp_pos_word_forward(vs->doc, vs->cursor, WP_FALSE)
            : wp_pos_next(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_UP:
        vs->cursor = wp_pos_paragraph_up(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_DOWN:
        vs->cursor = wp_pos_paragraph_down(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_HOME:
        vs->cursor = (key.mods & (u8)WP_MOD_CTRL) ? wp_pos_doc_start(vs->doc) : wp_pos_line_start(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_END:
        if (key.mods & (u8)WP_MOD_CTRL) { delete_to_line_end(vs); return WP_WPACTION_NONE; }
        vs->cursor = wp_pos_line_end(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    default:
        break;
    }

    if (key.special != (u16)WP_KEY_NONE) return WP_WPACTION_NONE; /* an unhandled special key: harmless no-op */

    if (key.mods == 0 && key.codepoint >= 0x20) {
        insert_char(vs, key.codepoint);
        return WP_WPACTION_NONE;
    }

    return WP_WPACTION_NONE;
}
