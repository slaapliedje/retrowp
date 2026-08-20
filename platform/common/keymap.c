#include <string.h>
#include "keymap.h"
#include "app_ctx.h"
#include "actions.h"

/* M10: this file's per-arm switches grow by one case each, one
   milestone phase at a time (docs/keymaps.md), the same incremental
   shape wp_keymap_state/wp_keymap_id already commit to in keymap.h. */

void wp_keymap_init(wp_app_ctx *ctx, wp_keymap_id keymap_id,
                     wp_document *doc, wp_undo_journal *journal)
{
    ctx->keymap_id = (u8)keymap_id;

    switch (keymap_id) {
    case WP_KEYMAP_WORDSTAR:
        wp_wordstar_init(&ctx->km.ws, doc, journal);
        break;
    case WP_KEYMAP_EMACS:
        wp_emacs_init(&ctx->km.em, doc, journal);
        break;
    case WP_KEYMAP_WORDPERFECT:
        wp_wordperfect_init(&ctx->km.wp5, doc, journal);
        break;
    case WP_KEYMAP_MSWORD:
        wp_msword_init(&ctx->km.mw, doc, journal);
        break;
    case WP_KEYMAP_VI:
    default: /* an unrecognized id (should not happen -- wp_config_load
                already clamps to a real value) falls back to vi-mode
                rather than leaving the union uninitialized */
        ctx->keymap_id = (u8)WP_KEYMAP_VI;
        wp_vimode_init(&ctx->km.vi, doc, journal);
        break;
    }
}

void wp_keymap_free(wp_app_ctx *ctx)
{
    switch ((wp_keymap_id)ctx->keymap_id) {
    case WP_KEYMAP_WORDSTAR:
        /* Owns no heap memory (no register/kill-ring) -- see
           wordstar.h's own comment on why no wp_wordstar_free exists. */
        break;
    case WP_KEYMAP_EMACS:
        wp_emacs_free(&ctx->km.em);
        break;
    case WP_KEYMAP_WORDPERFECT:
        wp_wordperfect_free(&ctx->km.wp5);
        break;
    case WP_KEYMAP_MSWORD:
        /* Owns no heap memory -- see msword.h's own comment on why no
           wp_msword_free exists. */
        break;
    case WP_KEYMAP_VI:
    default:
        wp_vimode_free(&ctx->km.vi);
        break;
    }
}

/* Exactly reproduces wp_dispatch_key's pre-M10 vi-mode dispatch (the
   ':w'/':q'/':wq'/beep action switch, then -- ONLY when vi-mode isn't in
   Command-line mode -- the wp_action_find_by_shortcut fallback), just
   moved here so every backend calls one function instead of duplicating
   this same two-step logic. Behavior is intentionally unchanged: this is
   Phase 0's regression-safety proof (see docs/keymaps.md and the M10
   plan). */
static wp_keymap_outcome feed_key_vi(wp_app_ctx *ctx, wp_key_event ev, u8 raw_shortcut_byte)
{
    wp_vimode_mode mode_before;
    wp_vimode_action action;
    const wp_action *act;
    wp_keymap_outcome outcome;

    outcome.kind = (u8)WP_KEYMAP_RESULT_NONE;
    outcome.action_id = 0;
    outcome.path_len = 0;
    outcome.inserted_char = WP_FALSE;

    mode_before = wp_vimode_get_mode(&ctx->km.vi);
    action = wp_vimode_feed_key(&ctx->km.vi, ev);

    /* Matches wp_dispatch_key's own exact condition for "this keystroke
       really was an Insert-mode character insertion" (mirrors
       feed_insert's own condition in vimode.c) -- see wp_keymap_outcome's
       own comment on inserted_char. */
    if (mode_before == WP_VIMODE_INSERT &&
        ev.special == (u16)WP_KEY_NONE && ev.mods == 0 && ev.codepoint >= 0x20) {
        outcome.inserted_char = WP_TRUE;
    }

    /* T172: carry an ex command's filename argument straight through.
       Copied unconditionally before the switch so every file-touching
       outcome below gets it without repeating this; actions that cannot
       take a name (QUIT, BEEP) simply never have a nonzero arg_len,
       because vimode.c rejects ":q foo" outright rather than dropping
       the tail. */
    if (action.arg_len > 0 && action.arg_len <= (u32)WP_VIMODE_ACTION_ARG_MAX) {
        u32 i;
        for (i = 0; i < action.arg_len; i++) outcome.path[i] = action.arg[i];
        outcome.path_len = (u8)action.arg_len;
    }

    switch ((wp_vimode_action_kind)action.kind) {
    case WP_VIACTION_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_QUIT;
        return outcome; /* the only two cases wp_dispatch_key's own pre-M10
                            switch ever returned early for */
    case WP_VIACTION_SAVE_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_SAVE_QUIT;
        return outcome;
    case WP_VIACTION_OPEN:
        /* Same shape as SAVE below, including the deliberate `break`
           rather than a return. Bare ":e" arrives here with path_len 0,
           which every backend already knows how to handle -- it is
           exactly what a File > Open menu pick looks like. */
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_OPEN;
        break;
    case WP_VIACTION_SAVE:
        /* Original code called do_file_save() directly here, THEN still
           fell through to the shortcut-fallback check below (a plain
           `break`, not a return) -- preserved exactly: set the action
           outcome but keep going, so a coincidental shortcut match on
           this same keystroke can still overwrite it below, exactly as
           before (in practice this never fires, since the ex-command's
           own completing Enter keystroke's raw byte never matches any
           registered shortcut_ascii -- see platform/common/actions.c's
           g_wp_actions[] table -- but this keeps the control flow
           provably identical rather than relying on that observation). */
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_SAVE;
        break;
    case WP_VIACTION_BEEP:
        outcome.kind = (u8)WP_KEYMAP_RESULT_BEEP;
        break;
    default:
        break;
    }

    if (wp_vimode_get_mode(&ctx->km.vi) != WP_VIMODE_COMMAND) {
        act = wp_action_find_by_shortcut(raw_shortcut_byte);
        if (act != NULL) {
            outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
            outcome.action_id = (u8)act->id;
        }
    }

    return outcome;
}

