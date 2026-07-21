#include <stddef.h>
#include "wordstar.h"
#include "edit_internal.h"
#include "platform.h"
#include "utf8.h"

/* See wordstar.h for the full design account. */

void wp_wordstar_init(wp_wordstar_state *vs, wp_document *doc, wp_undo_journal *journal)
{
    vs->doc = doc;
    vs->journal = journal;
    vs->cursor = wp_pos_doc_start(doc);
    vs->pending_prefix = 0;
    vs->block_begin_set = WP_FALSE;
    vs->block_end_set = WP_FALSE;

    vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
    vs->prompt_is_replace = WP_FALSE;
    vs->prompt_find_len = 0;
    vs->prompt_replace_len = 0;
    vs->opt_global = WP_FALSE;
    vs->opt_no_confirm = WP_FALSE;
    vs->opt_backward = WP_FALSE;
    vs->opt_ignore_case = WP_FALSE;
    vs->opt_whole_word = WP_FALSE;
    vs->have_last = WP_FALSE;
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

static void delete_chars_forward(wp_wordstar_state *vs, u32 n)
{
    wp_pos target = vs->cursor;
    wp_pos out;
    u32 i;

    for (i = 0; i < n; i++) target = wp_pos_next(vs->doc, target);
    if (wp_pos_compare(target, vs->cursor) == 0) return;

    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

/* Mirrors vimode.c's own insert_backspace exactly (same special-cased
   "cursor at paragraph start" merge, rather than a single generic
   delete-selection call) -- deliberately reproduced, not re-derived,
   since that function's own shape is the proven-correct pattern for
   this exact edge case. */
static void delete_char_backward(wp_wordstar_state *vs)
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

static void delete_word_forward(wp_wordstar_state *vs)
{
    wp_pos target = wp_pos_word_forward(vs->doc, vs->cursor, WP_FALSE);
    wp_pos out;

    if (wp_pos_compare(target, vs->cursor) == 0) return;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, target), &out);
    vs->cursor = out;
}

static void delete_to_line_end(wp_wordstar_state *vs)
{
    wp_pos end = wp_pos_line_end(vs->doc, vs->cursor);
    wp_pos out;

    if (wp_pos_compare(end, vs->cursor) == 0) return;
    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(vs->cursor, end), &out);
    vs->cursor = out;
}

/* ^Y "delete line": mirrors vimode.c's own normalize_linewise_selection
   applied to a zero-width (cursor, cursor) range -- deletes the whole
   current paragraph, extending into the paragraph after (or, on the
   section's last paragraph, merging upward into the one before) rather
   than just emptying it and leaving an orphaned blank paragraph behind.
   A self-contained copy, not a shared engine promotion (see wordstar.c's
   own file comment on when promotion is/isn't warranted) -- unlike
   wp_edit_paste_text, no second concrete consumer exists yet. */
static void delete_current_paragraph(wp_wordstar_state *vs)
{
    const wp_section *sec = wp_document_section_get(vs->doc, vs->cursor.section);
    wp_pos start, end, out;

    if (sec == NULL) return;

    if (vs->cursor.paragraph + 1 < sec->para_count) {
        start.section = vs->cursor.section; start.paragraph = vs->cursor.paragraph; start.run = 0; start.offset = 0;
        end.section = vs->cursor.section; end.paragraph = vs->cursor.paragraph + 1; end.run = 0; end.offset = 0;
    } else if (vs->cursor.paragraph > 0) {
        start.section = vs->cursor.section;
        start.paragraph = vs->cursor.paragraph - 1;
        paragraph_end_run_offset(&sec->paras[vs->cursor.paragraph - 1], &start.run, &start.offset);
        end = wp_pos_doc_end(vs->doc);
    } else {
        start.section = vs->cursor.section; start.paragraph = vs->cursor.paragraph; start.run = 0; start.offset = 0;
        end = wp_pos_doc_end(vs->doc);
    }

    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(start, end), &out);
    vs->cursor = out;
}

