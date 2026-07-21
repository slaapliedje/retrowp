#ifndef WP_KEYMAP_H
#define WP_KEYMAP_H

#include "types.h"
#include "docmodel.h"
#include "keyevent.h"
#include "vimode.h"
#include "wordstar.h"
#include "emacs.h"
#include "wordperfect.h"
#include "statusline.h"

/* M10: switchable keybinding layouts (WordStar/WordPerfect/Emacs/MS Word
   alongside vi-mode), selected once at startup from config.h/config.c and
   fixed for the process lifetime -- no live switching. See docs/keymaps.md.

   Build discipline: this file (and keymap.c) may call platform.h
   functions and any engine header, but must NEVER #include a native
   header -- same platform/common/ rule as actions.h/help_doc.h.

   struct wp_app_ctx is only forward-declared here: every function below
   takes just a pointer to it, which needs no full definition in a
   signature. app_ctx.h is the one that #includes this file (not the
   reverse) since IT needs wp_keymap_state's full definition to embed one
   by value -- the reverse direction would be circular. */
struct wp_app_ctx;

typedef enum {
    WP_KEYMAP_VI = 0,
    WP_KEYMAP_WORDSTAR,
    WP_KEYMAP_EMACS,
    WP_KEYMAP_WORDPERFECT,
    WP_KEYMAP_COUNT
    /* MS Word's own arm is added last, in its own milestone phase
       (docs/keymaps.md) -- see wp_keymap_state's own comment for why
       this and the union grow together, one arm per phase. */
} wp_keymap_id;

typedef union {
    wp_vimode_state vi;
    wp_wordstar_state ws;
    wp_emacs_state em;
    wp_wordperfect_state wp5;
    /* MS Word's own arm is added here once its own phase lands -- see
       wp_keymap_id's own comment. Every
       arm must keep its OWN cursor/doc/journal fields (mirroring
       wp_vimode_state's shape exactly, not hoisted out to wp_app_ctx),
       so each stays independently host-testable with no wp_app_ctx in
       the picture at all -- the same way test_vimode.c already
       constructs a bare wp_vimode_state today. Any variable-size per-arm
       data must be mem_alloc'd (pointer + len/cap, NULL when empty --
       wp_vimode_state.register_bytes's existing pattern), never an
       embedded fixed array, so this union's size stays dominated by
       whichever single arm is largest rather than growing with the sum
       of all arms -- enforced by test_keymap.c's sizeof(wp_app_ctx)
       ceiling test. */
} wp_keymap_state;

typedef enum {
    WP_KEYMAP_RESULT_NONE = 0,  /* consumed internally (motion/edit/pending-prefix),
                                    or genuinely unrecognized -- caller just redraws */
    WP_KEYMAP_RESULT_ACTION,    /* action_id is a platform/common/actions.h wp_action_id --
                                    dispatch it exactly like a menu click (handler-or-backend;
                                    see each backend's own invoke_action helper) */
    WP_KEYMAP_RESULT_QUIT,
    WP_KEYMAP_RESULT_SAVE_QUIT,
    WP_KEYMAP_RESULT_BEEP       /* cosmetic: invalid/incomplete command */
} wp_keymap_result_kind;

typedef struct {
    u8      kind;           /* wp_keymap_result_kind */
    u8      action_id;      /* a platform/common/actions.h wp_action_id; valid only
                                when kind == WP_KEYMAP_RESULT_ACTION. Kept as a plain
                                u8 here rather than actions.h's own enum type, since
                                this header must not depend on actions.h -- the
                                caller (a backend's own app_shell.c) already includes
                                actions.h and casts this back to wp_action_id. */
    wp_bool inserted_char;  /* WP_TRUE iff this call inserted a printable char at the
                                cursor -- lets the caller keep stamping style_flags onto
                                the new run generically (see apply_current_style_to_run_at's
                                own callers), replacing a per-scheme mode-aware check. */
} wp_keymap_outcome;

/* doc/journal are borrowed, same convention wp_vimode_init already uses.
   keymap_id selects which union arm gets initialized and is recorded in
   ctx->keymap_id -- every other arm is left uninitialized (never read,
   since ctx->keymap_id is fixed for the process lifetime once set here,
   until a caller re-inits onto a replacement document, which passes the
   same id back in). */
void wp_keymap_init(struct wp_app_ctx *ctx, wp_keymap_id keymap_id,
                     wp_document *doc, wp_undo_journal *journal);

/* Frees the active arm's own owned memory (e.g. vi-mode's yank register).
   Does not touch doc/journal (borrowed). Call before re-initializing onto
   a replacement document (see each backend's own replace-document path)
   or at shutdown -- mirrors wp_vimode_free's own contract exactly. */
void wp_keymap_free(struct wp_app_ctx *ctx);

/* The entire input surface, generalizing wp_vimode_feed_key to whichever
   scheme is active. raw_shortcut_byte is the backend's own already-
   computed "plain ASCII byte for this keystroke, independent of any
   scheme's own translation" (Atari: (u8)(key & 0xFF) off evnt_multi's
   raw scan+ascii short; Amiga: g_last_raw_ascii off MapRawKey) -- needed
   ONLY by the WP_KEYMAP_VI arm, to preserve its existing "vi-mode didn't
   claim this key, fall back to wp_action_find_by_shortcut" behavior
   exactly (docs/vimode.md's own documented, deliberate scope: vi-mode
   requires an unmodified key for every plain-letter command specifically
   so Ctrl+<letter> app shortcuts stay reachable). Every other scheme
   ignores this parameter -- each is fully responsible for producing
   every wp_action_id it wants to expose itself, since WordStar/Emacs/
   WordPerfect/MS Word all define real historical meanings for much of
   that same Ctrl-key space themselves (see docs/keymaps.md). */
wp_keymap_outcome wp_keymap_feed_key(struct wp_app_ctx *ctx, wp_key_event ev,
                                      u8 raw_shortcut_byte);

/* Returns a pointer into the active arm's own cursor field -- a pointer,
   not a by-value wp_pos, because existing call sites both read AND
   write through it (whole-position replacement on document reload,
   individual .section/.paragraph/.run/.offset field writes during
   editing). */
wp_pos *wp_keymap_cursor(struct wp_app_ctx *ctx);

/* Writes a NUL-terminated mode/status string into buf (truncated to fit
   buf_cap), then fills the rest of *out the same way every existing
   status-line caller already does (pending_count/pending_cmd left at
   0/NULL -- see platform/atari/app_shell.c's paint_status_line, whose
   own comment explains why: no per-mode accessor exists yet that
   wouldn't be more confusing shown alone). Pure data, no drawing --
   mirrors wp_vimode_get_status_text's own contract. */
void wp_keymap_get_status_view(const struct wp_app_ctx *ctx, char *buf, u32 buf_cap,
                                wp_vi_status_view *out);

#endif /* WP_KEYMAP_H */
