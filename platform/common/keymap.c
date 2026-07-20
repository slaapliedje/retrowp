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

    switch ((wp_vimode_action_kind)action.kind) {
    case WP_VIACTION_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_QUIT;
        return outcome; /* the only two cases wp_dispatch_key's own pre-M10
                            switch ever returned early for */
    case WP_VIACTION_SAVE_QUIT:
        outcome.kind = (u8)WP_KEYMAP_RESULT_SAVE_QUIT;
        return outcome;
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
       afterward; see wp_keymap_outcome's own comment on inserted_char). */
    will_insert = (wp_bool)(ctx->km.ws.pending_prefix == 0 &&
                             ev.special == (u16)WP_KEY_NONE &&
                             ev.mods == 0 && ev.codepoint >= 0x20);

    action = wp_wordstar_feed_key(&ctx->km.ws, ev);

    outcome.kind = (u8)WP_KEYMAP_RESULT_NONE;
    outcome.action_id = 0;
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

wp_keymap_outcome wp_keymap_feed_key(wp_app_ctx *ctx, wp_key_event ev, u8 raw_shortcut_byte)
{
    switch ((wp_keymap_id)ctx->keymap_id) {
    case WP_KEYMAP_WORDSTAR:
        return feed_key_ws(ctx, ev);
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
        /* WordStar has no real notion of a "mode" the way vi does (it's
           non-modal for typing) -- a fixed label is honest, not a
           simplification of something that would otherwise vary. */
        if (buf_cap > 0) {
            const char *label = "WORDSTAR";
            u32 n = 0;
            while (label[n] != '\0' && n + 1 < buf_cap) { buf[n] = label[n]; n++; }
            buf[n] = '\0';
        }
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