/* ^KC: copies the marked block to the cursor, then re-marks the block
   at the NEW copy's location (real WordStar behavior -- lets a
   follow-up ^KC copy the copy, without needing to re-mark by hand).
   Correct regardless of the cursor's position relative to the block:
   the block's own OLD markers are only ever read once, before anything
   changes, so nothing about them needs to stay valid afterward -- see
   wordstar.c's own file comment on move_block for why THAT operation
   is harder (it also has to delete the original, whose position DOES
   need to stay valid across the insert). */
static void copy_block(wp_wordstar_state *vs)
{
    wp_pos begin, end, insert_start, out;
    wp_selection sel;
    u32 needed;
    u8 *buf;

    if (!vs->block_begin_set || !vs->block_end_set) return;
    if (wp_pos_compare(vs->block_begin, vs->block_end) <= 0) { begin = vs->block_begin; end = vs->block_end; }
    else { begin = vs->block_end; end = vs->block_begin; }
    if (wp_pos_compare(begin, end) == 0) return;

    sel = sel_of(begin, end);
    wp_document_extract_text(vs->doc, sel, NULL, 0, &needed);
    if (needed == 0) return;
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;
    wp_document_extract_text(vs->doc, sel, buf, needed, &needed);

    insert_start = vs->cursor;
    wp_edit_paste_text(vs->doc, vs->journal, insert_start, buf, needed, &out);
    mem_free(buf);

    vs->cursor = out;
    vs->block_begin = insert_start;
    vs->block_end = out;
}

/* ^KY: deletes the marked block outright (no copy elsewhere). A single
   structural change -- unlike move_block, nothing else needs rebasing
   afterward. */
static void delete_block(wp_wordstar_state *vs)
{
    wp_pos begin, end, out;

    if (!vs->block_begin_set || !vs->block_end_set) return;
    if (wp_pos_compare(vs->block_begin, vs->block_end) <= 0) { begin = vs->block_begin; end = vs->block_end; }
    else { begin = vs->block_end; end = vs->block_begin; }
    if (wp_pos_compare(begin, end) == 0) return;

    wp_edit_delete_selection(vs->doc, vs->journal, sel_of(begin, end), &out);
    vs->cursor = out;
    vs->block_begin_set = WP_FALSE;
    vs->block_end_set = WP_FALSE;
}

/* ^KV: moves the marked block to the cursor. Refuses (silent no-op --
   the caller BEEPs) when the cursor lands inside the block, or in the
   exact same paragraph the block's own end boundary is in -- the one
   case this MVP's simple whole-paragraph-index rebasing can't handle
   correctly (recomputing a run/offset inside a paragraph that itself
   just got merged away needs the same byte-offset machinery
   wp_edit_delete_selection already solves internally for ITS OWN
   out_pos, not exposed for a third, unrelated position -- a real,
   deliberately scoped MVP limit, not a silent wrong answer: see
   docs/keymaps.md).

   For every other relative position, exactly one of {insert, delete}
   can safely run first without invalidating what the other one reads:
   - cursor strictly before the block: deleting the block doesn't touch
     anything before it, so cursor stays valid -- delete first, then
     insert at the (still-valid) cursor.
   - cursor strictly after the block (and outside its end paragraph):
     inserting at the cursor doesn't touch anything before it, so the
     block's own begin/end stay valid -- insert first, then delete the
     original using those still-valid positions. The insert's own
     result position, and the cursor itself, both then need their
     paragraph index shifted back by however many paragraphs the
     now-deleted block spanned (their run/offset are untouched -- a
     paragraph strictly after a same-section deletion only has its
     INDEX shift, never its own internal run/offset structure). */
