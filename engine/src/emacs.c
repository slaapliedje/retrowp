#include <stddef.h>
#include "emacs.h"
#include "edit_internal.h"
#include "platform.h"
#include "utf8.h"

/* See emacs.h for the full design account. */

void wp_emacs_init(wp_emacs_state *vs, wp_document *doc, wp_undo_journal *journal)
{
    vs->doc = doc;
    vs->journal = journal;
    vs->cursor = wp_pos_doc_start(doc);
    vs->pending_prefix = 0;
    vs->pending_meta = WP_FALSE;
    vs->mark_set = WP_FALSE;
    vs->kill_bytes = NULL;
    vs->kill_len = 0;
    vs->kill_cap = 0;

    vs->isearch_mode = 0;
    vs->isearch_len = 0;
    vs->isearch_anchor = vs->cursor;
    vs->isearch_have_match = WP_FALSE;
    vs->isearch_last_match = vs->cursor;

    vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
    vs->qr_find_len = 0;
    vs->qr_replace_len = 0;
    vs->scope_start = vs->cursor;
    vs->pending_match = vs->cursor;
}

void wp_emacs_free(wp_emacs_state *vs)
{
    mem_free(vs->kill_bytes);
    vs->kill_bytes = NULL;
    vs->kill_len = 0;
    vs->kill_cap = 0;
}

static wp_selection sel_of(wp_pos a, wp_pos b)
{
    wp_selection s;
    s.anchor = a;
    s.caret = b;
    return s;
}

/* Mirrors vi-mode's own yank_selection (engine/src/vimode.c) exactly:
   the same "size, grow if needed, copy" two-call wp_document_extract_text
   idiom, just against this scheme's own single kill slot rather than
   vi's register. Not promoted to a shared helper: each scheme's own
   register/kill-slot field names differ enough (register_bytes/_len/
   _cap vs kill_bytes/_len/_cap) that sharing would need an indirection
   layer for no real benefit over these ~10 lines duplicated once. */
static void save_kill(wp_emacs_state *vs, wp_selection sel)
{
    u32 needed;

    wp_document_extract_text(vs->doc, sel, NULL, 0, &needed);
    if (needed > vs->kill_cap) {
        u8 *new_buf = (u8 *)mem_alloc(needed);
        if (new_buf == NULL) return; /* OOM: kill slot left unchanged */
        mem_free(vs->kill_bytes);
        vs->kill_bytes = new_buf;
        vs->kill_cap = needed;
    }
    wp_document_extract_text(vs->doc, sel, vs->kill_bytes, vs->kill_cap, &vs->kill_len);
}

/* Mirrors wordstar.c's own delete_char_backward, which itself mirrors
   vimode.c's insert_backspace -- same proven-correct paragraph-start
   special case, reproduced rather than re-derived. */
static void delete_char_backward(wp_emacs_state *vs)
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

static void delete_chars_forward(wp_emacs_state *vs, u32 n)
{
    wp_pos target = vs->cursor;
    wp_pos out;
    u32 i;

    for (i = 0; i < n; i++) target = wp_pos_next(vs->doc, target);
    if (wp_pos_compare(target, vs->cursor) == 0) return;

    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

/* Real Emacs kill-word (M-d) removes only the word itself, stopping at
   its own last character -- NOT vim's 'w' motion (wp_pos_word_forward),
   which deliberately skips through trailing whitespace to land at the
   START of the next word (a real, verified-the-hard-way distinction:
   killing "Hello World" at offset 0 must leave " World", not "World").
   wp_pos_word_end is the right primitive here, matching real Emacs's
   own "end of this word" landing exactly. Backward-kill (M-DEL,
   kill_word_backward below) doesn't have this problem: Emacs's own
   backward-word already lands at a word's START, the same place
   wp_pos_word_backward (vim's 'b') lands. */
static void kill_word_forward(wp_emacs_state *vs)
{
    wp_pos target = wp_pos_word_end(vs->doc, vs->cursor, WP_FALSE);
    wp_pos out;

    if (wp_pos_compare(target, vs->cursor) == 0) return;
    save_kill(vs, sel_of(vs->cursor, target));
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

static void kill_word_backward(wp_emacs_state *vs)
{
    wp_pos target = wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE);
    wp_pos out;

    if (wp_pos_compare(target, vs->cursor) == 0) return;
    save_kill(vs, sel_of(target, vs->cursor));
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(target, vs->cursor), &out);
    vs->cursor = out;
}

