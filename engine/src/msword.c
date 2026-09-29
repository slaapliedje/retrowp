/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "msword.h"
#include "utf8.h"

/* See msword.h for the full design account. */

void wp_msword_init(wp_msword_state *vs, wp_document *doc, wp_undo_journal *journal)
{
    vs->doc = doc;
    vs->journal = journal;
    vs->cursor = wp_pos_doc_start(doc);
    vs->selecting = WP_FALSE;

    vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
    vs->prompt_is_replace = WP_FALSE;
    vs->find_len = 0;
    vs->replace_len = 0;
    vs->last_find_len = 0;
    vs->scope_start = vs->cursor;
    vs->pending_match = vs->cursor;
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
    case 'q': return WP_MSACTION_QUIT;
    case 'c': return WP_MSACTION_COPY;
    case 'x': return WP_MSACTION_CUT;
    case 'v': return WP_MSACTION_PASTE;
    case 'f': /* Find -- see msword.h's own file comment */
        vs->prompt_is_replace = WP_FALSE;
        vs->find_len = 0;
        vs->prompt_stage = (u8)WP_MS_PROMPT_FIND_TEXT;
        return WP_MSACTION_NONE;
    case 'h': /* Replace */
        vs->prompt_is_replace = WP_TRUE;
        vs->find_len = 0;
        vs->replace_len = 0;
        vs->prompt_stage = (u8)WP_MS_PROMPT_FIND_TEXT;
        return WP_MSACTION_NONE;
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

/* ------------------------------------------------------------------ */
/* Find / Replace (Ctrl+F, Ctrl+H, Shift+F4) -- M11                     */
/* ------------------------------------------------------------------ */

/* bounded scopes the search to never cross back past vs->scope_start --
   Replace's own confirm loop needs this (see ws_find_next_in_scope's
   own comment in wordstar.c for the "never re-cover ground already
   passed" reasoning); plain Find (Ctrl+F/Shift+F4, always called with
   bounded=WP_FALSE) instead gets wp_document_find_text's own natural
   single-wrap-and-stop behavior, the same shape vi's own `/` uses. */
static wp_bool ms_find(wp_msword_state *vs, wp_pos from_pos, const u8 *needle, u32 needle_len,
                        wp_bool bounded, wp_pos *out)
{
    wp_pos found;

    if (wp_document_find_text(vs->doc, from_pos, needle, needle_len,
                               WP_TRUE, WP_FALSE, WP_FALSE, &found) != WP_OK) {
        return WP_FALSE;
    }
    if (bounded && wp_pos_compare(found, vs->scope_start) < 0) return WP_FALSE;
    *out = found;
    return WP_TRUE;
}

/* 'a' (real "Replace All"): replaces first_match and every remaining
   match in scope silently, as ONE undo group. Forward-only (see
   msword.h's own file comment on why this scheme has no G-equivalent),
   so continuation is always "after the inserted text" -- still guards
   against an infinite loop when the replacement text itself contains
   the search pattern, same reasoning every other scheme's own
   replace-all helper documents. */
static void ms_replace_all_remaining(wp_msword_state *vs, wp_pos first_match)
{
    wp_pos match = first_match;

    wp_undo_group_begin(vs->journal);
    for (;;) {
        wp_selection sel = wp_document_match_selection(vs->doc, match, vs->find_len);
        wp_pos before_insert, out, next;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->replace_buf,
                            vs->replace_len, &out);
        vs->cursor = out;

        if (!ms_find(vs, out, vs->find_buf, vs->find_len, WP_TRUE, &next)) break;
        match = next;
    }
    wp_undo_group_end(vs->journal);
}

/* Both Find/Replace fields submitted: jumps to the first match and
   pauses in REPLACE_CONFIRM, standing in for real Word's own persistent
   dialog showing the first hit highlighted and ready for Replace/
   Replace All/Find Next/Close (see msword.h's own file comment). */
static wp_msword_action_kind ms_begin_replace(wp_msword_state *vs)
{
    wp_pos found;

    vs->scope_start = vs->cursor;
    if (!ms_find(vs, vs->scope_start, vs->find_buf, vs->find_len, WP_TRUE, &found)) {
        vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        return WP_MSACTION_BEEP;
    }

    vs->cursor = found;
    vs->pending_match = found;
    vs->prompt_stage = (u8)WP_MS_PROMPT_REPLACE_CONFIRM;
    return WP_MSACTION_NONE;
}

/* FIND_TEXT and REPLACE_TEXT fields -- Enter-terminated (real Word's
   own dialog convention), Backspace/Escape handled the same way every
   other scheme's own text-collection prompt does. Always starts empty
   on entry (see msword.h's own file comment on why, and how Shift+F4
   covers repeat instead). */