static void move_block(wp_wordstar_state *vs)
{
    wp_pos begin, end, out;
    wp_selection sel;
    u32 needed;
    u8 *buf;
    wp_pos cursor = vs->cursor;

    if (!vs->block_begin_set || !vs->block_end_set) return;
    if (wp_pos_compare(vs->block_begin, vs->block_end) <= 0) { begin = vs->block_begin; end = vs->block_end; }
    else { begin = vs->block_end; end = vs->block_begin; }
    if (wp_pos_compare(begin, end) == 0) return;

    if (cursor.section != begin.section) return;
    if (wp_pos_compare(cursor, begin) >= 0 && wp_pos_compare(cursor, end) < 0) return; /* inside the block */
    /* The one deferred edge case (see above): cursor lands in the exact
       paragraph a MULTI-paragraph block's own end boundary sits in --
       only a real hazard when the block spans more than one paragraph
       (begin.paragraph != end.paragraph), since only then does deleting
       it actually MERGE that paragraph's content elsewhere, invalidating
       any run/offset within it. A single-paragraph block (by far the
       common case -- rearranging words within one line) never merges
       anything, so cursor sharing that same paragraph is completely
       normal and must NOT be refused here. */
    if (begin.paragraph != end.paragraph && cursor.paragraph == end.paragraph &&
        wp_pos_compare(cursor, begin) >= 0) {
        return;
    }

    sel = sel_of(begin, end);
    wp_document_extract_text(vs->doc, sel, NULL, 0, &needed);
    if (needed == 0) return;
    buf = (u8 *)mem_alloc(needed);
    if (buf == NULL) return;
    wp_document_extract_text(vs->doc, sel, buf, needed, &needed);

    if (wp_pos_compare(cursor, begin) < 0) {
        wp_edit_delete_selection(vs->doc, vs->journal, sel, NULL);
        wp_edit_paste_text(vs->doc, vs->journal, cursor, buf, needed, &out);
        vs->cursor = out;
        vs->block_begin = cursor;
        vs->block_end = out;
    } else {
        u32 removed_paragraphs = end.paragraph - begin.paragraph;
        wp_pos new_begin = cursor;
        wp_pos new_end;

        wp_edit_paste_text(vs->doc, vs->journal, cursor, buf, needed, &out);
        wp_edit_delete_selection(vs->doc, vs->journal, sel, NULL);

        new_begin.paragraph -= removed_paragraphs;
        new_end = out;
        new_end.paragraph -= removed_paragraphs;

        vs->cursor = new_end;
        vs->block_begin = new_begin;
        vs->block_end = new_end;
    }

    mem_free(buf);
}

/* ------------------------------------------------------------------ */
/* Find / Replace (^QF, ^QA, ^L) -- M11                                 */
/* ------------------------------------------------------------------ */

/* Finds the next match scanning from `from_pos`, honoring opt_backward
   and the current opt_ignore_case/opt_whole_word flags, without ever
   crossing back past vs->scope_start -- WordStar's own G option means
   "restart from the top of the document," not "wrap around forever";
   without G, a Replace only ever covers cursor-to-end (or, backward,
   cursor-to-start), never doubling back over ground already covered.
   wp_document_find_text's own single-wrap-then-stop contract is reused
   for the underlying scan; this just rejects a match that lands on the
   wrapped-around side of scope_start. */
static wp_bool ws_find_next_in_scope(wp_wordstar_state *vs, wp_pos from_pos, wp_pos *out)
{
    wp_pos found;
    wp_bool forward = (wp_bool)!vs->opt_backward;

    if (wp_document_find_text(vs->doc, from_pos, vs->prompt_find, vs->prompt_find_len,
                               forward, vs->opt_ignore_case, vs->opt_whole_word, &found) != WP_OK) {
        return WP_FALSE;
    }
    if (forward && wp_pos_compare(found, vs->scope_start) < 0) return WP_FALSE;
    if (!forward && wp_pos_compare(found, vs->scope_start) > 0) return WP_FALSE;
    *out = found;
    return WP_TRUE;
}