/* WordStar has no free pass into wp_action_find_by_shortcut the way
   vi-mode does (see wp_keymap_feed_key's own header comment in
   keymap.h) -- raw_shortcut_byte is unused here, deliberately, not an
   oversight. Every WP_ACT_* this scheme wants to expose is produced
   directly by its own wp_wordstar_action_kind result below. */
static wp_keymap_outcome feed_key_ws(wp_app_ctx *ctx, wp_key_event ev)
{
    wp_wordstar_action_kind action;
    wp_keymap_outcome outcome;
    wp_bool will_insert;

    /* Matches wp_wordstar_feed_key's own exact condition for "this
       keystroke will insert a plain character" -- computed BEFORE the
       call (unlike vi-mode, WordStar has no separate mode to inspect
       afterward; see wp_keymap_outcome's own comment on inserted_char).
       M11: a keystroke typed into the Find/Replace prompt (prompt_stage
       != NONE) edits that prompt's own text buffer, never the document --
       must not be reported as a document character insertion. */
    will_insert = (wp_bool)(ctx->km.ws.prompt_stage == (u8)WP_WS_PROMPT_NONE &&
                             ctx->km.ws.pending_prefix == 0 &&
                             ev.special == (u16)WP_KEY_NONE &&
                             ev.mods == 0 && ev.codepoint >= 0x20);

    action = wp_wordstar_feed_key(&ctx->km.ws, ev);

    outcome.kind = (u8)WP_KEYMAP_RESULT_NONE;
    outcome.action_id = 0;
    outcome.path_len = 0;
    outcome.inserted_char = will_insert;

    switch (action) {
    case WP_WSACTION_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_QUIT;
        break;
    case WP_WSACTION_SAVE_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_SAVE_QUIT;
        break;
    case WP_WSACTION_SAVE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_SAVE;
        break;
    case WP_WSACTION_FORMAT_BOLD:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_BOLD;
        break;
    case WP_WSACTION_FORMAT_ITALIC:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_ITALIC;
        break;
    case WP_WSACTION_FORMAT_UNDERLINE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_UNDERLINE;
        break;
    case WP_WSACTION_BEEP:
        outcome.kind = (u8)WP_KEYMAP_RESULT_BEEP;
        break;
    case WP_WSACTION_NONE:
    default:
        break;
    }

    return outcome;
}

/* Same "no free pass" reasoning as WordStar's own feed_key_ws (see its
   comment) -- raw_shortcut_byte is unused here too, deliberately: this
   scheme is fully responsible for every wp_action_id it wants to
   expose (C-x C-s/C-f/C-c, C-c b/i/u), and real Emacs itself defines
   meanings for much of the Ctrl-key space this app's own shortcut
   table also uses (C-b/C-p/C-n/C-a/C-e/... are all real Emacs motion
   commands, not free for an app shortcut to claim). */