/* C-k: kills from cursor to line end, OR -- if the cursor is already
   AT line end -- kills the paragraph break itself (merges with the
   next paragraph), matching real Emacs's own two-press-on-a-non-empty-
   line behavior naturally: the first C-k always leaves the cursor
   exactly at line_end, so a second C-k right after correctly takes
   this second branch, with no extra "was the last command also C-k"
   state needed. A no-op on the section's last paragraph (nothing to
   merge into) -- matches real Emacs raising "End of buffer" and
   leaving the buffer alone. */
static void kill_line(wp_emacs_state *vs)
{
    wp_pos line_end = wp_pos_line_end(vs->doc, vs->cursor);

    if (wp_pos_compare(vs->cursor, line_end) != 0) {
        wp_pos out;
        save_kill(vs, sel_of(vs->cursor, line_end));
        wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, line_end), &out);
        vs->cursor = out;
    } else {
        const wp_section *sec = wp_document_section_get(vs->doc, vs->cursor.section);
        if (sec != NULL && vs->cursor.paragraph + 1 < sec->para_count) {
            wp_pos out;
            /* Real Emacs saves the newline itself into the kill ring
               here, so a subsequent C-y correctly re-splits a
               paragraph -- matches wp_document_extract_text's own
               '\n'-per-paragraph-boundary convention wp_edit_paste_text
               (editing.h) already expects on the read side. */
            if (vs->kill_cap < 1) {
                u8 *new_buf = (u8 *)mem_alloc(1);
                if (new_buf == NULL) return;
                mem_free(vs->kill_bytes);
                vs->kill_bytes = new_buf;
                vs->kill_cap = 1;
            }
            vs->kill_bytes[0] = '\n';
            vs->kill_len = 1;

            wp_edit_merge_paragraph(vs->doc, vs->journal, vs->cursor.section, vs->cursor.paragraph, &out);
            vs->cursor = out;
        }
    }
}

static void yank(wp_emacs_state *vs)
{
    wp_pos out;
    wp_edit_paste_text(vs->doc, vs->journal, vs->cursor, vs->kill_bytes, vs->kill_len, &out);
    vs->cursor = out;
}

static void kill_region(wp_emacs_state *vs)
{
    wp_pos out;
    save_kill(vs, sel_of(vs->mark, vs->cursor));
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->mark, vs->cursor), &out);
    vs->cursor = out;
    vs->mark_set = WP_FALSE; /* real Emacs deactivates the mark once the region is killed */
}

static void copy_region(wp_emacs_state *vs)
{
    save_kill(vs, sel_of(vs->mark, vs->cursor)); /* does not move point, matches real M-w */
}

static void exchange_point_and_mark(wp_emacs_state *vs)
{
    wp_pos tmp = vs->cursor;
    vs->cursor = vs->mark;
    vs->mark = tmp;
}

static void insert_char(wp_emacs_state *vs, u32 codepoint)
{
    u8 utf8_buf[4];
    u32 n = utf8_encode(codepoint, utf8_buf);

    if (n > 0) {
        wp_pos out;
        wp_edit_insert_text(vs->doc, vs->journal, vs->cursor, utf8_buf, n, &out);
        vs->cursor = out;
    }
}

/* Shared by both Meta-chord entry points (a direct Alt-held key, and
   the ESC-then-key historical fallback -- see wp_emacs_feed_key's own
   dispatch below and emacs.h's own file comment on why bare Escape
   means "await Meta" here). Deliberately does not itself inspect
   key.mods: a direct Alt-chord's ALT bit and the escape-prefix path's
   (usually unset) mods are both irrelevant here -- only codepoint/
   special decide the command, matching real Emacs's own M-<key>
   semantics regardless of which physical mechanism produced it. */