/* ^QA without the N option: replaces first_match and every remaining
   match in scope silently, as ONE undo group. Continuation position
   differs by direction -- see the comment above move_block for the same
   "which side stays valid" reasoning applied here: forward continues
   from AFTER the freshly-inserted replacement (out), so it can never
   re-match inside/adjacent to what was just written; backward continues
   from BEFORE it (the delete's own returned collapse position, valid by
   wp_edit_delete_selection's own contract), so it never re-enters the
   region it just touched either. Both guard against an infinite loop
   when the replacement text itself contains the search pattern (e.g.
   replacing "a" with "aa"). */
static void ws_replace_all_remaining(wp_wordstar_state *vs, wp_pos first_match)
{
    wp_pos match = first_match;
    wp_bool forward = (wp_bool)!vs->opt_backward;

    wp_undo_group_begin(vs->journal);
    for (;;) {
        wp_selection sel = wp_document_match_selection(vs->doc, match, vs->prompt_find_len);
        wp_pos before_insert, out, next, continue_from;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->prompt_replace,
                            vs->prompt_replace_len, &out);
        vs->cursor = out;

        continue_from = forward ? out : before_insert;
        if (!ws_find_next_in_scope(vs, continue_from, &next)) break;
        match = next;
    }
    wp_undo_group_end(vs->journal);
}

/* Runs the search/replace this scheme's Find/Replace prompt (or ^L) just
   collected: computes scope_start from opt_global/opt_backward, finds
   the first match, and either jumps there (plain Find), silently
   batch-replaces the rest of scope (Replace with N given), or pauses in
   WP_WS_PROMPT_CONFIRM awaiting a per-match Y/N (Replace without N). */
static wp_wordstar_action_kind ws_begin_search(wp_wordstar_state *vs)
{
    wp_pos found;

    vs->scope_start = vs->opt_global
        ? (vs->opt_backward ? wp_pos_doc_end(vs->doc) : wp_pos_doc_start(vs->doc))
        : vs->cursor;

    if (!ws_find_next_in_scope(vs, vs->scope_start, &found)) return WP_WSACTION_BEEP;

    vs->cursor = found;

    if (!vs->prompt_is_replace) return WP_WSACTION_NONE; /* ^QF: one jump, done */

    if (vs->opt_no_confirm) {
        ws_replace_all_remaining(vs, found);
        return WP_WSACTION_NONE;
    }

    vs->pending_match = found;
    vs->prompt_stage = (u8)WP_WS_PROMPT_CONFIRM;
    return WP_WSACTION_NONE;
}

/* FIND_TEXT and REPLACE_TEXT stages share this shape -- collect
   printable characters into the caller's buf/len pair, Backspace/
   Escape/Enter handled uniformly, mirroring vi-mode's own cmdline_buf
   Command-line mode. */
static wp_wordstar_action_kind feed_prompt_text(wp_wordstar_state *vs, wp_key_event key,
                                                 u8 *buf, u32 *len, wp_bool is_find_stage)
{
    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
        return WP_WSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (*len > 0) (*len)--;
        else vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
        return WP_WSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        if (is_find_stage && *len == 0) {
            vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
            return WP_WSACTION_BEEP;
        }
        if (is_find_stage && vs->prompt_is_replace) {
            vs->prompt_stage = (u8)WP_WS_PROMPT_REPLACE_TEXT;
        } else {
            vs->opt_global = WP_FALSE;
            vs->opt_no_confirm = WP_FALSE;
            vs->opt_backward = WP_FALSE;
            vs->opt_ignore_case = WP_FALSE;
            vs->opt_whole_word = WP_FALSE;
            vs->prompt_stage = (u8)WP_WS_PROMPT_OPTIONS;
        }
        return WP_WSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (*len < WP_WORDSTAR_PROMPT_MAX - 1) buf[(*len)++] = (u8)key.codepoint;
    }
    return WP_WSACTION_NONE;
}

