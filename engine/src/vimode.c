#include <string.h>
#include "vimode.h"
#include "edit_internal.h"
#include "platform.h"
#include "utf8.h"

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static u32 effective_count(const wp_vimode_state *vs)
{
    return (vs->count_pending == 0) ? 1 : vs->count_pending;
}

static u32 effective_op_count(const wp_vimode_state *vs)
{
    return (vs->op_count_pending == 0) ? 1 : vs->op_count_pending;
}

static void reset_pending(wp_vimode_state *vs)
{
    vs->count_pending = 0;
    vs->op_count_pending = 0;
    vs->pending_operator = (u8)WP_VIOP_NONE;
    vs->pending_prefix = 0;
}

/* wp_pos_clamp_to_document (editing.h) is the shared, engine-level
   version of what used to be this function's own body -- promoted in
   M10 Phase 3 once Emacs's own C-x u (undo) needed the exact same
   check. Thin wrapper purely for call-site brevity (every caller below
   already has a wp_vimode_state * in hand, mirroring paste_text_at's
   own M10 Phase 1 refactor). */
static void clamp_cursor(wp_vimode_state *vs)
{
    vs->cursor = wp_pos_clamp_to_document(vs->doc, vs->cursor);
}

static wp_selection make_selection(wp_pos a, wp_pos b)
{
    wp_selection s;
    s.anchor = a;
    s.caret = b;
    return s;
}

static void ensure_insert_group_open(wp_vimode_state *vs)
{
    if (!vs->insert_group_open) {
        wp_undo_group_begin(vs->journal);
        vs->insert_group_open = WP_TRUE;
    }
}

static void enter_insert_at(wp_vimode_state *vs, wp_pos at)
{
    vs->cursor = at;
    vs->mode = (u8)WP_VIMODE_INSERT;
}

static void exit_insert_mode(wp_vimode_state *vs)
{
    if (vs->insert_group_open) {
        wp_undo_group_end(vs->journal);
        vs->insert_group_open = WP_FALSE;
    }
    vs->mode = (u8)WP_VIMODE_NORMAL;
    reset_pending(vs);
}

/* ------------------------------------------------------------------ */
/* Register (single unnamed register)                                  */
/* ------------------------------------------------------------------ */

static void yank_selection(wp_vimode_state *vs, wp_selection sel, wp_bool linewise)
{
    u32 needed = 0;

    wp_document_extract_text(vs->doc, sel, NULL, 0, &needed);
    if (needed > vs->register_cap) {
        u8 *new_buf = (u8 *)mem_alloc(needed);
        if (new_buf == NULL) return; /* OOM: register left unchanged, matches this
                                         journal's existing no-rollback posture elsewhere */
        mem_free(vs->register_bytes);
        vs->register_bytes = new_buf;
        vs->register_cap = needed;
    }
    wp_document_extract_text(vs->doc, sel, vs->register_bytes, vs->register_cap, &vs->register_len);
    vs->register_linewise = linewise;
}

/* wp_edit_paste_text (editing.h) is the shared, engine-level version of
   what used to be this function's own body -- promoted to editing.h/
   undo.c in M10 once WordStar's block copy/move needed the exact same
   "insert text, splitting a new paragraph at each embedded '\n'"
   behavior this module's own put commands already relied on. This is
   now a thin wrapper purely for call-site brevity (every caller below
   already has a wp_vimode_state * in hand). */
static wp_pos paste_text_at(wp_vimode_state *vs, wp_pos at, const u8 *bytes, u32 len)
{
    wp_pos out = at;
    wp_edit_paste_text(vs->doc, vs->journal, at, bytes, len, &out);
    return out;
}

static void put_charwise(wp_vimode_state *vs, wp_bool after)
{
    wp_pos at = after ? wp_pos_next(vs->doc, vs->cursor) : vs->cursor;
    vs->cursor = paste_text_at(vs, at, vs->register_bytes, vs->register_len);
}

static void put_linewise(wp_vimode_state *vs, wp_bool after)
{
    if (after) {
        wp_pos split_at = wp_pos_line_end(vs->doc, vs->cursor);
        wp_pos out;

        wp_edit_split_paragraph(vs->doc, vs->journal, split_at, &out);
        vs->cursor = paste_text_at(vs, out, vs->register_bytes, vs->register_len);
    } else {
        wp_pos split_at = wp_pos_line_start(vs->doc, vs->cursor);
        wp_pos new_empty;

        new_empty.section = split_at.section;
        new_empty.paragraph = split_at.paragraph;
        new_empty.run = 0;
        new_empty.offset = 0;
        /* Splitting at a paragraph's own start empties THAT paragraph
           and pushes all its original content to paragraph+1 (see
           docs/editing.md) -- so the newly-empty slot to paste into is
           at the ORIGINAL index, not the split's own out_pos. */
        wp_edit_split_paragraph(vs->doc, vs->journal, split_at, NULL);
        vs->cursor = paste_text_at(vs, new_empty, vs->register_bytes, vs->register_len);
    }
}

static void put_register(wp_vimode_state *vs, wp_bool after)
{
    if (vs->register_len == 0) return;
    if (vs->register_linewise) put_linewise(vs, after);
    else put_charwise(vs, after);
}

/* ------------------------------------------------------------------ */
/* Operators                                                            */
/* ------------------------------------------------------------------ */

/* Normalizes a charwise (anchor, caret) pair into a whole-paragraph
   range for a linewise operator (dd/cc/yy, or an operator combined with
   a linewise motion like j/k/gg/G): from the start of the earlier
   paragraph through the start of the paragraph immediately after the
   later one. If the later paragraph is the section's LAST one, there is
   no "paragraph after" to extend into -- extend the START backward
   instead (merging into the paragraph before, if one exists), matching
   real vim's "dd on the last line merges upward" behavior instead of
   just emptying the last paragraph and leaving an orphaned blank one
   behind. See docs/vimode.md. */