static wp_emacs_action_kind handle_meta_key(wp_emacs_state *vs, wp_key_event key)
{
    if (key.special == (u16)WP_KEY_BACKSPACE) { kill_word_backward(vs); return WP_EMACSACTION_NONE; }
    if (key.special != (u16)WP_KEY_NONE) return WP_EMACSACTION_BEEP;

    switch (key.codepoint) {
    case 'b': vs->cursor = wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE); return WP_EMACSACTION_NONE;
    case 'f': vs->cursor = wp_pos_word_end(vs->doc, vs->cursor, WP_FALSE); return WP_EMACSACTION_NONE; /* see kill_word_forward's own comment on why word_end, not word_forward */
    case 'd': kill_word_forward(vs); return WP_EMACSACTION_NONE;
    case '<': vs->cursor = wp_pos_doc_start(vs->doc); return WP_EMACSACTION_NONE;
    case '>': vs->cursor = wp_pos_doc_end(vs->doc); return WP_EMACSACTION_NONE;
    case '{': vs->cursor = wp_pos_para_block_backward(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case '}': vs->cursor = wp_pos_para_block_forward(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'w':
        if (!vs->mark_set) return WP_EMACSACTION_BEEP;
        copy_region(vs);
        return WP_EMACSACTION_NONE;
    case '%': /* M-% -- query-replace, see emacs.h's own file comment */
        vs->prompt_stage = (u8)WP_EMACS_PROMPT_FIND_TEXT;
        vs->qr_find_len = 0;
        vs->qr_replace_len = 0;
        return WP_EMACSACTION_NONE;
    default:
        return WP_EMACSACTION_BEEP;
    }
}

static wp_emacs_action_kind handle_prefix_key(wp_emacs_state *vs, wp_key_event key)
{
    u8 prefix = vs->pending_prefix;
    vs->pending_prefix = 0; /* single-level: always consumed by this one follow-up key */

    if (prefix == 'x') {
        if ((key.mods & (u8)WP_MOD_CTRL) && key.special == (u16)WP_KEY_NONE) {
            switch (key.codepoint) {
            case 's': return WP_EMACSACTION_SAVE;
            case 'f': return WP_EMACSACTION_OPEN;
            case 'c': return WP_EMACSACTION_QUIT;
            case 'x':
                if (!vs->mark_set) return WP_EMACSACTION_BEEP;
                exchange_point_and_mark(vs);
                return WP_EMACSACTION_NONE;
            default:
                return WP_EMACSACTION_BEEP;
            }
        }
        if (key.mods == 0 && key.special == (u16)WP_KEY_NONE && key.codepoint == 'u') {
            wp_edit_undo(vs->doc, vs->journal);
            vs->cursor = wp_pos_clamp_to_document(vs->doc, vs->cursor);
            return WP_EMACSACTION_NONE;
        }
        return WP_EMACSACTION_BEEP;
    }

    if (prefix == 'c') {
        if (key.mods == 0 && key.special == (u16)WP_KEY_NONE) {
            switch (key.codepoint) {
            case 'b': return WP_EMACSACTION_FORMAT_BOLD;
            case 'i': return WP_EMACSACTION_FORMAT_ITALIC;
            case 'u': return WP_EMACSACTION_FORMAT_UNDERLINE;
            default: return WP_EMACSACTION_BEEP;
            }
        }
        return WP_EMACSACTION_BEEP;
    }

    return WP_EMACSACTION_BEEP;
}

static wp_emacs_action_kind handle_ctrl_letter(wp_emacs_state *vs, wp_key_event key)
{
    switch (key.codepoint) {
    case 0: /* C-SPC / C-@ -- see emacs.h's own reachability caveat */
        vs->mark = vs->cursor;
        vs->mark_set = WP_TRUE;
        return WP_EMACSACTION_NONE;
    case 'b': vs->cursor = wp_pos_prev(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'f': vs->cursor = wp_pos_next(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'p': vs->cursor = wp_pos_paragraph_up(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'n': vs->cursor = wp_pos_paragraph_down(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'a': vs->cursor = wp_pos_line_start(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'e': vs->cursor = wp_pos_line_end(vs->doc, vs->cursor); return WP_EMACSACTION_NONE;
    case 'd': delete_chars_forward(vs, 1); return WP_EMACSACTION_NONE;
    case 'k': kill_line(vs); return WP_EMACSACTION_NONE;
    case 'w':
        if (!vs->mark_set) return WP_EMACSACTION_BEEP;
        kill_region(vs);
        return WP_EMACSACTION_NONE;
    case 'y':
        if (vs->kill_len == 0) return WP_EMACSACTION_BEEP;
        yank(vs);
        return WP_EMACSACTION_NONE;
    case 'g':
        vs->pending_prefix = 0;
        vs->pending_meta = WP_FALSE;
        return WP_EMACSACTION_NONE;
    case 'x': vs->pending_prefix = 'x'; return WP_EMACSACTION_NONE;
    case 'c': vs->pending_prefix = 'c'; return WP_EMACSACTION_NONE;
    case 's': /* C-s -- isearch-forward, see emacs.h's own file comment */
        vs->isearch_mode = (u8)'f';
        vs->isearch_anchor = vs->cursor;
        vs->isearch_len = 0;
        vs->isearch_have_match = WP_FALSE;
        return WP_EMACSACTION_NONE;
    case 'r': /* C-r -- isearch-backward */
        vs->isearch_mode = (u8)'r';
        vs->isearch_anchor = vs->cursor;
        vs->isearch_len = 0;
        vs->isearch_have_match = WP_FALSE;
        return WP_EMACSACTION_NONE;
    default:
        return WP_EMACSACTION_BEEP;
    }
}

/* ------------------------------------------------------------------ */
/* Incremental search (C-s / C-r) -- M11                                */
/* ------------------------------------------------------------------ */

/* Re-searches from from_pos with the current isearch_buf/len/direction.
   On success, moves point to the real Emacs landing spot -- the END of
   the match for forward isearch, the START for backward (a real,
   verified asymmetry, not an inconsistency) -- and records the match
   for a later repeat. On failure, deliberately leaves cursor/
   isearch_have_match/isearch_last_match untouched: real Emacs's own
   "Failing I-search" stays live and still-typeable rather than
   snapping back or exiting. */
static void isearch_run(wp_emacs_state *vs, wp_pos from_pos)
{
    wp_pos found;
    wp_bool forward = (wp_bool)(vs->isearch_mode == (u8)'f');

    if (vs->isearch_len == 0) return;

    if (wp_document_find_text(vs->doc, from_pos, vs->isearch_buf, vs->isearch_len,
                               forward, WP_FALSE, WP_FALSE, &found) != WP_OK) {
        return;
    }

    vs->isearch_have_match = WP_TRUE;
    vs->isearch_last_match = found;
    vs->cursor = forward ? wp_document_match_selection(vs->doc, found, vs->isearch_len).caret
                          : found;
}

static wp_emacs_action_kind feed_isearch(wp_emacs_state *vs, wp_key_event key)
{
    if (key.special == (u16)WP_KEY_ESCAPE || key.special == (u16)WP_KEY_ENTER) {
        vs->isearch_mode = 0; /* accept the current position */
        return WP_EMACSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (vs->isearch_len > 0) {
            vs->isearch_len--;
            vs->isearch_have_match = WP_FALSE;
            vs->cursor = vs->isearch_anchor;
            isearch_run(vs, vs->isearch_anchor);
        } else {
            vs->isearch_mode = 0;
            vs->cursor = vs->isearch_anchor;
        }
        return WP_EMACSACTION_NONE;
    }
    if (key.special != (u16)WP_KEY_NONE) return WP_EMACSACTION_NONE; /* stay active, ignore other specials */

    if (key.mods & (u8)WP_MOD_CTRL) {
        if (key.codepoint == 'g') { /* C-g: real Emacs abort -- restores the original position */
            vs->isearch_mode = 0;
            vs->cursor = vs->isearch_anchor;
            return WP_EMACSACTION_NONE;
        }
        if (key.codepoint == 's' || key.codepoint == 'r') {
            wp_pos from_pos;
            vs->isearch_mode = (key.codepoint == 's') ? (u8)'f' : (u8)'r';
            from_pos = vs->isearch_have_match ? vs->isearch_last_match : vs->isearch_anchor;
            isearch_run(vs, from_pos);
            return WP_EMACSACTION_NONE;
        }
        return WP_EMACSACTION_NONE; /* other Ctrl-chords: ignored, stay active */
    }

    if (key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (vs->isearch_len < WP_EMACS_ISEARCH_MAX - 1) {
            vs->isearch_buf[vs->isearch_len++] = (u8)key.codepoint;
        }
        isearch_run(vs, vs->isearch_anchor);
    }
    return WP_EMACSACTION_NONE;
}

/* ------------------------------------------------------------------ */
/* Query-replace (M-%) -- M11                                          */
/* ------------------------------------------------------------------ */

/* Forward-only, point to end of document -- real stock M-% has no
   reverse variant and no G-equivalent restart-from-top (see emacs.h's
   own file comment); never wraps back past scope_start (the position
   M-% was invoked from), same "never re-cover ground already passed"
   reasoning wordstar.c's own ws_find_next_in_scope documents. */
static wp_bool qr_find_next_in_scope(wp_emacs_state *vs, wp_pos from_pos, wp_pos *out)
{
    wp_pos found;

    if (wp_document_find_text(vs->doc, from_pos, vs->qr_find, vs->qr_find_len,
                               WP_TRUE, WP_FALSE, WP_FALSE, &found) != WP_OK) {
        return WP_FALSE;
    }
    if (wp_pos_compare(found, vs->scope_start) < 0) return WP_FALSE;
    *out = found;
    return WP_TRUE;
}

/* '!' -- replaces first_match and every remaining match in scope
   silently, as ONE undo group. Continuation is always "after the
   inserted text" (forward-only, unlike WordStar/WordPerfect's own
   bidirectional version of this helper) -- still guards against an
   infinite loop when the replacement text itself contains the search
   pattern. */
static void qr_replace_all_remaining(wp_emacs_state *vs, wp_pos first_match)
{
    wp_pos match = first_match;

    wp_undo_group_begin(vs->journal);
    for (;;) {
        wp_selection sel = wp_document_match_selection(vs->doc, match, vs->qr_find_len);
        wp_pos before_insert, out, next;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->qr_replace,
                            vs->qr_replace_len, &out);
        vs->cursor = out;

        if (!qr_find_next_in_scope(vs, out, &next)) break;
        match = next;
    }
    wp_undo_group_end(vs->journal);
}

/* M-% always pauses at the first match (or BEEPs if none) -- unlike
   WordStar/WordPerfect's own upfront "confirm every match, or none at
   all" choice, real query-replace always starts interactive; replacing
   the rest silently is reached mid-loop via '!' instead (see
   feed_qr_confirm). */
static wp_emacs_action_kind qr_begin(wp_emacs_state *vs)
{
    wp_pos found;

    vs->scope_start = vs->cursor;
    if (!qr_find_next_in_scope(vs, vs->scope_start, &found)) {
        vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
        return WP_EMACSACTION_BEEP;
    }

    vs->cursor = found;
    vs->pending_match = found;
    vs->prompt_stage = (u8)WP_EMACS_PROMPT_CONFIRM_MATCH;
    return WP_EMACSACTION_NONE;
}

/* FIND_TEXT and REPLACE_TEXT fields -- Enter-terminated (real Emacs's
   own minibuffer convention, unlike WordPerfect's F2-terminated
   fields), Backspace/Escape handled the same way every other scheme's
   own text-collection prompt does. */
static wp_emacs_action_kind feed_qr_text(wp_emacs_state *vs, wp_key_event key,
                                          u8 *buf, u32 *len, wp_bool is_find_stage)
{
    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
        return WP_EMACSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (*len > 0) (*len)--;
        else vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
        return WP_EMACSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        if (is_find_stage) {
            if (*len == 0) {
                vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
                return WP_EMACSACTION_BEEP;
            }
            vs->prompt_stage = (u8)WP_EMACS_PROMPT_REPLACE_TEXT;
            return WP_EMACSACTION_NONE;
        }
        vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
        return qr_begin(vs);
    }
    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (*len < WP_EMACS_QR_MAX - 1) buf[(*len)++] = (u8)key.codepoint;
    }
    return WP_EMACSACTION_NONE;
}

/* Real query-replace vocabulary, scoped subset (see emacs.h's own file
   comment for what's deliberately left out): y/SPC replace+advance,
   n/DEL skip+advance, '!' replace all remaining silently, '.'
   replace+stop, q/RET/Escape stop without replacing this one. */
static wp_emacs_action_kind feed_qr_confirm(wp_emacs_state *vs, wp_key_event key)
{
    wp_pos continue_from, next;
    wp_bool stop_after = WP_FALSE;

    if (key.special == (u16)WP_KEY_ESCAPE || key.special == (u16)WP_KEY_ENTER) {
        vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
        return WP_EMACSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) { /* DEL: skip, a real query-replace binding */
        continue_from = vs->pending_match;
    } else if (key.special == (u16)WP_KEY_NONE) {
        if (key.codepoint == ' ' || key.codepoint == 'y' || key.codepoint == 'Y' ||
            key.codepoint == '.') {
            wp_selection sel = wp_document_match_selection(vs->doc, vs->pending_match, vs->qr_find_len);
            wp_pos before_insert, out;

            wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
            wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->qr_replace,
                                vs->qr_replace_len, &out);
            vs->cursor = out;
            continue_from = out;
            if (key.codepoint == '.') stop_after = WP_TRUE;
        } else if (key.codepoint == 'n' || key.codepoint == 'N') {
            continue_from = vs->pending_match;
        } else if (key.codepoint == '!') {
            vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
            qr_replace_all_remaining(vs, vs->pending_match);
            return WP_EMACSACTION_NONE;
        } else if (key.codepoint == 'q') {
            vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
            return WP_EMACSACTION_NONE;
        } else {
            return WP_EMACSACTION_NONE; /* unrecognized: stay paused */
        }
    } else {
        return WP_EMACSACTION_NONE;
    }

    vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
    if (stop_after) return WP_EMACSACTION_NONE;

    if (!qr_find_next_in_scope(vs, continue_from, &next)) return WP_EMACSACTION_NONE;
    vs->cursor = next;
    vs->pending_match = next;
    vs->prompt_stage = (u8)WP_EMACS_PROMPT_CONFIRM_MATCH;
    return WP_EMACSACTION_NONE;
}

static wp_emacs_action_kind feed_qr_prompt(wp_emacs_state *vs, wp_key_event key)
{
    switch ((wp_emacs_prompt_stage)vs->prompt_stage) {
    case WP_EMACS_PROMPT_FIND_TEXT:
        return feed_qr_text(vs, key, vs->qr_find, &vs->qr_find_len, WP_TRUE);
    case WP_EMACS_PROMPT_REPLACE_TEXT:
        return feed_qr_text(vs, key, vs->qr_replace, &vs->qr_replace_len, WP_FALSE);
    case WP_EMACS_PROMPT_CONFIRM_MATCH:
        return feed_qr_confirm(vs, key);
    default:
        vs->prompt_stage = (u8)WP_EMACS_PROMPT_NONE;
        return WP_EMACSACTION_NONE;
    }
}

wp_emacs_action_kind wp_emacs_feed_key(wp_emacs_state *vs, wp_key_event key)
{
    if (vs->isearch_mode != 0) return feed_isearch(vs, key);
    if (vs->prompt_stage != (u8)WP_EMACS_PROMPT_NONE) return feed_qr_prompt(vs, key);
    if (vs->pending_prefix != 0) return handle_prefix_key(vs, key);
    if (vs->pending_meta) {
        vs->pending_meta = WP_FALSE;
        return handle_meta_key(vs, key);
    }

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->pending_meta = WP_TRUE;
        return WP_EMACSACTION_NONE;
    }

    switch (key.special) {
    case WP_KEY_ENTER: {
        wp_pos out;
        wp_edit_split_paragraph(vs->doc, vs->journal, vs->cursor, &out);
        vs->cursor = out;
        return WP_EMACSACTION_NONE;
    }
    case WP_KEY_BACKSPACE:
        if (key.mods & (u8)WP_MOD_ALT) { kill_word_backward(vs); return WP_EMACSACTION_NONE; }
        delete_char_backward(vs);
        return WP_EMACSACTION_NONE;
    case WP_KEY_DELETE:
        delete_chars_forward(vs, 1);
        return WP_EMACSACTION_NONE;
    case WP_KEY_LEFT:
        vs->cursor = wp_pos_prev(vs->doc, vs->cursor);
        return WP_EMACSACTION_NONE;
    case WP_KEY_RIGHT:
        vs->cursor = wp_pos_next(vs->doc, vs->cursor);
        return WP_EMACSACTION_NONE;
    case WP_KEY_UP:
        vs->cursor = wp_pos_paragraph_up(vs->doc, vs->cursor);
        return WP_EMACSACTION_NONE;
    case WP_KEY_DOWN:
        vs->cursor = wp_pos_paragraph_down(vs->doc, vs->cursor);
        return WP_EMACSACTION_NONE;
    case WP_KEY_HOME:
        vs->cursor = wp_pos_line_start(vs->doc, vs->cursor);
        return WP_EMACSACTION_NONE;
    case WP_KEY_END:
        vs->cursor = wp_pos_line_end(vs->doc, vs->cursor);
        return WP_EMACSACTION_NONE;
    default:
        break;
    }

    if (key.special != (u16)WP_KEY_NONE) return WP_EMACSACTION_NONE; /* an unhandled special key: harmless no-op */

    if (key.mods & (u8)WP_MOD_ALT) return handle_meta_key(vs, key);
    if (key.mods & (u8)WP_MOD_CTRL) return handle_ctrl_letter(vs, key);

    if (key.mods == 0 && key.codepoint >= 0x20) {
        insert_char(vs, key.codepoint);
        return WP_EMACSACTION_NONE;
    }

    return WP_EMACSACTION_NONE;
}

void wp_emacs_get_status_text(const wp_emacs_state *vs, char *out, u32 out_cap)
{
    const char *label;
    const u8 *buf = NULL;
    u32 buf_len = 0;
    u32 len = 0;

    if (out_cap == 0) return;

    if (vs->isearch_mode != 0) {
        label = (vs->isearch_mode == (u8)'f') ? "I-search:" : "I-search backward:";
        buf = vs->isearch_buf; buf_len = vs->isearch_len;
    } else {
        switch ((wp_emacs_prompt_stage)vs->prompt_stage) {
        case WP_EMACS_PROMPT_FIND_TEXT:
            label = "Query replace:"; buf = vs->qr_find; buf_len = vs->qr_find_len; break;
        case WP_EMACS_PROMPT_REPLACE_TEXT:
            label = "Query replace with:"; buf = vs->qr_replace; buf_len = vs->qr_replace_len; break;
        case WP_EMACS_PROMPT_CONFIRM_MATCH:
            label = "Query replacing (y/n/!/.)?"; break;
        case WP_EMACS_PROMPT_NONE:
        default:
            label = "EMACS"; break;
        }
    }

    while (label[len] != '\0' && len < out_cap - 1) { out[len] = label[len]; len++; }
    if (buf != NULL) {
        u32 i;
        if (len < out_cap - 1) out[len++] = ' ';
        for (i = 0; i < buf_len && len < out_cap - 1; i++) out[len++] = (char)buf[i];
    }
    out[len] = '\0';
}