static wp_keymap_outcome feed_key_em(wp_app_ctx *ctx, wp_key_event ev)
{
    wp_emacs_action_kind action;
    wp_keymap_outcome outcome;
    wp_bool will_insert;

    /* Matches wp_emacs_feed_key's own exact condition for "this
       keystroke will insert a plain character" -- computed BEFORE the
       call, same reasoning feed_key_ws's own comment gives. M11: a
       keystroke typed while isearch_mode is active or a query-replace
       prompt_stage is set edits that prompt's own buffer, never the
       document -- same fix WordStar/WordPerfect's own prompts needed. */
    will_insert = (wp_bool)(ctx->km.em.pending_prefix == 0 && !ctx->km.em.pending_meta &&
                             ctx->km.em.isearch_mode == 0 &&
                             ctx->km.em.prompt_stage == (u8)WP_EMACS_PROMPT_NONE &&
                             ev.special == (u16)WP_KEY_NONE &&
                             ev.mods == 0 && ev.codepoint >= 0x20);

    action = wp_emacs_feed_key(&ctx->km.em, ev);

    outcome.kind = (u8)WP_KEYMAP_RESULT_NONE;
    outcome.action_id = 0;
    outcome.path_len = 0;
    outcome.inserted_char = will_insert;

    switch (action) {
    case WP_EMACSACTION_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_QUIT;
        break;
    case WP_EMACSACTION_SAVE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_SAVE;
        break;
    case WP_EMACSACTION_OPEN:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_OPEN;
        break;
    case WP_EMACSACTION_FORMAT_BOLD:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_BOLD;
        break;
    case WP_EMACSACTION_FORMAT_ITALIC:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_ITALIC;
        break;
    case WP_EMACSACTION_FORMAT_UNDERLINE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_UNDERLINE;
        break;
    case WP_EMACSACTION_BEEP:
        outcome.kind = (u8)WP_KEYMAP_RESULT_BEEP;
        break;
    case WP_EMACSACTION_NONE:
    default:
        break;
    }

    return outcome;
}

/* Same "no free pass" reasoning as WordStar/Emacs's own feed_key_ws/
   feed_key_em (see either's own comment) -- raw_shortcut_byte is
   unused here too: this scheme is fully responsible for every
   wp_action_id it wants to expose (F10/F7/F6/F8), and its own bare
   printable-key dispatch already claims the entire non-modified
   keyboard for typing, the same way every other non-vi scheme does. */
static wp_keymap_outcome feed_key_wp5(wp_app_ctx *ctx, wp_key_event ev)
{
    wp_wordperfect_action_kind action;
    wp_keymap_outcome outcome;
    wp_bool will_insert;

    /* Matches wp_wordperfect_feed_key's own exact condition for "this
       keystroke will insert a plain character" -- computed BEFORE the
       call, same reasoning feed_key_ws/feed_key_em's own comments give.
       M11: a keystroke typed into the Search/Replace prompt
       (prompt_stage != NONE) edits that prompt's own text field, never
       the document -- same fix feed_key_ws needed for WordStar's own
       prompt (see its comment). No pending_prefix check needed here
       (unlike WordStar/Emacs): this scheme has no prefix-key mechanism
       at all -- see wp_wordperfect_feed_key's own header comment. */
    will_insert = (wp_bool)(ctx->km.wp5.prompt_stage == (u8)WP_WP_PROMPT_NONE &&
                             ev.special == (u16)WP_KEY_NONE &&
                             ev.mods == 0 && ev.codepoint >= 0x20);

    action = wp_wordperfect_feed_key(&ctx->km.wp5, ev);

    outcome.kind = (u8)WP_KEYMAP_RESULT_NONE;
    outcome.action_id = 0;
    outcome.path_len = 0;
    outcome.inserted_char = will_insert;

    switch (action) {
    case WP_WPACTION_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_QUIT;
        break;
    case WP_WPACTION_SAVE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_SAVE;
        break;
    case WP_WPACTION_FORMAT_BOLD:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_BOLD;
        break;
    case WP_WPACTION_FORMAT_UNDERLINE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_UNDERLINE;
        break;
    case WP_WPACTION_BEEP:
        outcome.kind = (u8)WP_KEYMAP_RESULT_BEEP;
        break;
    case WP_WPACTION_NONE:
    default:
        break;
    }

    return outcome;
}

/* Same "no free pass" reasoning as every other non-vi scheme's own
   feed_key_* (see e.g. feed_key_ws's own comment) -- raw_shortcut_byte
   is unused here too: this scheme is fully responsible for every
   wp_action_id it wants to expose (Ctrl-B/I/U/S/O/C/X/V), and its own
   bare printable-key dispatch already claims the entire non-modified
   keyboard for typing. */