static wp_selection normalize_linewise_selection(wp_vimode_state *vs, wp_selection sel)
{
    wp_pos lo, hi, start, end;
    const wp_section *sec;

    if (wp_pos_compare(sel.anchor, sel.caret) <= 0) { lo = sel.anchor; hi = sel.caret; }
    else { lo = sel.caret; hi = sel.anchor; }

    sec = wp_document_section_get(vs->doc, hi.section);

    if (sec != NULL && hi.paragraph + 1 < sec->para_count) {
        start.section = lo.section; start.paragraph = lo.paragraph; start.run = 0; start.offset = 0;
        end.section = hi.section; end.paragraph = hi.paragraph + 1; end.run = 0; end.offset = 0;
    } else if (sec != NULL && lo.paragraph > 0) {
        start.section = lo.section;
        start.paragraph = lo.paragraph - 1;
        paragraph_end_run_offset(&sec->paras[lo.paragraph - 1], &start.run, &start.offset);
        end = wp_pos_doc_end(vs->doc);
    } else {
        start.section = lo.section; start.paragraph = lo.paragraph; start.run = 0; start.offset = 0;
        end = wp_pos_doc_end(vs->doc);
    }

    return make_selection(start, end);
}

static void perform_operator(wp_vimode_state *vs, u8 op, wp_selection sel, wp_bool linewise)
{
    wp_pos out_pos;

    if (linewise) sel = normalize_linewise_selection(vs, sel);

    if (op == (u8)WP_VIOP_YANK) {
        yank_selection(vs, sel, linewise);
        vs->cursor = (wp_pos_compare(sel.anchor, sel.caret) <= 0) ? sel.anchor : sel.caret;
        return;
    }

    if (op == (u8)WP_VIOP_CHANGE) ensure_insert_group_open(vs);

    yank_selection(vs, sel, linewise);
    wp_edit_delete_selection(vs->doc, vs->journal, sel, &out_pos);
    vs->cursor = out_pos;

    if (op == (u8)WP_VIOP_CHANGE) enter_insert_at(vs, out_pos);
}

static void apply_linewise_current_lines(wp_vimode_state *vs, u8 op, u32 count)
{
    wp_pos target = vs->cursor;
    u32 i;

    for (i = 1; i < count; i++) target = wp_pos_paragraph_down(vs->doc, target);
    perform_operator(vs, op, make_selection(vs->cursor, target), WP_TRUE);
}

static void delete_chars_forward(wp_vimode_state *vs, u32 n)
{
    wp_pos target = vs->cursor;
    wp_selection sel;
    wp_pos out;
    u32 i;

    for (i = 0; i < n; i++) target = wp_pos_next(vs->doc, target);
    if (wp_pos_compare(target, vs->cursor) == 0) return;

    sel = make_selection(vs->cursor, target);
    yank_selection(vs, sel, WP_FALSE);
    wp_edit_delete_selection(vs->doc, vs->journal, sel, &out);
    vs->cursor = out;
}

/* ------------------------------------------------------------------ */
/* Motion resolution                                                    */
/* ------------------------------------------------------------------ */

static wp_pos goto_paragraph_1indexed(wp_vimode_state *vs, u32 n)
{
    const wp_section *sec = wp_document_section_get(vs->doc, vs->cursor.section);
    wp_pos out;
    u32 idx = (n == 0) ? 0 : n - 1;

    out.section = vs->cursor.section;
    if (sec != NULL && sec->para_count > 0 && idx >= sec->para_count) idx = sec->para_count - 1;
    out.paragraph = idx;
    out.run = 0;
    out.offset = 0;
    return out;
}

/* The classic "dw doesn't eat the newline" rule: a bare w/W motion
   legitimately crosses into the next paragraph (see
   test_motion_word_forward_crosses_paragraph_boundary in
   test_motion.c), but an OPERATOR+w/W must not extend the edit across a
   paragraph break it didn't ask for -- clip back to the end of the
   cursor's own line instead. Only applies when an operator is actually
   pending; a bare motion (just moving the cursor) is unaffected. */
static wp_pos clip_word_forward_to_line(wp_vimode_state *vs, wp_pos target)
{
    if (vs->pending_operator != (u8)WP_VIOP_NONE && target.paragraph != vs->cursor.paragraph) {
        return wp_pos_line_end(vs->doc, vs->cursor);
    }
    return target;
}

