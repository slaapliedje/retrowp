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

static void clamp_cursor(wp_vimode_state *vs)
{
    const wp_section *sec;

    if (vs->doc->section_count == 0) { vs->cursor = wp_pos_doc_start(vs->doc); return; }
    if (vs->cursor.section >= vs->doc->section_count) { vs->cursor = wp_pos_doc_end(vs->doc); return; }
    sec = &vs->doc->sections[vs->cursor.section];
    if (sec->para_count == 0 || vs->cursor.paragraph >= sec->para_count) {
        vs->cursor = wp_pos_doc_end(vs->doc);
    }
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
        case 'n':
            if (vs->last_search_len > 0) {
                wp_pos found;
                if (wp_vimode_search(vs->doc, vs->cursor, vs->last_search, vs->last_search_len,
                                      WP_TRUE, &found) == WP_OK) {
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

static wp_vimode_action execute_ex_command(wp_vimode_state *vs)
{
    wp_vimode_action action;

    action.kind = (u8)WP_VIACTION_BEEP;
    action.arg_len = 0;

    if (vs->cmdline_len == 1 && vs->cmdline_buf[0] == 'w') action.kind = (u8)WP_VIACTION_SAVE;
    else if (vs->cmdline_len == 1 && vs->cmdline_buf[0] == 'q') action.kind = (u8)WP_VIACTION_QUIT;
    else if (vs->cmdline_len == 1 && vs->cmdline_buf[0] == 'x') action.kind = (u8)WP_VIACTION_SAVE_QUIT;
    else if (vs->cmdline_len == 2 && vs->cmdline_buf[0] == 'w' && vs->cmdline_buf[1] == 'q') {
        action.kind = (u8)WP_VIACTION_SAVE_QUIT;
    }

    return action;
}

static void execute_search(wp_vimode_state *vs)
{
    wp_pos found;

    if (vs->cmdline_len == 0 || vs->cmdline_len > WP_VIMODE_SEARCH_MAX) return;

    memcpy(vs->last_search, vs->cmdline_buf, vs->cmdline_len);
    vs->last_search_len = vs->cmdline_len;
    vs->last_search_forward = WP_TRUE;

    if (wp_vimode_search(vs->doc, vs->cursor, vs->cmdline_buf, vs->cmdline_len, WP_TRUE, &found) == WP_OK) {
        vs->cursor = found;
        vs->last_error = WP_OK;
    } else {
        vs->last_error = WP_ERR;
    }
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
        if (vs->cmdline_prefix == ':') action = execute_ex_command(vs);
        else if (vs->cmdline_prefix == '/') execute_search(vs);
        vs->mode = (u8)WP_VIMODE_NORMAL;
        vs->cmdline_len = 0;
        vs->cmdline_prefix = 0;
        return action;
    }
    if (key.special == (u16)WP_KEY_NONE && key.codepoint >= 0x20 && key.codepoint < 0x7F) {
        if (vs->cmdline_len < WP_VIMODE_CMDLINE_MAX - 1) {
            vs->cmdline_buf[vs->cmdline_len++] = (u8)key.codepoint;
        }
    }
    return action;
}

/* ------------------------------------------------------------------ */
/* Search                                                                */
/* ------------------------------------------------------------------ */

static wp_pos pos_from_byte_in_paragraph(u32 section, u32 paragraph, const wp_paragraph *para, u32 byte_offset)
{
    wp_pos pos;
    u32 consumed = 0;
    u32 r;

    pos.section = section;
    pos.paragraph = paragraph;

    if (para->run_count == 0) {
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }

    for (r = 0; r < para->run_count; r++) {
        u32 len = para->runs[r].text.length;

        if (byte_offset < consumed + len) {
            pos.run = r;
            pos.offset = byte_offset - consumed;
            return pos;
        }
        if (byte_offset == consumed + len) {
            if ((u32)(r + 1) < para->run_count) {
                pos.run = r + 1;
                pos.offset = 0;
            } else {
                pos.run = r;
                pos.offset = len;
            }
            return pos;
        }
        consumed += len;
    }

    paragraph_end_run_offset(para, &pos.run, &pos.offset);
    return pos;
}

wp_status wp_vimode_search(const wp_document *doc, wp_pos from,
                            const u8 *needle, u32 needle_len,
                            wp_bool forward, wp_pos *out_pos)
{
    u32 si = from.section, pi = from.paragraph;
    wp_bool first = WP_TRUE;

    WP_UNUSED(forward); /* forward-only this pass -- see docs/vimode.md */

    if (needle_len == 0 || si >= doc->section_count) return WP_ERR;

    for (;;) {
        const wp_section *sec = &doc->sections[si];

        if (pi < sec->para_count) {
            const wp_paragraph *para = &sec->paras[pi];
            u8 scratch[512];
            u32 total_len, len, min_start;

            {
                wp_selection sel;
                sel.anchor.section = si; sel.anchor.paragraph = pi; sel.anchor.run = 0; sel.anchor.offset = 0;
                sel.caret.section = si; sel.caret.paragraph = pi;
                paragraph_end_run_offset(para, &sel.caret.run, &sel.caret.offset);
                wp_document_extract_text(doc, sel, scratch, (u32)sizeof(scratch), &total_len);
            }
            len = (total_len < (u32)sizeof(scratch)) ? total_len : (u32)sizeof(scratch);

            min_start = 0;
            if (first && si == from.section && pi == from.paragraph) {
                u32 rr;
                for (rr = 0; rr < from.run; rr++) min_start += para->runs[rr].text.length;
                min_start += from.offset + 1;
            }

            if (len >= needle_len && min_start <= len - needle_len) {
                u32 s;
                for (s = min_start; s <= len - needle_len; s++) {
                    if (memcmp(scratch + s, needle, needle_len) == 0) {
                        *out_pos = pos_from_byte_in_paragraph(si, pi, para, s);
                        return WP_OK;
                    }
                }
            }
        }
        first = WP_FALSE;

        if (pi + 1 < sec->para_count) {
            pi++;
        } else if (si + 1 < doc->section_count) {
            si++;
            pi = 0;
        } else {
            si = 0;
            pi = 0;
        }

        if (si == from.section && pi == from.paragraph) break;
    }

    return WP_ERR;
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
    case WP_VIMODE_COMMAND: mode_name = (vs->cmdline_prefix == '/') ? "SEARCH" : "COMMAND"; break;
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
