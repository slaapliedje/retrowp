/* SPDX-License-Identifier: GPL-2.0-or-later */
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

    vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
    vs->prompt_is_replace = WP_FALSE;
    vs->prompt_backward = WP_FALSE;
    vs->replace_confirm_each = WP_FALSE;
    vs->prompt_find_len = 0;
    vs->prompt_replace_len = 0;
    vs->scope_start = vs->cursor;
    vs->pending_match = vs->cursor;
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

/* ------------------------------------------------------------------ */
/* Search / Replace (F2, Shift-F2, Alt-F2) -- M11                       */
/* ------------------------------------------------------------------ */

/* Same "never wrap back past where this run started" reasoning as
   WordStar's own ws_find_next_in_scope (see wordstar.c) -- no G-
   equivalent here, so scope_start is always the cursor position at the
   moment Search/Replace began. No case-fold/whole-word option either:
   real WP 5.1's own Search prompt has no such toggle in the sources
   checked this pass (unlike WordStar's real OPTIONS? letters). */
static wp_bool wp5_find_next_in_scope(wp_wordperfect_state *vs, wp_pos from_pos, wp_pos *out)
{
    wp_pos found;
    wp_bool forward = (wp_bool)!vs->prompt_backward;

    if (wp_document_find_text(vs->doc, from_pos, vs->prompt_find, vs->prompt_find_len,
                               forward, WP_FALSE, WP_FALSE, &found) != WP_OK) {
        return WP_FALSE;
    }
    if (forward && wp_pos_compare(found, vs->scope_start) < 0) return WP_FALSE;
    if (!forward && wp_pos_compare(found, vs->scope_start) > 0) return WP_FALSE;
    *out = found;
    return WP_TRUE;
}

/* w/Confirm? answered N: replaces first_match and every remaining match
   in scope silently, as ONE undo group -- same continuation-position
   reasoning as WordStar's own ws_replace_all_remaining (forward
   continues from after the inserted text, backward from before it,
   both guarding against re-matching inside a replacement that itself
   contains the search text). */
static void wp5_replace_all_remaining(wp_wordperfect_state *vs, wp_pos first_match)
{
    wp_pos match = first_match;
    wp_bool forward = (wp_bool)!vs->prompt_backward;

    wp_undo_group_begin(vs->journal);
    for (;;) {
        wp_selection sel = wp_document_match_selection(vs->doc, match, vs->prompt_find_len);
        wp_pos before_insert, out, next, continue_from;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->prompt_replace,
                            vs->prompt_replace_len, &out);
        vs->cursor = out;

        continue_from = forward ? out : before_insert;
        if (!wp5_find_next_in_scope(vs, continue_from, &next)) break;
        match = next;
    }
    wp_undo_group_end(vs->journal);
}

/* Runs the Search/Replace this scheme's own F2/Shift-F2/Alt-F2 flow
   just collected: finds the first match from the cursor, and either
   jumps there (plain Search), silently batch-replaces the rest of scope
   (Replace with w/Confirm? answered N), or pauses awaiting a per-match
   Y/N (w/Confirm? answered Y). */
static wp_wordperfect_action_kind wp5_begin_search(wp_wordperfect_state *vs)
{
    wp_pos found;

    vs->scope_start = vs->cursor;

    if (!wp5_find_next_in_scope(vs, vs->scope_start, &found)) return WP_WPACTION_BEEP;

    vs->cursor = found;

    if (!vs->prompt_is_replace) return WP_WPACTION_NONE; /* plain Search: one jump, done */

    if (!vs->replace_confirm_each) {
        wp5_replace_all_remaining(vs, found);
        return WP_WPACTION_NONE;
    }

    vs->pending_match = found;
    vs->prompt_stage = (u8)WP_WP_PROMPT_CONFIRM_MATCH;
    return WP_WPACTION_NONE;
}

static wp_wordperfect_action_kind feed_prompt_replace_confirm(wp_wordperfect_state *vs, wp_key_event key)
{
    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
        return WP_WPACTION_NONE;
    }
    if (key.special != (u16)WP_KEY_NONE) return WP_WPACTION_NONE;

    if (key.codepoint == 'y' || key.codepoint == 'Y') {
        vs->replace_confirm_each = WP_TRUE;
        vs->prompt_stage = (u8)WP_WP_PROMPT_SEARCH_TEXT;
    } else if (key.codepoint == 'n' || key.codepoint == 'N') {
        vs->replace_confirm_each = WP_FALSE;
        vs->prompt_stage = (u8)WP_WP_PROMPT_SEARCH_TEXT;
    }
    return WP_WPACTION_NONE; /* anything else: stay at the prompt */
}

/* SEARCH_TEXT and REPLACE_TEXT fields share this shape -- collect
   printable characters, Backspace/Escape handled uniformly, but
   terminated by F2 (real WP's own Search key), not Enter -- the one
   structural difference from every other scheme's own text-collection
   prompt (WordStar/vim/Emacs/MS Word all use Enter). Deliberately does
   NOT clear buf/len on entry (see wp_wordperfect_state's own struct
   comment on why: that persistence is what makes an immediate,
   untouched F2 press repeat the last search). */
static wp_wordperfect_action_kind feed_prompt_text(wp_wordperfect_state *vs, wp_key_event key,
                                                    u8 *buf, u32 *len, wp_bool is_find_stage)
{
    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
        return WP_WPACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (*len > 0) (*len)--;
        else vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
        return WP_WPACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_F2) {
        if (is_find_stage) {
            if (*len == 0) {
                vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
                return WP_WPACTION_BEEP;
            }
            if (vs->prompt_is_replace) {
                vs->prompt_stage = (u8)WP_WP_PROMPT_REPLACE_TEXT;
                return WP_WPACTION_NONE;
            }
            vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
            return wp5_begin_search(vs);
        }
        /* REPLACE_TEXT field: an empty replacement is a real, legitimate
           "search and delete," same as every other scheme's own Replace. */
        vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
        return wp5_begin_search(vs);
    }
    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (*len < WP_WORDPERFECT_PROMPT_MAX - 1) buf[(*len)++] = (u8)key.codepoint;
    }
    return WP_WPACTION_NONE;
}