static wp_bool resolve_motion(wp_vimode_state *vs, wp_key_event key, wp_pos *out_target, wp_bool *out_linewise)
{
    u32 n = effective_count(vs);
    wp_pos pos = vs->cursor;
    u32 i;

    if (key.special != (u16)WP_KEY_NONE) {
        switch (key.special) {
        case WP_KEY_LEFT:  for (i = 0; i < n; i++) pos = wp_pos_prev(vs->doc, pos); *out_linewise = WP_FALSE; break;
        case WP_KEY_RIGHT: for (i = 0; i < n; i++) pos = wp_pos_next(vs->doc, pos); *out_linewise = WP_FALSE; break;
        case WP_KEY_UP:    for (i = 0; i < n; i++) pos = wp_pos_paragraph_up(vs->doc, pos);   *out_linewise = WP_TRUE; break;
        case WP_KEY_DOWN:  for (i = 0; i < n; i++) pos = wp_pos_paragraph_down(vs->doc, pos); *out_linewise = WP_TRUE; break;
        case WP_KEY_HOME:  pos = wp_pos_line_start(vs->doc, pos); *out_linewise = WP_FALSE; break;
        case WP_KEY_END:   pos = wp_pos_line_end(vs->doc, pos);   *out_linewise = WP_FALSE; break;
        default: return WP_FALSE;
        }
        *out_target = pos;
        return WP_TRUE;
    }

    /* A modified key (Ctrl+<letter> etc.) is never a bare motion letter --
       without this, a host embedding vi-mode alongside its own Ctrl+<letter>
       shortcuts (e.g. Ctrl+B for Bold) would have those chords silently
       swallowed as the plain letter's motion ('b' word-backward here),
       since wp_key_event deliberately reuses the plain letter's codepoint
       for Ctrl-chords (see keyevent.h). Found integrating M8.5's Atari
       retrofit -- see docs/vimode.md. */
    if (key.mods != 0) return WP_FALSE;

    switch (key.codepoint) {
    case 'h': for (i = 0; i < n; i++) pos = wp_pos_prev(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case 'l': for (i = 0; i < n; i++) pos = wp_pos_next(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case 'j': for (i = 0; i < n; i++) pos = wp_pos_paragraph_down(vs->doc, pos); *out_linewise = WP_TRUE; break;
    case 'k': for (i = 0; i < n; i++) pos = wp_pos_paragraph_up(vs->doc, pos);   *out_linewise = WP_TRUE; break;
    /* Real vim special case (":help cw"): "cw"/"cW" do NOT include
       trailing whitespace the way "dw"/"dW" do -- they behave like
       "ce"/"cE" instead. Only applies to the CHANGE operator; a bare
       w/W motion, and d/y combined with w/W, are unaffected. */
    case 'w':
        if (vs->pending_operator == (u8)WP_VIOP_CHANGE) {
            for (i = 0; i < n; i++) pos = wp_pos_word_end(vs->doc, pos, WP_FALSE);
        } else {
            for (i = 0; i < n; i++) pos = wp_pos_word_forward(vs->doc, pos, WP_FALSE);
            pos = clip_word_forward_to_line(vs, pos);
        }
        *out_linewise = WP_FALSE;
        break;
    case 'W':
        if (vs->pending_operator == (u8)WP_VIOP_CHANGE) {
            for (i = 0; i < n; i++) pos = wp_pos_word_end(vs->doc, pos, WP_TRUE);
        } else {
            for (i = 0; i < n; i++) pos = wp_pos_word_forward(vs->doc, pos, WP_TRUE);
            pos = clip_word_forward_to_line(vs, pos);
        }
        *out_linewise = WP_FALSE;
        break;
    case 'b': for (i = 0; i < n; i++) pos = wp_pos_word_backward(vs->doc, pos, WP_FALSE); *out_linewise = WP_FALSE; break;
    case 'B': for (i = 0; i < n; i++) pos = wp_pos_word_backward(vs->doc, pos, WP_TRUE);  *out_linewise = WP_FALSE; break;
    case 'e': for (i = 0; i < n; i++) pos = wp_pos_word_end(vs->doc, pos, WP_FALSE); *out_linewise = WP_FALSE; break;
    case 'E': for (i = 0; i < n; i++) pos = wp_pos_word_end(vs->doc, pos, WP_TRUE);  *out_linewise = WP_FALSE; break;
    case '0': pos = wp_pos_line_start(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case '^': pos = wp_pos_line_first_nonblank(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case '$': pos = wp_pos_line_end(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case '{': pos = wp_pos_para_block_backward(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case '}': pos = wp_pos_para_block_forward(vs->doc, pos); *out_linewise = WP_FALSE; break;
    case 'G':
        pos = (vs->count_pending > 0) ? goto_paragraph_1indexed(vs, vs->count_pending) : wp_pos_doc_end(vs->doc);
        *out_linewise = WP_TRUE;
        break;
    default:
        return WP_FALSE;
    }

    *out_target = pos;
    return WP_TRUE;
}

static wp_vimode_action handle_resolved_motion(wp_vimode_state *vs, wp_pos target, wp_bool linewise)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_NONE;
    action.arg_len = 0;

    if (vs->pending_operator != (u8)WP_VIOP_NONE) {
        perform_operator(vs, vs->pending_operator, make_selection(vs->cursor, target), linewise);
    } else {
        vs->cursor = target;
    }
    reset_pending(vs);
    return action;
}

/* ------------------------------------------------------------------ */
/* Insert mode                                                          */
/* ------------------------------------------------------------------ */

static void insert_backspace(wp_vimode_state *vs)
{
    if (vs->cursor.run == 0 && vs->cursor.offset == 0 && vs->cursor.paragraph > 0) {
        wp_pos out;
        wp_edit_merge_paragraph(vs->doc, vs->journal, vs->cursor.section, vs->cursor.paragraph - 1, &out);
        vs->cursor = out;
    } else {
        wp_pos prev = wp_pos_prev(vs->doc, vs->cursor);
        if (wp_pos_compare(prev, vs->cursor) != 0) {
            wp_selection sel = make_selection(prev, vs->cursor);
            wp_pos out;
            wp_edit_delete_selection(vs->doc, vs->journal, sel, &out);
            vs->cursor = out;
        }
    }
}

static wp_vimode_action feed_insert(wp_vimode_state *vs, wp_key_event key)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_NONE;
    action.arg_len = 0;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        exit_insert_mode(vs);
        return action;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        wp_pos out;
        wp_edit_split_paragraph(vs->doc, vs->journal, vs->cursor, &out);
        vs->cursor = out;
        return action;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        insert_backspace(vs);
        return action;
    }
    /* mods == 0 here for the same reason resolve_motion requires it --
       otherwise a Ctrl+<letter> app-level shortcut (Bold, Save, ...)
       fed through this module while composing text would be typed
       literally instead of reaching the caller as an unhandled key. */
    if (key.special == (u16)WP_KEY_NONE && key.mods == 0 && key.codepoint >= 0x20) {
        u8 utf8_buf[4];
        u32 n = utf8_encode(key.codepoint, utf8_buf);
        if (n > 0) {
            wp_pos out;
            wp_edit_insert_text(vs->doc, vs->journal, vs->cursor, utf8_buf, n, &out);
            vs->cursor = out;
        }
        return action;
    }
    return action;
}

/* ------------------------------------------------------------------ */
/* Normal mode                                                          */
/* ------------------------------------------------------------------ */

static wp_vimode_action feed_normal(wp_vimode_state *vs, wp_key_event key)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_NONE;
    action.arg_len = 0;

    if (vs->pending_prefix == 'g') {
        vs->pending_prefix = 0;
        if (key.special == (u16)WP_KEY_NONE && key.mods == 0 && key.codepoint == 'g') {
            wp_pos target = (vs->count_pending > 0)
                ? goto_paragraph_1indexed(vs, vs->count_pending)
                : wp_pos_doc_start(vs->doc);
            return handle_resolved_motion(vs, target, WP_TRUE);
        }
        /* not a real "gg" -- cancel the prefix and keep processing this
           key normally below, matching real vim (an invalid g-command
           just drops back to Normal mode with the key otherwise live) */
    }

    if (key.special == (u16)WP_KEY_ESCAPE) { reset_pending(vs); return action; }

    /* See resolve_motion's own comment -- a Ctrl+<letter> chord must not
       be treated as the bare letter for count-accumulation, the 'g'
       prefix, or the d/c/y operator either, only the redo chord below is
       a deliberate exception. */
    if (key.special == (u16)WP_KEY_NONE && key.mods != 0 &&
        !(key.codepoint == 'r' && (key.mods & (u8)WP_MOD_CTRL))) {
        vs->count_pending = 0;
        action.kind = (u8)WP_VIACTION_BEEP;
        return action;
    }

    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= '0' && key.codepoint <= '9' &&
        !(key.codepoint == '0' && vs->count_pending == 0)) {
        vs->count_pending = vs->count_pending * 10 + (key.codepoint - '0');
        return action;
    }

    if (key.special == (u16)WP_KEY_NONE && key.codepoint == 'g') {
        vs->pending_prefix = 'g';
        return action;
    }

    if (key.special == (u16)WP_KEY_NONE &&
        (key.codepoint == 'd' || key.codepoint == 'c' || key.codepoint == 'y')) {
        u8 op = (key.codepoint == 'd') ? (u8)WP_VIOP_DELETE
              : (key.codepoint == 'c') ? (u8)WP_VIOP_CHANGE : (u8)WP_VIOP_YANK;

        if (vs->pending_operator == op) {
            apply_linewise_current_lines(vs, op, effective_op_count(vs) * effective_count(vs));
            reset_pending(vs);
            return action;
        }
        vs->pending_operator = op;
        vs->op_count_pending = vs->count_pending;
        vs->count_pending = 0;
        return action;
    }

    {
        wp_pos target;
        wp_bool linewise = WP_FALSE;
        if (resolve_motion(vs, key, &target, &linewise)) {
            return handle_resolved_motion(vs, target, linewise);
        }
    }

    if (vs->pending_operator != (u8)WP_VIOP_NONE) reset_pending(vs);

    if (key.special == (u16)WP_KEY_NONE) {
        if (key.codepoint == 'r' && (key.mods & (u8)WP_MOD_CTRL)) {
            wp_edit_redo(vs->doc, vs->journal);
            clamp_cursor(vs);
        } else switch (key.codepoint) {
        case 'i':
            ensure_insert_group_open(vs);
            enter_insert_at(vs, vs->cursor);
            break;
        case 'a': {
            wp_pos at = wp_pos_next(vs->doc, vs->cursor);
            ensure_insert_group_open(vs);
            enter_insert_at(vs, at);
            break;
        }
        case 'I': {
            wp_pos at = wp_pos_line_first_nonblank(vs->doc, vs->cursor);
            ensure_insert_group_open(vs);
            enter_insert_at(vs, at);
            break;
        }
        case 'A': {
            wp_pos at = wp_pos_line_end(vs->doc, vs->cursor);
            ensure_insert_group_open(vs);
            enter_insert_at(vs, at);
            break;
        }
        case 'o': {
            wp_pos out;
            ensure_insert_group_open(vs);
            wp_edit_split_paragraph(vs->doc, vs->journal, wp_pos_line_end(vs->doc, vs->cursor), &out);
            enter_insert_at(vs, out);
            break;
        }
        case 'O': {
            wp_pos line_start = wp_pos_line_start(vs->doc, vs->cursor);
            wp_pos new_empty;

            new_empty.section = line_start.section;
            new_empty.paragraph = line_start.paragraph;
            new_empty.run = 0;
            new_empty.offset = 0;
            ensure_insert_group_open(vs);
            wp_edit_split_paragraph(vs->doc, vs->journal, line_start, NULL);
            enter_insert_at(vs, new_empty);
            break;
        }
        case 'x':
            delete_chars_forward(vs, effective_count(vs));
            break;
        case 'p':
            put_register(vs, WP_TRUE);
            break;
        case 'P':
            put_register(vs, WP_FALSE);
            break;
        case 'u':
            wp_edit_undo(vs->doc, vs->journal);
            clamp_cursor(vs);
            break;
        case 'v':
            vs->mode = (u8)WP_VIMODE_VISUAL;
            vs->visual.anchor = vs->cursor;
            vs->visual.caret = vs->cursor;
            break;
        case 'V':
            vs->mode = (u8)WP_VIMODE_VISUAL_LINE;
            vs->visual.anchor = vs->cursor;
            vs->visual.caret = vs->cursor;
            break;
        case ':':
            vs->mode = (u8)WP_VIMODE_COMMAND;
            vs->cmdline_prefix = ':';
            vs->cmdline_len = 0;
            break;
        case '/':
            vs->mode = (u8)WP_VIMODE_COMMAND;
            vs->cmdline_prefix = '/';
            vs->cmdline_len = 0;
            break;
        case '?':
            vs->mode = (u8)WP_VIMODE_COMMAND;
            vs->cmdline_prefix = '?';
            vs->cmdline_len = 0;
            break;
        case 'n':
            if (vs->last_search_len > 0) {
                wp_pos found;
                if (wp_vimode_search(vs->doc, vs->cursor, vs->last_search, vs->last_search_len,
                                      vs->last_search_forward, &found) == WP_OK) {
                    vs->cursor = found;
                }
            }
            break;
        case 'N':
            if (vs->last_search_len > 0) {
                wp_pos found;
                if (wp_vimode_search(vs->doc, vs->cursor, vs->last_search, vs->last_search_len,
                                      (wp_bool)!vs->last_search_forward, &found) == WP_OK) {
                    vs->cursor = found;
                }
            }
            break;
        default:
            action.kind = (u8)WP_VIACTION_BEEP;
            break;
        }
    } else {
        action.kind = (u8)WP_VIACTION_BEEP;
    }

    vs->count_pending = 0;
    return action;
}

/* ------------------------------------------------------------------ */
/* Visual / Visual-Line mode                                            */
/* ------------------------------------------------------------------ */

static wp_vimode_action feed_visual(wp_vimode_state *vs, wp_key_event key)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_NONE;
    action.arg_len = 0;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->mode = (u8)WP_VIMODE_NORMAL;
        reset_pending(vs);
        return action;
    }

    /* See resolve_motion's/feed_normal's own comment -- a Ctrl+<letter>
       chord is never a Visual-mode command letter either. */
    if (key.special == (u16)WP_KEY_NONE && key.mods != 0) {
        vs->count_pending = 0;
        action.kind = (u8)WP_VIACTION_BEEP;
        return action;
    }

    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= '0' && key.codepoint <= '9' &&
        !(key.codepoint == '0' && vs->count_pending == 0)) {
        vs->count_pending = vs->count_pending * 10 + (key.codepoint - '0');
        return action;
    }

    {
        wp_pos target;
        wp_bool linewise = WP_FALSE;
        if (resolve_motion(vs, key, &target, &linewise)) {
            /* resolve_motion always reads FROM vs->cursor -- keep it in
               sync with visual.caret so a second motion (e.g. the
               repeated 'w' in "vwwd") chains from where the selection
               now extends to, not from the position 'v' was pressed
               at. */
            vs->cursor = target;
            vs->visual.caret = target;
            vs->count_pending = 0;
            return action;
        }
    }

    if (key.special == (u16)WP_KEY_NONE) {
        wp_bool line_mode = (vs->mode == (u8)WP_VIMODE_VISUAL_LINE);

        switch (key.codepoint) {
        case 'd': case 'x':
            perform_operator(vs, (u8)WP_VIOP_DELETE, vs->visual, line_mode);
            vs->mode = (u8)WP_VIMODE_NORMAL;
            break;
        case 'y':
            perform_operator(vs, (u8)WP_VIOP_YANK, vs->visual, line_mode);
            vs->mode = (u8)WP_VIMODE_NORMAL;
            break;
        case 'c':
            perform_operator(vs, (u8)WP_VIOP_CHANGE, vs->visual, line_mode);
            /* perform_operator already transitions to INSERT mode for CHANGE */
            break;
        default:
            break;
        }
    }

    vs->count_pending = 0;
    return action;
}