static wp_wordstar_action_kind feed_prompt_options(wp_wordstar_state *vs, wp_key_event key)
{
    if (key.special == (u16)WP_KEY_ESCAPE || key.special == (u16)WP_KEY_BACKSPACE) {
        vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
        return WP_WSACTION_NONE;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
        vs->have_last = WP_TRUE;
        return ws_begin_search(vs);
    }
    if (key.special == (u16)WP_KEY_NONE) {
        u32 c = key.codepoint;
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        switch (c) {
        case 'G': vs->opt_global = WP_TRUE; break;
        case 'N': vs->opt_no_confirm = WP_TRUE; break;
        case 'B': vs->opt_backward = WP_TRUE; break;
        case 'U': vs->opt_ignore_case = WP_TRUE; break;
        case 'W': vs->opt_whole_word = WP_TRUE; break;
        default: break; /* silently ignored -- real WordStar's own OPTIONS?
                            prompt has a numeric repeat-count and a '?' help
                            request this scheme doesn't implement (see
                            wordstar.h); anything else typed here is just
                            not a recognized option letter. */
        }
    }
    return WP_WSACTION_NONE;
}

static wp_wordstar_action_kind feed_prompt_confirm(wp_wordstar_state *vs, wp_key_event key)
{
    wp_bool forward;
    wp_pos continue_from, next;
    u32 c;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
        return WP_WSACTION_NONE;
    }
    if (key.special != (u16)WP_KEY_NONE) return WP_WSACTION_NONE;

    c = key.codepoint;
    forward = (wp_bool)!vs->opt_backward;

    if (c == 'y' || c == 'Y') {
        wp_selection sel = wp_document_match_selection(vs->doc, vs->pending_match, vs->prompt_find_len);
        wp_pos before_insert, out;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &before_insert);
        wp_edit_paste_text(vs->doc, vs->journal, before_insert, vs->prompt_replace,
                            vs->prompt_replace_len, &out);
        vs->cursor = out;
        continue_from = forward ? out : before_insert;
    } else if (c == 'n' || c == 'N') {
        continue_from = vs->pending_match; /* skip: leave it untouched, just move past it */
    } else {
        return WP_WSACTION_NONE; /* not y/n -- stay paused, matching real WordStar's own prompt */
    }

    vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
    if (!ws_find_next_in_scope(vs, continue_from, &next)) return WP_WSACTION_NONE;

    vs->cursor = next;
    vs->pending_match = next;
    vs->prompt_stage = (u8)WP_WS_PROMPT_CONFIRM;
    return WP_WSACTION_NONE;
}

static wp_wordstar_action_kind feed_prompt(wp_wordstar_state *vs, wp_key_event key)
{
    switch ((wp_wordstar_prompt_stage)vs->prompt_stage) {
    case WP_WS_PROMPT_FIND_TEXT:
        return feed_prompt_text(vs, key, vs->prompt_find, &vs->prompt_find_len, WP_TRUE);
    case WP_WS_PROMPT_REPLACE_TEXT:
        return feed_prompt_text(vs, key, vs->prompt_replace, &vs->prompt_replace_len, WP_FALSE);
    case WP_WS_PROMPT_OPTIONS:
        return feed_prompt_options(vs, key);
    case WP_WS_PROMPT_CONFIRM:
        return feed_prompt_confirm(vs, key);
    default:
        vs->prompt_stage = (u8)WP_WS_PROMPT_NONE;
        return WP_WSACTION_NONE;
    }
}