static wp_msword_action_kind feed_prompt_text(wp_msword_state *vs, wp_key_event key,
                                               wp_bool is_find_stage)
{
    u8  *buf = is_find_stage ? vs->find_buf : vs->replace_buf;
    u32 *len = is_find_stage ? &vs->find_len : &vs->replace_len;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        return WP_MSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (*len > 0) (*len)--;
        else vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        return WP_MSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        if (is_find_stage) {
            wp_pos found;

            if (*len == 0) {
                vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
                return WP_MSACTION_BEEP;
            }
            memcpy(vs->last_find, vs->find_buf, vs->find_len);
            vs->last_find_len = vs->find_len;

            if (vs->prompt_is_replace) {
                vs->prompt_stage = (u8)WP_MS_PROMPT_REPLACE_TEXT;
                return WP_MSACTION_NONE;
            }

            vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
            if (!ms_find(vs, vs->cursor, vs->find_buf, vs->find_len, WP_FALSE, &found)) {
                return WP_MSACTION_BEEP;
            }
            vs->cursor = found;
            return WP_MSACTION_NONE;
        }
        vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        return ms_begin_replace(vs);
    }
    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (*len < WP_MSWORD_PROMPT_MAX - 1) buf[(*len)++] = (u8)key.codepoint;
    }
    return WP_MSACTION_NONE;
}

/* 'r' replace+advance (real "Replace"), 'n' skip+advance (real "Find
   Next"), 'a' replace all remaining silently (real "Replace All"),
   Escape closes (real "Close"). */
static wp_msword_action_kind feed_replace_confirm(wp_msword_state *vs, wp_key_event key)
{
    wp_pos continue_from, next;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        return WP_MSACTION_NONE;
    }
    if (key.special != (u16)WP_KEY_NONE) return WP_MSACTION_NONE;

    if (key.codepoint == 'r' || key.codepoint == 'R') {
        wp_selection sel = wp_document_match_selection(vs->doc, vs->pending_match, vs->find_len);
        wp_pos before_insert, out;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->replace_buf,
                            vs->replace_len, &out);
        vs->cursor = out;
        continue_from = out;
    } else if (key.codepoint == 'n' || key.codepoint == 'N') {
        continue_from = vs->pending_match;
    } else if (key.codepoint == 'a' || key.codepoint == 'A') {
        vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        ms_replace_all_remaining(vs, vs->pending_match);
        return WP_MSACTION_NONE;
    } else {
        return WP_MSACTION_NONE; /* unrecognized: stay paused */
    }

    vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
    if (!ms_find(vs, continue_from, vs->find_buf, vs->find_len, WP_TRUE, &next)) {
        return WP_MSACTION_NONE;
    }
    vs->cursor = next;
    vs->pending_match = next;
    vs->prompt_stage = (u8)WP_MS_PROMPT_REPLACE_CONFIRM;
    return WP_MSACTION_NONE;
}

static wp_msword_action_kind feed_prompt(wp_msword_state *vs, wp_key_event key)
{
    switch ((wp_msword_prompt_stage)vs->prompt_stage) {
    case WP_MS_PROMPT_FIND_TEXT:
        return feed_prompt_text(vs, key, WP_TRUE);
    case WP_MS_PROMPT_REPLACE_TEXT:
        return feed_prompt_text(vs, key, WP_FALSE);
    case WP_MS_PROMPT_REPLACE_CONFIRM:
        return feed_replace_confirm(vs, key);
    default:
        vs->prompt_stage = (u8)WP_MS_PROMPT_NONE;
        return WP_MSACTION_NONE;
    }
}

wp_msword_action_kind wp_msword_feed_key(wp_msword_state *vs, wp_key_event key)
{
    wp_bool extend = (wp_bool)((key.mods & (u8)WP_MOD_SHIFT) != 0);
    wp_bool ctrl = (wp_bool)((key.mods & (u8)WP_MOD_CTRL) != 0);

    if (vs->prompt_stage != (u8)WP_MS_PROMPT_NONE) return feed_prompt(vs, key);

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
    case WP_KEY_F4:
        if (extend) { /* Shift+F4: repeat the last executed Find verbatim */
            wp_pos found;
            if (vs->last_find_len == 0) return WP_MSACTION_BEEP;
            if (!ms_find(vs, vs->cursor, vs->last_find, vs->last_find_len, WP_FALSE, &found)) {
                return WP_MSACTION_BEEP;
            }
            vs->cursor = found;
            return WP_MSACTION_NONE;
        }
        return WP_MSACTION_BEEP; /* plain F4 unbound in this scheme */
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

void wp_msword_get_status_text(const wp_msword_state *vs, char *out, u32 out_cap)
{
    const char *label;
    const u8 *buf = NULL;
    u32 buf_len = 0;
    u32 len = 0;

    if (out_cap == 0) return;

    switch ((wp_msword_prompt_stage)vs->prompt_stage) {
    case WP_MS_PROMPT_FIND_TEXT:
        label = "Find:"; buf = vs->find_buf; buf_len = vs->find_len; break;
    case WP_MS_PROMPT_REPLACE_TEXT:
        label = "Replace with:"; buf = vs->replace_buf; buf_len = vs->replace_len; break;
    case WP_MS_PROMPT_REPLACE_CONFIRM:
        label = "Replace (r/a/n)?"; break;
    case WP_MS_PROMPT_NONE:
    default:
        label = "WORD"; break;
    }

    while (label[len] != '\0' && len < out_cap - 1) { out[len] = label[len]; len++; }
    if (buf != NULL) {
        u32 i;
        if (len < out_cap - 1) out[len++] = ' ';
        for (i = 0; i < buf_len && len < out_cap - 1; i++) out[len++] = (char)buf[i];
    }
    out[len] = '\0';
}