/* ------------------------------------------------------------------ */
/* Command-line mode (':' and '/')                                     */
/* ------------------------------------------------------------------ */

/* Splits an ex command line into its leading command word and an
   optional filename argument. Returns the command word's length; the
   argument (if any) is reported through the arg_off and arg_len
   out-params, with both leading and trailing spaces stripped.

   Deliberately simple: the command word is the leading run of letters,
   the separator is one or more spaces, and everything after it is the
   argument verbatim. No quoting, no escaping -- none of this project's
   targets has a path where that matters (8.3 on Atari/DOS, and neither
   AmigaDOS nor an ExtFS share needs a quoted name for the filenames
   this app produces). */
static u32 split_ex_command(const u8 *buf, u32 len, u32 *arg_off, u32 *arg_len)
{
    u32 word = 0;
    u32 i;

    while (word < len && ((buf[word] >= 'a' && buf[word] <= 'z') ||
                          (buf[word] >= 'A' && buf[word] <= 'Z'))) {
        word++;
    }

    i = word;
    while (i < len && buf[i] == ' ') i++;
    *arg_off = i;

    while (len > i && buf[len - 1] == ' ') len--;
    *arg_len = (len > i) ? (len - i) : 0;

    return word;
}

static wp_bool ex_word_is(const u8 *buf, u32 word_len, const char *lit)
{
    u32 i;

    for (i = 0; i < word_len; i++) {
        if (lit[i] == '\0' || buf[i] != (u8)lit[i]) return WP_FALSE;
    }
    return (wp_bool)(lit[word_len] == '\0');
}