static wp_wordstar_action_kind handle_prefix_key(wp_wordstar_state *vs, wp_key_event key)
{
    u8 prefix = vs->pending_prefix;
    u32 c = key.codepoint;

    vs->pending_prefix = 0; /* single-level: always consumed by this one follow-up key */

    /* Works with or without Ctrl held on the follow-up key -- ^KB and
       ^K then plain B are equivalent, WordStar's own real convention
       (see wordstar.h's own file comment). Backends already lowercase
       a Ctrl-chord's codepoint; a plain Shift+letter still arrives
       uppercase, so fold it here too. */
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';

    switch (prefix) {
    case 'k':
        switch (c) {
        case 'b': vs->block_begin = vs->cursor; vs->block_begin_set = WP_TRUE; return WP_WSACTION_NONE;
        case 'k': vs->block_end = vs->cursor; vs->block_end_set = WP_TRUE; return WP_WSACTION_NONE;
        case 'c': copy_block(vs); return WP_WSACTION_NONE;
        case 'v': move_block(vs); return WP_WSACTION_NONE;
        case 'y': delete_block(vs); return WP_WSACTION_NONE;
        case 's': case 'd': return WP_WSACTION_SAVE;
        case 'x': return WP_WSACTION_SAVE_QUIT;
        case 'q': return WP_WSACTION_QUIT;
        default: return WP_WSACTION_BEEP;
        }
    case 'q':
        switch (c) {
        case 's': vs->cursor = wp_pos_line_start(vs->doc, vs->cursor); return WP_WSACTION_NONE;
        case 'd': vs->cursor = wp_pos_line_end(vs->doc, vs->cursor); return WP_WSACTION_NONE;
        case 'r': vs->cursor = wp_pos_doc_start(vs->doc); return WP_WSACTION_NONE;
        case 'c': vs->cursor = wp_pos_doc_end(vs->doc); return WP_WSACTION_NONE;
        case 'y': delete_to_line_end(vs); return WP_WSACTION_NONE;
        case 'f':
            vs->prompt_is_replace = WP_FALSE;
            vs->prompt_find_len = 0;
            vs->prompt_stage = (u8)WP_WS_PROMPT_FIND_TEXT;
            return WP_WSACTION_NONE;
        case 'a':
            vs->prompt_is_replace = WP_TRUE;
            vs->prompt_find_len = 0;
            vs->prompt_replace_len = 0;
            vs->prompt_stage = (u8)WP_WS_PROMPT_FIND_TEXT;
            return WP_WSACTION_NONE;
        default: return WP_WSACTION_BEEP;
        }
    case 'p':
        switch (c) {
        case 'b': return WP_WSACTION_FORMAT_BOLD;
        case 'y': return WP_WSACTION_FORMAT_ITALIC;
        case 's': return WP_WSACTION_FORMAT_UNDERLINE;
        default: return WP_WSACTION_BEEP;
        }
    default:
        /* 'o'/'j' (Onscreen/Help): real prefix letters, registered so
           pressing them doesn't fall through as a bare insert, but no
           subcommands are implemented this pass -- see wordstar.h's
           own file comment. */
        return WP_WSACTION_BEEP;
    }
}

static wp_wordstar_action_kind handle_ctrl_letter(wp_wordstar_state *vs, wp_key_event key)
{
    switch (key.codepoint) {
    case 's': vs->cursor = wp_pos_prev(vs->doc, vs->cursor); return WP_WSACTION_NONE;
    case 'd': vs->cursor = wp_pos_next(vs->doc, vs->cursor); return WP_WSACTION_NONE;
    case 'e': vs->cursor = wp_pos_paragraph_up(vs->doc, vs->cursor); return WP_WSACTION_NONE;
    case 'x': vs->cursor = wp_pos_paragraph_down(vs->doc, vs->cursor); return WP_WSACTION_NONE;
    case 'a': vs->cursor = wp_pos_word_backward(vs->doc, vs->cursor, WP_FALSE); return WP_WSACTION_NONE;
    case 'f': vs->cursor = wp_pos_word_forward(vs->doc, vs->cursor, WP_FALSE); return WP_WSACTION_NONE;
    case 'g': delete_chars_forward(vs, 1); return WP_WSACTION_NONE;
    case 't': delete_word_forward(vs); return WP_WSACTION_NONE;
    case 'y': delete_current_paragraph(vs); return WP_WSACTION_NONE;
    case 'l': /* find/replace again -- see wordstar.h's own file comment */
        if (!vs->have_last) return WP_WSACTION_BEEP;
        return ws_begin_search(vs);
    case 'k': case 'q': case 'p': case 'o': case 'j':
        vs->pending_prefix = (u8)key.codepoint;
        return WP_WSACTION_NONE;
    default:
        return WP_WSACTION_BEEP;
    }
}

