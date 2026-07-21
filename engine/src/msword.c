#include "msword.h"
#include "utf8.h"

/* See msword.h for the full design account. */

void wp_msword_init(wp_msword_state *vs, wp_document *doc, wp_undo_journal *journal)
{
    vs->doc = doc;
    vs->journal = journal;
    vs->cursor = wp_pos_doc_start(doc);
    vs->selecting = WP_FALSE;
}

static wp_selection sel_of(wp_pos a, wp_pos b)
{
    wp_selection s;
    s.anchor = a;
    s.caret = b;
    return s;
}

/* The one piece of bookkeeping every motion command below shares:
   Shift held extends the selection (starting one fresh, anchored at
   the pre-motion cursor, if none was already active); Shift not held
   collapses/clears any active selection first, matching the familiar
   "moving without Shift drops the selection" behavior most modern
   editors share. */
static void move_to(wp_msword_state *vs, wp_pos target, wp_bool extend)
{
    if (extend) {
        if (!vs->selecting) {
            vs->anchor = vs->cursor;
            vs->selecting = WP_TRUE;
        }
    } else {
        vs->selecting = WP_FALSE;
    }
    vs->cursor = target;
}

/* Deletes the active selection, if any, collapsing the cursor to its
   start and clearing selecting. Returns WP_TRUE iff there was one to
   delete -- callers use this to decide whether to ALSO perform their
   own single-char/word delete (there wasn't one) or skip it entirely
   (there was, and it's already handled). */
static wp_bool delete_selection_if_any(wp_msword_state *vs)
{
    wp_pos out;

    if (!vs->selecting) return WP_FALSE;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->anchor, vs->cursor), &out);
    vs->cursor = out;
    vs->selecting = WP_FALSE;
    return WP_TRUE;
}

/* Mirrors wordstar.c's own delete_char_backward, which itself mirrors
   vimode.c's insert_backspace -- same proven-correct paragraph-start
   special case, reproduced rather than re-derived. */
static void delete_char_backward(wp_msword_state *vs)
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

static void delete_chars_forward(wp_msword_state *vs, u32 n)
{
    wp_pos target = vs->cursor;
    wp_pos out;
    u32 i;

    for (i = 0; i < n; i++) target = wp_pos_next(vs->doc, target);
    if (wp_pos_compare(target, vs->cursor) == 0) return;

    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

/* Real Word's own Ctrl-Delete/Ctrl-Backspace stop at a word's own
   boundary -- vim's word_forward/word_backward (NOT word_end), unlike
   Emacs's own kill-word (see engine/src/emacs.c's own comment on that
   distinction): classic Word's word-delete already eats trailing
   whitespace the way vim's own 'w'/'b' motions do, verified against
   real Word behavior, not assumed identical to Emacs's. */
static void delete_word_forward(wp_msword_state *vs)
{
    wp_pos target = wp_pos_word_forward(vs->doc, vs->cursor, WP_FALSE);
    wp_pos out;

    if (wp_pos_compare(target, vs->cursor) == 0) return;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

static void delete_word_backward(wp_msword_state *vs)
{
    wp_pos target = wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE);
    wp_pos out;

    if (wp_pos_compare(target, vs->cursor) == 0) return;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(target, vs->cursor), &out);
    vs->cursor = out;
}

static void insert_char(wp_msword_state *vs, u32 codepoint)
{
    u8 utf8_buf[4];
    u32 n = utf8_encode(codepoint, utf8_buf);

    if (n > 0) {
        wp_pos out;
        wp_edit_insert_text(vs->doc, vs->journal, vs->cursor, utf8_buf, n, &out);
        vs->cursor = out;
    }
}