/* Recognizes ':w', ':q', ':x', ':wq' and ':e', each of the file-touching
   ones optionally followed by a filename (M-keyboard-ergonomics work:
   naming a file without reaching for the mouse, which on three of the
   four backends is otherwise only possible through a mouse-driven native
   file picker -- see docs/vimode.md).

   Bare forms keep their exact previous meaning: ':w' still means "save
   however you normally would", and ':e' likewise means "open however you
   normally would", i.e. the backend raises its own native picker. */
static wp_vimode_action execute_ex_command(wp_vimode_state *vs)
{
    wp_vimode_action action;
    u32 word_len, arg_off, arg_len;
    const u8 *buf = vs->cmdline_buf;

    action.kind = (u8)WP_VIACTION_BEEP;
    action.arg_len = 0;

    word_len = split_ex_command(buf, vs->cmdline_len, &arg_off, &arg_len);
    if (word_len == 0) return action;

    if (ex_word_is(buf, word_len, "w"))       action.kind = (u8)WP_VIACTION_SAVE;
    else if (ex_word_is(buf, word_len, "q"))  action.kind = (u8)WP_VIACTION_QUIT;
    else if (ex_word_is(buf, word_len, "x"))  action.kind = (u8)WP_VIACTION_SAVE_QUIT;
    else if (ex_word_is(buf, word_len, "wq")) action.kind = (u8)WP_VIACTION_SAVE_QUIT;
    else if (ex_word_is(buf, word_len, "e"))  action.kind = (u8)WP_VIACTION_OPEN;
    else return action;

    if (arg_len == 0) return action;

    /* ':q' takes no argument -- real vim rejects ":q foo" (E488 trailing
       characters) rather than quietly ignoring it, and so does this.
       Silently dropping it would be worse here than anywhere: the user
       who typed ":q somefile" plainly meant something else. */
    if (action.kind == (u8)WP_VIACTION_QUIT) {
        action.kind = (u8)WP_VIACTION_BEEP;
        return action;
    }

    /* Reject rather than truncate. A truncated path names a DIFFERENT
       file, and on ':w' that is silent data loss. */
    if (arg_len > (u32)WP_VIMODE_ACTION_ARG_MAX) {
        action.kind = (u8)WP_VIACTION_BEEP;
        return action;
    }

    memcpy(action.arg, buf + arg_off, arg_len);
    action.arg_len = arg_len;
    return action;
}

static void execute_search(wp_vimode_state *vs)
{
    wp_pos found;
    wp_bool forward;

    if (vs->cmdline_len == 0 || vs->cmdline_len > WP_VIMODE_SEARCH_MAX) return;

    memcpy(vs->last_search, vs->cmdline_buf, vs->cmdline_len);
    vs->last_search_len = vs->cmdline_len;
    forward = (wp_bool)(vs->cmdline_prefix != '?');
    vs->last_search_forward = forward;

    if (wp_vimode_search(vs->doc, vs->cursor, vs->cmdline_buf, vs->cmdline_len, forward, &found) == WP_OK) {
        vs->cursor = found;
        vs->last_error = WP_OK;
    } else {
        vs->last_error = WP_ERR;
    }
}

