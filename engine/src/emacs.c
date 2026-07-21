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
    default:
        return WP_EMACSACTION_BEEP;
    }
}

wp_emacs_action_kind wp_emacs_feed_key(wp_emacs_state *vs, wp_key_event key)
{
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