static wp_keymap_outcome feed_key_mw(wp_app_ctx *ctx, wp_key_event ev)
{
    wp_msword_action_kind action;
    wp_keymap_outcome outcome;
    wp_bool will_insert;

    /* Matches wp_msword_feed_key's own exact condition for "this
       keystroke will insert a plain character" -- computed BEFORE the
       call, same reasoning every other scheme's own comment gives. No
       prefix/pending-mode state to check here either, like WordPerfect
       -- this scheme has none, beyond M11's own Find/Replace prompt
       (prompt_stage != NONE), the same exclusion every other scheme's
       own prompt needed. */
    will_insert = (wp_bool)(ctx->km.mw.prompt_stage == (u8)WP_MS_PROMPT_NONE &&
                             ev.special == (u16)WP_KEY_NONE &&
                             ev.mods == 0 && ev.codepoint >= 0x20);

    action = wp_msword_feed_key(&ctx->km.mw, ev);

    outcome.kind = (u8)WP_KEYMAP_RESULT_NONE;
    outcome.action_id = 0;
    outcome.path_len = 0;
    outcome.inserted_char = will_insert;

    switch (action) {
    case WP_MSACTION_SAVE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_SAVE;
        break;
    case WP_MSACTION_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_QUIT;
        break;
    case WP_MSACTION_OPEN:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FILE_OPEN;
        break;
    case WP_MSACTION_CUT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_EDIT_CUT;
        break;
    case WP_MSACTION_COPY:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_EDIT_COPY;
        break;
    case WP_MSACTION_PASTE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_EDIT_PASTE;
        break;
    case WP_MSACTION_FORMAT_BOLD:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_BOLD;
        break;
    case WP_MSACTION_FORMAT_ITALIC:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_ITALIC;
        break;
    case WP_MSACTION_FORMAT_UNDERLINE:
        outcome.kind = (u8)WP_KEYMAP_RESULT_ACTION;
        outcome.action_id = (u8)WP_ACT_FORMAT_UNDERLINE;
        break;
    case WP_MSACTION_BEEP:
        outcome.kind = (u8)WP_KEYMAP_RESULT_BEEP;
        break;
    case WP_MSACTION_NONE:
    default:
        break;
    }

    return outcome;
}

wp_keymap_outcome wp_keymap_feed_key(wp_app_ctx *ctx, wp_key_event ev, u8 raw_shortcut_byte)
{
    switch ((wp_keymap_id)ctx->keymap_id) {
    case WP_KEYMAP_WORDSTAR:
        return feed_key_ws(ctx, ev);
    case WP_KEYMAP_EMACS:
        return feed_key_em(ctx, ev);
    case WP_KEYMAP_WORDPERFECT:
        return feed_key_wp5(ctx, ev);
    case WP_KEYMAP_MSWORD:
        return feed_key_mw(ctx, ev);
    case WP_KEYMAP_VI:
    default:
        return feed_key_vi(ctx, ev, raw_shortcut_byte);
    }
}

wp_pos *wp_keymap_cursor(wp_app_ctx *ctx)
{
    switch ((wp_keymap_id)ctx->keymap_id) {
    case WP_KEYMAP_WORDSTAR:
        return &ctx->km.ws.cursor;
    case WP_KEYMAP_EMACS:
        return &ctx->km.em.cursor;
    case WP_KEYMAP_WORDPERFECT:
        return &ctx->km.wp5.cursor;
    case WP_KEYMAP_MSWORD:
        return &ctx->km.mw.cursor;
    case WP_KEYMAP_VI:
    default:
        return &ctx->km.vi.cursor;
    }
}