static wp_wordperfect_action_kind feed_prompt_confirm_match(wp_wordperfect_state *vs, wp_key_event key)
{
    wp_bool forward;
    wp_pos continue_from, next;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
        return WP_WPACTION_NONE;
    }
    if (key.special != (u16)WP_KEY_NONE) return WP_WPACTION_NONE;

    forward = (wp_bool)!vs->prompt_backward;

    if (key.codepoint == 'y' || key.codepoint == 'Y') {
        wp_selection sel = wp_document_match_selection(vs->doc, vs->pending_match, vs->prompt_find_len);
        wp_pos before_insert, out;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->prompt_replace,
                            vs->prompt_replace_len, &out);
        vs->cursor = out;
        continue_from = forward ? out : before_insert;
    } else if (key.codepoint == 'n' || key.codepoint == 'N') {
        continue_from = vs->pending_match;
    } else {
        return WP_WPACTION_NONE; /* not y/n -- stay paused */
    }

    vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
    if (!wp5_find_next_in_scope(vs, continue_from, &next)) return WP_WPACTION_NONE;

    vs->cursor = next;
    vs->pending_match = next;
    vs->prompt_stage = (u8)WP_WP_PROMPT_CONFIRM_MATCH;
    return WP_WPACTION_NONE;
}

static wp_wordperfect_action_kind feed_prompt(wp_wordperfect_state *vs, wp_key_event key)
{
    switch ((wp_wordperfect_prompt_stage)vs->prompt_stage) {
    case WP_WP_PROMPT_REPLACE_CONFIRM:
        return feed_prompt_replace_confirm(vs, key);
    case WP_WP_PROMPT_SEARCH_TEXT:
        return feed_prompt_text(vs, key, vs->prompt_find, &vs->prompt_find_len, WP_TRUE);
    case WP_WP_PROMPT_REPLACE_TEXT:
        return feed_prompt_text(vs, key, vs->prompt_replace, &vs->prompt_replace_len, WP_FALSE);
    case WP_WP_PROMPT_CONFIRM_MATCH:
        return feed_prompt_confirm_match(vs, key);
    default:
        vs->prompt_stage = (u8)WP_WP_PROMPT_NONE;
        return WP_WPACTION_NONE;
    }
}

wp_wordperfect_action_kind wp_wordperfect_feed_key(wp_wordperfect_state *vs, wp_key_event key)
{
    if (vs->prompt_stage != (u8)WP_WP_PROMPT_NONE) return feed_prompt(vs, key);

    switch (key.special) {
    case WP_KEY_F1: /* Undelete -- single-level, see wordperfect.h's own file comment */
        wp_edit_undo(vs->doc, vs->journal);
        vs->cursor = wp_pos_clamp_to_document(vs->doc, vs->cursor);
        return WP_WPACTION_NONE;
    case WP_KEY_F2: /* Search / Replace -- see wordperfect.h's own file comment */
        if (key.mods & (u8)WP_MOD_ALT) {
            vs->prompt_is_replace = WP_TRUE;
            vs->prompt_backward = WP_FALSE;
            vs->prompt_stage = (u8)WP_WP_PROMPT_REPLACE_CONFIRM;
            return WP_WPACTION_NONE;
        }
        if (key.mods & (u8)WP_MOD_SHIFT) {
            vs->prompt_is_replace = WP_FALSE;
            vs->prompt_backward = WP_TRUE;
            vs->prompt_stage = (u8)WP_WP_PROMPT_SEARCH_TEXT;
            return WP_WPACTION_NONE;
        }
        if (key.mods == 0) {
            vs->prompt_is_replace = WP_FALSE;
            vs->prompt_backward = WP_FALSE;
            vs->prompt_stage = (u8)WP_WP_PROMPT_SEARCH_TEXT;
            return WP_WPACTION_NONE;
        }
        return WP_WPACTION_BEEP;
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

void wp_wordperfect_get_status_text(const wp_wordperfect_state *vs, char *out, u32 out_cap)
{
    const char *label;
    const u8 *buf = NULL;
    u32 buf_len = 0;
    u32 len = 0;

    if (out_cap == 0) return;

    switch ((wp_wordperfect_prompt_stage)vs->prompt_stage) {
    case WP_WP_PROMPT_REPLACE_CONFIRM:
        label = "w/Confirm? (Y/N)"; break;
    case WP_WP_PROMPT_SEARCH_TEXT:
        label = vs->prompt_backward ? "<- Srch:" : "-> Srch:";
        buf = vs->prompt_find; buf_len = vs->prompt_find_len;
        break;
    case WP_WP_PROMPT_REPLACE_TEXT:
        label = "Replace with:";
        buf = vs->prompt_replace; buf_len = vs->prompt_replace_len;
        break;
    case WP_WP_PROMPT_CONFIRM_MATCH:
        label = "Replace? (Y/N)"; break;
    case WP_WP_PROMPT_NONE:
    default:
        label = "WORDPERFECT"; break;
    }

    while (label[len] != '\0' && len < out_cap - 1) { out[len] = label[len]; len++; }
    if (buf != NULL) {
        u32 i;
        if (len < out_cap - 1) out[len++] = ' ';
        for (i = 0; i < buf_len && len < out_cap - 1; i++) out[len++] = (char)buf[i];
    }
    out[len] = '\0';
}