/* ------------------------------------------------------------------ */
/* Substitute (':s/old/new/flags', ':%s/old/new/flags') -- M11          */
/* ------------------------------------------------------------------ */

/* Literal-only grammar (real vim's regex/arbitrary-delimiter support is
   deliberately not implemented -- see docs/keymaps.md's own "no regex"
   scope note for wp_document_find_text, which this inherits): an
   optional leading '%' (whole section, real vim's own single-section
   scope narrowing -- matches wp_document_extract_text/
   wp_edit_delete_selection's own existing boundary), then 's', then
   '/old/new/' with '/' as the only recognized delimiter, then any
   combination of the 'g'/'c'/'i' flag letters. Any other trailing
   character makes the whole command unrecognized (falls through to
   execute_ex_command's own BEEP). */
static wp_bool parse_substitute(const u8 *buf, u32 len, wp_bool *whole_section,
                                 u32 *old_off, u32 *old_len,
                                 u32 *new_off, u32 *new_len,
                                 wp_bool *g, wp_bool *c, wp_bool *i)
{
    u32 pos = 0;
    const u8 *slash;

    *whole_section = WP_FALSE;
    if (pos < len && buf[pos] == '%') { *whole_section = WP_TRUE; pos++; }
    if (pos >= len || buf[pos] != 's') return WP_FALSE;
    pos++;
    if (pos >= len || buf[pos] != '/') return WP_FALSE;
    pos++;

    *old_off = pos;
    slash = (const u8 *)memchr(buf + pos, '/', len - pos);
    if (!slash) return WP_FALSE;
    *old_len = (u32)(slash - (buf + pos));
    pos = (u32)(slash - buf) + 1;

    *new_off = pos;
    slash = (const u8 *)memchr(buf + pos, '/', len - pos);
    if (!slash) return WP_FALSE;
    *new_len = (u32)(slash - (buf + pos));
    pos = (u32)(slash - buf) + 1;

    *g = WP_FALSE; *c = WP_FALSE; *i = WP_FALSE;
    for (; pos < len; pos++) {
        if (buf[pos] == 'g') *g = WP_TRUE;
        else if (buf[pos] == 'c') *c = WP_TRUE;
        else if (buf[pos] == 'i') *i = WP_TRUE;
        else return WP_FALSE;
    }
    return WP_TRUE;
}

/* Bounded, non-wrapping, INCLUSIVE-of-from_byte scan within a single
   paragraph -- deliberately NOT wp_document_find_text (editing.h),
   whose "strictly after `from`, wraps around the whole document"
   contract fits interactive cursor-relative search, not this function's
   own "resume this bounded paragraph range exactly where the last
   match/replace left off" job. Small, self-contained, honest duplication
   of wp_document_find_text's own per-paragraph scratch-buffer scan
   rather than forcing a mismatched API to fit. */