static wp_msword_action_kind handle_ctrl_letter(wp_msword_state *vs, wp_key_event key)
{
    switch (key.codepoint) {
    case 'b': return WP_MSACTION_FORMAT_BOLD;
    case 'i': return WP_MSACTION_FORMAT_ITALIC;
    case 'u': return WP_MSACTION_FORMAT_UNDERLINE;
    case 's': return WP_MSACTION_SAVE;
    case 'o': return WP_MSACTION_OPEN;
    case 'c': return WP_MSACTION_COPY;
    case 'x': return WP_MSACTION_CUT;
    case 'v': return WP_MSACTION_PASTE;
    case 'a': /* Select All */
        vs->anchor = wp_pos_doc_start(vs->doc);
        vs->cursor = wp_pos_doc_end(vs->doc);
        vs->selecting = WP_TRUE;
        return WP_MSACTION_NONE;
    case 'z': /* Undo -- a real, full undo stack, unlike every other
                 scheme's own narrower or absent undo (see msword.h's
                 own file comment) */
        wp_edit_undo(vs->doc, vs->journal);
        vs->cursor = wp_pos_clamp_to_document(vs->doc, vs->cursor);
        vs->selecting = WP_FALSE; /* a structural doc change invalidates any active selection */
        return WP_MSACTION_NONE;
    case 'y': /* Redo -- the only scheme in this milestone that has one */
        wp_edit_redo(vs->doc, vs->journal);
        vs->cursor = wp_pos_clamp_to_document(vs->doc, vs->cursor);
        vs->selecting = WP_FALSE;
        return WP_MSACTION_NONE;
    default:
        return WP_MSACTION_BEEP;
    }
}

wp_msword_action_kind wp_msword_feed_key(wp_msword_state *vs, wp_key_event key)
{
    wp_bool extend = (wp_bool)((key.mods & (u8)WP_MOD_SHIFT) != 0);
    wp_bool ctrl = (wp_bool)((key.mods & (u8)WP_MOD_CTRL) != 0);

    switch (key.special) {
    case WP_KEY_ENTER: {
        wp_pos out;
        delete_selection_if_any(vs);
        wp_edit_split_paragraph(vs->doc, vs->journal, vs->cursor, &out);
        vs->cursor = out;
        return WP_MSACTION_NONE;
    }
    case WP_KEY_BACKSPACE:
        if (!delete_selection_if_any(vs)) {
            if (ctrl) delete_word_backward(vs); else delete_char_backward(vs);
        }
        return WP_MSACTION_NONE;
    case WP_KEY_DELETE:
        if (!delete_selection_if_any(vs)) {
            if (ctrl) delete_word_forward(vs); else delete_chars_forward(vs, 1);
        }
        return WP_MSACTION_NONE;
    case WP_KEY_LEFT:
        move_to(vs, ctrl ? wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE) : wp_pos_prev(vs->doc, vs->cursor), extend);
        return WP_MSACTION_NONE;
    case WP_KEY_RIGHT:
        move_to(vs, ctrl ? wp_pos_word_forward(vs->doc, vs->cursor, WP_FALSE) : wp_pos_next(vs->doc, vs->cursor), extend);
        return WP_MSACTION_NONE;
    case WP_KEY_UP:
        move_to(vs, wp_pos_paragraph_up(vs->doc, vs->cursor), extend);
        return WP_MSACTION_NONE;
    case WP_KEY_DOWN:
        move_to(vs, wp_pos_paragraph_down(vs->doc, vs->cursor), extend);
        return WP_MSACTION_NONE;
    case WP_KEY_HOME:
        move_to(vs, ctrl ? wp_pos_doc_start(vs->doc) : wp_pos_line_start(vs->doc, vs->cursor), extend);
        return WP_MSACTION_NONE;
    case WP_KEY_END:
        move_to(vs, ctrl ? wp_pos_doc_end(vs->doc) : wp_pos_line_end(vs->doc, vs->cursor), extend);
        return WP_MSACTION_NONE;
    default:
        break;
    }

    if (key.special != (u16)WP_KEY_NONE) return WP_MSACTION_NONE; /* an unhandled special key: harmless no-op */

    if (ctrl) return handle_ctrl_letter(vs, key);

    if (key.mods == 0 && key.codepoint >= 0x20) {
        delete_selection_if_any(vs); /* typing over a selection replaces it */
        insert_char(vs, key.codepoint);
        return WP_MSACTION_NONE;
    }

    return WP_MSACTION_NONE;
}