void wp_keymap_get_status_view(const wp_app_ctx *ctx, char *buf, u32 buf_cap,
                                wp_vi_status_view *out)
{
    switch ((wp_keymap_id)ctx->keymap_id) {
    case WP_KEYMAP_WORDSTAR:
        /* M11: real, dynamic prompt text once a Find/Replace prompt is
           active (FIND?/REPLACE WITH?/OPTIONS?/REPLACE (Y/N)?); the
           fixed "WORDSTAR" label otherwise -- WordStar has no other
           notion of "mode" the way vi does. */
        wp_wordstar_get_status_text(&ctx->km.ws, buf, buf_cap);
        break;
    case WP_KEYMAP_EMACS:
        /* M11: real, dynamic prompt text once isearch or query-replace
           is active ("I-search:"/"I-search backward:"/"Query
           replace:"/"Query replace with:"/"Query replacing
           (y/n/!/.)?"); the fixed "EMACS" label otherwise, same
           reasoning WordStar's own status text gives. */
        wp_emacs_get_status_text(&ctx->km.em, buf, buf_cap);
        break;
    case WP_KEYMAP_WORDPERFECT:
        /* M11: real, dynamic prompt text once a Search/Replace prompt
           is active ("-> Srch:"/"<- Srch:"/"w/Confirm? (Y/N)"/"Replace
           with:"/"Replace? (Y/N)"); the fixed "WORDPERFECT" label
           otherwise, same reasoning WordStar's own status text gives. */
        wp_wordperfect_get_status_text(&ctx->km.wp5, buf, buf_cap);
        break;
    case WP_KEYMAP_MSWORD:
        /* M11: real, dynamic prompt text once a Find/Replace prompt is
           active ("Find:"/"Replace with:"/"Replace (r/a/n)?"); the
           fixed "WORD" label otherwise, same reasoning WordStar's own
           status text gives. */
        wp_msword_get_status_text(&ctx->km.mw, buf, buf_cap);
        break;
    case WP_KEYMAP_VI:
    default:
        wp_vimode_get_status_text(&ctx->km.vi, buf, buf_cap);
        break;
    }

    /* No scheme has a pending_count/pending_cmd accessor exposed
       separately yet -- see wp_keymap_get_status_view's own header
       comment for why (mirrors paint_status_line's pre-M10 behavior
       exactly, not a new gap). */
    out->mode_label_utf8 = (const u8 *)buf;
    out->mode_label_len = (u32)strlen(buf);
    out->pending_count = 0;
    out->pending_cmd_utf8 = NULL;
    out->pending_cmd_len = 0;
}

/* ------------------------------------------------------------------ */
/* T172: the pending ex-command filename                               */
/* ------------------------------------------------------------------ */

/* Every backend routes both menu picks and keystroke outcomes through
   one invoke_action(action_id) dispatcher, which by design carries no
   payload -- a menu pick has nothing to say beyond "the user chose
   Open". Rather than thread a path parameter through that dispatcher on
   four backends (and past every non-file action that would have to
   ignore it), a keystroke that named a file parks the name here and the
   backend's own do_file_open/do_file_save collects it.

   Lives in platform/common, not four times over, because all four
   backends have the identical seam: each builds a `fullpath` -- from its
   native picker -- and everything downstream of that point (format
   detection by extension, load, save) is already path-driven and needs
   no change at all. Supplying the path is simply skipping the picker.

   Single-slot and consume-once BY DESIGN: it is written immediately
   before invoke_action and cleared the moment it is read, so a name can
   never survive to be picked up by a later, unrelated File > Open from
   the menu. Reading it always clears it, even when the caller then
   decides not to use it. */
static char    g_pending_path[WP_VIMODE_ACTION_ARG_MAX + 1];
static wp_bool g_pending_path_set = WP_FALSE;

void wp_keymap_set_pending_path(const u8 *path, u32 len)
{
    u32 i;

    g_pending_path_set = WP_FALSE;
    if (path == NULL || len == 0 || len > (u32)WP_VIMODE_ACTION_ARG_MAX) return;

    for (i = 0; i < len; i++) g_pending_path[i] = (char)path[i];
    g_pending_path[len] = '\0';
    g_pending_path_set = WP_TRUE;
}

wp_bool wp_keymap_take_pending_path(char *out, u32 out_cap)
{
    u32 i;

    if (!g_pending_path_set) return WP_FALSE;
    g_pending_path_set = WP_FALSE;   /* consumed, whatever happens below */

    if (out == NULL || out_cap == 0) return WP_FALSE;
    for (i = 0; g_pending_path[i] != '\0' && i + 1 < out_cap; i++) out[i] = g_pending_path[i];
    out[i] = '\0';
    /* A path that would not fit the caller's buffer is refused outright
       rather than handed back truncated -- a truncated path names a
       different file, and on a save that is silent data loss. */
    return (wp_bool)(g_pending_path[i] == '\0');
}

wp_bool wp_keymap_claims_f1(wp_keymap_id id)
{
    /* WordPerfect and nothing else -- see keymap.h. Word binds Shift-F4
       and no plain F-key, so it does not compete for F1; vi and Emacs
       bind no function keys at all. */
    return (id == WP_KEYMAP_WORDPERFECT) ? WP_TRUE : WP_FALSE;
}