static wp_bool find_match_in_paragraph_from(const wp_vimode_state *vs, u32 pi, u32 from_byte,
                                             u32 *out_start_byte)
{
    const wp_section *sec = &vs->doc->sections[vs->cursor.section];
    const wp_paragraph *para;
    wp_selection sel;
    u8 scratch[512];
    u32 total_len, len, s;

    if (pi >= sec->para_count) return WP_FALSE;
    para = &sec->paras[pi];

    sel.anchor.section = vs->cursor.section; sel.anchor.paragraph = pi;
    sel.anchor.run = 0; sel.anchor.offset = 0;
    sel.caret.section = vs->cursor.section; sel.caret.paragraph = pi;
    paragraph_end_run_offset(para, &sel.caret.run, &sel.caret.offset);
    wp_document_extract_text(vs->doc, sel, scratch, (u32)sizeof(scratch), &total_len);
    len = (total_len < (u32)sizeof(scratch)) ? total_len : (u32)sizeof(scratch);

    if (len < vs->subst_old_len || from_byte > len - vs->subst_old_len) return WP_FALSE;

    for (s = from_byte; s <= len - vs->subst_old_len; s++) {
        wp_bool eq;
        if (!vs->subst_ignore_case) {
            eq = (wp_bool)(memcmp(scratch + s, vs->subst_old, vs->subst_old_len) == 0);
        } else {
            u32 k;
            eq = WP_TRUE;
            for (k = 0; k < vs->subst_old_len; k++) {
                u8 a = scratch[s + k], b = vs->subst_old[k];
                if (a >= 'A' && a <= 'Z') a = (u8)(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = (u8)(b - 'A' + 'a');
                if (a != b) { eq = WP_FALSE; break; }
            }
        }
        if (eq) { *out_start_byte = s; return WP_TRUE; }
    }
    return WP_FALSE;
}

/* The byte offset of vs->subst_match within vs->subst_paragraph -- the
   inverse of pos_from_byte_in_paragraph, needed to resume scanning past
   a confirm-mode match the caller just handled (y/n/a). */
static u32 subst_match_byte_offset(const wp_vimode_state *vs)
{
    const wp_section *sec = &vs->doc->sections[vs->cursor.section];
    const wp_paragraph *para = &sec->paras[vs->subst_paragraph];
    u32 byte = 0, r;

    for (r = 0; r < vs->subst_match.run; r++) byte += para->runs[r].text.length;
    byte += vs->subst_match.offset;
    return byte;
}

/* Scans forward from (subst_paragraph, subst_scan_offset) through
   subst_last_paragraph for the next match. Pauses in
   WP_VIMODE_SUBSTITUTE_CONFIRM with subst_match/vs->cursor set to it if
   found; leaves mode at WP_VIMODE_NORMAL (nothing left to confirm) once
   the whole scope is exhausted. */
static void advance_to_next_match(wp_vimode_state *vs)
{
    for (;;) {
        u32 start_byte;

        if (vs->subst_paragraph > vs->subst_last_paragraph) {
            vs->mode = (u8)WP_VIMODE_NORMAL;
            return;
        }
        if (find_match_in_paragraph_from(vs, vs->subst_paragraph, vs->subst_scan_offset, &start_byte)) {
            const wp_section *sec = &vs->doc->sections[vs->cursor.section];
            const wp_paragraph *para = &sec->paras[vs->subst_paragraph];

            vs->subst_match = pos_from_byte_in_paragraph(vs->cursor.section, vs->subst_paragraph,
                                                           para, start_byte);
            vs->cursor = vs->subst_match;
            vs->mode = (u8)WP_VIMODE_SUBSTITUTE_CONFIRM;
            return;
        }
        vs->subst_paragraph++;
        vs->subst_scan_offset = 0;
    }
}

/* Replaces every match in [start_para, end_para] starting no earlier
   than start_byte within start_para -- honoring subst_global (every
   match per paragraph vs. just the first) -- as ONE undo group (real
   vim's own ":s" is a single undo step regardless of how many matches
   it touched). Used both by the non-confirm top-level path and by
   confirm mode's 'a' ("replace this and all remaining"). Returns the
   number of replacements made. */
static u32 substitute_range(wp_vimode_state *vs, u32 start_para, u32 start_byte, u32 end_para)
{
    u32 pi = start_para;
    u32 from_byte = start_byte;
    u32 count = 0;
    wp_pos last_pos = vs->cursor;

    wp_undo_group_begin(vs->journal);
    for (; pi <= end_para; pi++, from_byte = 0) {
        for (;;) {
            u32 start;
            wp_pos match_pos, out_pos;
            wp_selection sel;
            const wp_section *sec;
            const wp_paragraph *para;

            if (!find_match_in_paragraph_from(vs, pi, from_byte, &start)) break;

            sec = &vs->doc->sections[vs->cursor.section];
            para = &sec->paras[pi];
            match_pos = pos_from_byte_in_paragraph(vs->cursor.section, pi, para, start);
            sel = wp_document_match_selection(vs->doc, match_pos, vs->subst_old_len);
            wp_edit_delete_selection(vs->doc, vs->journal, sel, &out_pos);
            wp_edit_paste_text(vs->doc, vs->journal, out_pos, vs->subst_new, vs->subst_new_len, &out_pos);
            last_pos = out_pos;
            count++;

            if (!vs->subst_global) break;
            from_byte = start + vs->subst_new_len;
        }
    }
    wp_undo_group_end(vs->journal);

    vs->cursor = wp_pos_clamp_to_document(vs->doc, last_pos);
    return count;
}

/* Recognizes and runs a ':s'/'%s' command out of vs->cmdline_buf.
   Returns WP_FALSE if the buffer isn't a substitute command at all (the
   caller falls through to execute_ex_command's own w/q/x/wq/BEEP
   handling); WP_TRUE otherwise, whether or not any match was found --
   matching execute_search's own "recognized command, use last_error for
   the miss case" precedent rather than BEEP. */
static wp_bool execute_substitute(wp_vimode_state *vs)
{
    wp_bool whole_section, g, c, i;
    u32 old_off, old_len, new_off, new_len;
    u32 start_para, end_para;

    if (!parse_substitute(vs->cmdline_buf, vs->cmdline_len, &whole_section,
                           &old_off, &old_len, &new_off, &new_len, &g, &c, &i)) {
        return WP_FALSE;
    }
    if (old_len == 0) {
        vs->last_error = WP_ERR;
        return WP_TRUE;
    }

    memcpy(vs->subst_old, vs->cmdline_buf + old_off, old_len);
    vs->subst_old_len = old_len;
    memcpy(vs->subst_new, vs->cmdline_buf + new_off, new_len);
    vs->subst_new_len = new_len;
    vs->subst_global = g;
    vs->subst_ignore_case = i;

    {
        const wp_section *sec = &vs->doc->sections[vs->cursor.section];
        start_para = whole_section ? 0 : vs->cursor.paragraph;
        end_para = whole_section ? (sec->para_count > 0 ? sec->para_count - 1 : 0) : vs->cursor.paragraph;
    }
    vs->subst_paragraph = start_para;
    vs->subst_last_paragraph = end_para;
    vs->subst_scan_offset = 0;

    if (c) {
        advance_to_next_match(vs);
        vs->last_error = (vs->mode == (u8)WP_VIMODE_SUBSTITUTE_CONFIRM) ? WP_OK : WP_ERR;
    } else {
        u32 count = substitute_range(vs, start_para, 0, end_para);
        vs->last_error = (count > 0) ? WP_OK : WP_ERR;
    }
    return WP_TRUE;
}

static wp_vimode_action feed_command(wp_vimode_state *vs, wp_key_event key)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_NONE;
    action.arg_len = 0;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->mode = (u8)WP_VIMODE_NORMAL;
        vs->cmdline_len = 0;
        vs->cmdline_prefix = 0;
        return action;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (vs->cmdline_len > 0) {
            vs->cmdline_len--;
        } else {
            vs->mode = (u8)WP_VIMODE_NORMAL;
            vs->cmdline_prefix = 0;
        }
        return action;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        if (vs->cmdline_prefix == ':') {
            if (!execute_substitute(vs)) action = execute_ex_command(vs);
        } else if (vs->cmdline_prefix == '/' || vs->cmdline_prefix == '?') {
            execute_search(vs);
        }
        vs->cmdline_len = 0;
        vs->cmdline_prefix = 0;
        /* execute_substitute may have already moved to SUBSTITUTE_CONFIRM
           (the 'c' flag paused at a match) -- don't stomp that back to
           NORMAL. */
        if (vs->mode != (u8)WP_VIMODE_SUBSTITUTE_CONFIRM) vs->mode = (u8)WP_VIMODE_NORMAL;
        return action;
    }
    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (vs->cmdline_len < WP_VIMODE_CMDLINE_MAX - 1) {
            vs->cmdline_buf[vs->cmdline_len++] = (u8)key.codepoint;
        }
    }
    return action;
}

/* ':s///c' paused at a match -- y (replace, next), n (skip, next), a
   (replace this and every remaining match, no further confirmation),
   q/Escape (stop). Real vim's fuller vocabulary (l/^/C-e/C-y/recursive
   edit) is deliberately not implemented, matching this milestone's
   already-established "scoped subset, documented not faked" precedent
   for Emacs's own query-replace. */