wp_wordstar_action_kind wp_wordstar_feed_key(wp_wordstar_state *vs, wp_key_event key)
{
    if (vs->prompt_stage != (u8)WP_WS_PROMPT_NONE) return feed_prompt(vs, key);
    if (vs->pending_prefix != 0) return handle_prefix_key(vs, key);

    switch (key.special) {
    case WP_KEY_ENTER: {
        wp_pos out;
        wp_edit_split_paragraph(vs->doc, vs->journal, vs->cursor, &out);
        vs->cursor = out;
        return WP_WSACTION_NONE;
    }
    case WP_KEY_BACKSPACE:
        delete_char_backward(vs);
        return WP_WSACTION_NONE;
    case WP_KEY_DELETE:
        delete_chars_forward(vs, 1);
        return WP_WSACTION_NONE;
    case WP_KEY_LEFT:
        vs->cursor = wp_pos_prev(vs->doc, vs->cursor);
        return WP_WSACTION_NONE;
    case WP_KEY_RIGHT:
        vs->cursor = wp_pos_next(vs->doc, vs->cursor);
        return WP_WSACTION_NONE;
    case WP_KEY_UP:
        vs->cursor = wp_pos_paragraph_up(vs->doc, vs->cursor);
        return WP_WSACTION_NONE;
    case WP_KEY_DOWN:
        vs->cursor = wp_pos_paragraph_down(vs->doc, vs->cursor);
        return WP_WSACTION_NONE;
    case WP_KEY_HOME:
        vs->cursor = wp_pos_line_start(vs->doc, vs->cursor);
        return WP_WSACTION_NONE;
    case WP_KEY_END:
        vs->cursor = wp_pos_line_end(vs->doc, vs->cursor);
        return WP_WSACTION_NONE;
    default:
        break;
    }

    if (key.special != (u16)WP_KEY_NONE) return WP_WSACTION_NONE; /* an unhandled special key: harmless no-op */

    if (key.mods & (u8)WP_MOD_CTRL) return handle_ctrl_letter(vs, key);

    if (key.mods == 0 && key.codepoint >= 0x20) {
        u8 utf8_buf[4];
        u32 n = utf8_encode(key.codepoint, utf8_buf);
        if (n > 0) {
            wp_pos out;
            wp_edit_insert_text(vs->doc, vs->journal, vs->cursor, utf8_buf, n, &out);
            vs->cursor = out;
        }
    }

    return WP_WSACTION_NONE;
}

void wp_wordstar_get_status_text(const wp_wordstar_state *vs, char *out, u32 out_cap)
{
    const char *label;
    const u8 *buf = NULL;
    u32 buf_len = 0;
    u32 len = 0;

    if (out_cap == 0) return;

    switch ((wp_wordstar_prompt_stage)vs->prompt_stage) {
    case WP_WS_PROMPT_FIND_TEXT:
        label = "FIND?"; buf = vs->prompt_find; buf_len = vs->prompt_find_len; break;
    case WP_WS_PROMPT_REPLACE_TEXT:
        label = "REPLACE WITH?"; buf = vs->prompt_replace; buf_len = vs->prompt_replace_len; break;
    case WP_WS_PROMPT_OPTIONS:
        label = "OPTIONS?"; break;
    case WP_WS_PROMPT_CONFIRM:
        label = "REPLACE (Y/N)?"; break;
    case WP_WS_PROMPT_NONE:
    default:
        label = "WORDSTAR"; break;
    }

    while (label[len] != '\0' && len < out_cap - 1) { out[len] = label[len]; len++; }
    if (buf != NULL) {
        u32 i;
        if (len < out_cap - 1) out[len++] = ' ';
        for (i = 0; i < buf_len && len < out_cap - 1; i++) out[len++] = (char)buf[i];
    }
    out[len] = '\0';
}