static wp_vimode_action feed_substitute_confirm(wp_vimode_state *vs, wp_key_event key)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_NONE;
    action.arg_len = 0;

    if (key.special == (u16)WP_KEY_ESCAPE) {
        vs->mode = (u8)WP_VIMODE_NORMAL;
        return action;
    }
    if (key.special != (u16)WP_KEY_NONE) return action;

    switch (key.codepoint) {
    case 'y': {
        wp_selection sel = wp_document_match_selection(vs->doc, vs->subst_match, vs->subst_old_len);
        u32 match_byte = subst_match_byte_offset(vs);
        wp_pos out_pos;

        wp_edit_delete_selection(vs->doc, vs->journal, sel, &out_pos);
        wp_edit_paste_text(vs->doc, vs->journal, out_pos, vs->subst_new, vs->subst_new_len, &out_pos);
        vs->cursor = out_pos;

        if (vs->subst_global) {
            vs->subst_scan_offset = match_byte + vs->subst_new_len;
        } else {
            vs->subst_paragraph++;
            vs->subst_scan_offset = 0;
        }
        advance_to_next_match(vs);
        break;
    }
    case 'n': {
        u32 match_byte = subst_match_byte_offset(vs);

        if (vs->subst_global) {
            vs->subst_scan_offset = match_byte + vs->subst_old_len;
        } else {
            vs->subst_paragraph++;
            vs->subst_scan_offset = 0;
        }
        advance_to_next_match(vs);
        break;
    }
    case 'a': {
        u32 match_byte = subst_match_byte_offset(vs);

        vs->mode = (u8)WP_VIMODE_NORMAL;
        substitute_range(vs, vs->subst_paragraph, match_byte, vs->subst_last_paragraph);
        break;
    }
    case 'q':
        vs->mode = (u8)WP_VIMODE_NORMAL;
        break;
    default:
        break;
    }
    return action;
}

/* ------------------------------------------------------------------ */
/* Search                                                                */
/* ------------------------------------------------------------------ */

/* Thin wrapper over the shared wp_document_find_text (search.c, M11) --
   forward-or-backward, case-sensitive, whole-word-insensitive, exactly
   vi's own historical `/`/`?`/`n`/`N` semantics. Promoted (M11) once a
   second scheme needed the identical byte-literal document search this
   function's own doc comment already predicted. */
wp_status wp_vimode_search(const wp_document *doc, wp_pos from,
                            const u8 *needle, u32 needle_len,
                            wp_bool forward, wp_pos *out_pos)
{
    return wp_document_find_text(doc, from, needle, needle_len, forward,
                                  WP_FALSE, WP_FALSE, out_pos);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void wp_vimode_init(wp_vimode_state *vs, wp_document *doc, wp_undo_journal *journal)
{
    vs->doc = doc;
    vs->journal = journal;
    vs->mode = (u8)WP_VIMODE_NORMAL;
    vs->cursor = wp_pos_doc_start(doc);
    vs->visual.anchor = vs->cursor;
    vs->visual.caret = vs->cursor;
    vs->count_pending = 0;
    vs->op_count_pending = 0;
    vs->pending_operator = (u8)WP_VIOP_NONE;
    vs->pending_prefix = 0;
    vs->register_bytes = NULL;
    vs->register_len = 0;
    vs->register_cap = 0;
    vs->register_linewise = WP_FALSE;
    vs->cmdline_len = 0;
    vs->cmdline_prefix = 0;
    vs->last_search_len = 0;
    vs->last_search_forward = WP_TRUE;
    vs->subst_old_len = 0;
    vs->subst_new_len = 0;
    vs->subst_global = WP_FALSE;
    vs->subst_ignore_case = WP_FALSE;
    vs->subst_paragraph = 0;
    vs->subst_last_paragraph = 0;
    vs->subst_scan_offset = 0;
    vs->subst_match = vs->cursor;
    vs->last_error = WP_OK;
    vs->insert_group_open = WP_FALSE;
}

void wp_vimode_free(wp_vimode_state *vs)
{
    mem_free(vs->register_bytes);
    vs->register_bytes = NULL;
    vs->register_cap = 0;
    vs->register_len = 0;
}

wp_vimode_action wp_vimode_feed_key(wp_vimode_state *vs, wp_key_event key)
{
    switch ((wp_vimode_mode)vs->mode) {
    case WP_VIMODE_NORMAL:
        return feed_normal(vs, key);
    case WP_VIMODE_INSERT:
        return feed_insert(vs, key);
    case WP_VIMODE_VISUAL:
    case WP_VIMODE_VISUAL_LINE:
        return feed_visual(vs, key);
    case WP_VIMODE_COMMAND:
        return feed_command(vs, key);
    case WP_VIMODE_SUBSTITUTE_CONFIRM:
        return feed_substitute_confirm(vs, key);
    default: {
        wp_vimode_action a;
        a.kind = (u8)WP_VIACTION_NONE;
        a.arg_len = 0;
        return a;
    }
    }
}

wp_vimode_mode wp_vimode_get_mode(const wp_vimode_state *vs)
{
    return (wp_vimode_mode)vs->mode;
}

void wp_vimode_get_status_text(const wp_vimode_state *vs, char *out, u32 out_cap)
{
    const char *mode_name;
    u32 len;

    if (out_cap == 0) return;

    switch ((wp_vimode_mode)vs->mode) {
    case WP_VIMODE_NORMAL: mode_name = "NORMAL"; break;
    case WP_VIMODE_INSERT: mode_name = "INSERT"; break;
    case WP_VIMODE_VISUAL: mode_name = "VISUAL"; break;
    case WP_VIMODE_VISUAL_LINE: mode_name = "VISUAL LINE"; break;
    case WP_VIMODE_COMMAND:
        mode_name = (vs->cmdline_prefix == '/' || vs->cmdline_prefix == '?') ? "SEARCH" : "COMMAND";
        break;
    case WP_VIMODE_SUBSTITUTE_CONFIRM: mode_name = "SUBSTITUTE (y/n/a/q)"; break;
    default: mode_name = ""; break;
    }

    len = 0;
    while (mode_name[len] != '\0' && len < out_cap - 1) {
        out[len] = mode_name[len];
        len++;
    }

    if (vs->mode == (u8)WP_VIMODE_COMMAND) {
        u32 i;
        if (len < out_cap - 1) out[len++] = ' ';
        if (len < out_cap - 1) out[len++] = (char)vs->cmdline_prefix;
        for (i = 0; i < vs->cmdline_len && len < out_cap - 1; i++) out[len++] = (char)vs->cmdline_buf[i];
    }

    out[len] = '\0';
}
