#ifndef WP_APP_CTX_H
#define WP_APP_CTX_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keymap.h"
#include "platform.h"

/* Shared application state threaded through the action registry
   (actions.h). This is real, in-scope refactor work, not incidental:
   before M8.5, every Atari handler (toggle_style_flag, change_font_size,
   apply_alignment, do_file_open, ...) closed over file-scope statics in
   app_shell.c -- a shared registry needs SOME argument to act on
   instead. May be built directly by a backend's own app_shell-
   equivalent, or by a host test exercising actions.c without any real
   window/gc (both window/gc/font may be NULL in that case -- portable
   action handlers that don't need drawing, like the Format toggles,
   never touch them).

   wp_keymap_state is embedded by value (not a pointer) -- see
   keymap.h's own comment for why (this codebase's transparent-struct
   convention; a tagged union, not an opaque handle, since the active
   scheme is fixed at startup and never switches live). Whichever arm
   is active has its own .doc/.journal wired to point at *doc / *journal
   below via wp_app_ctx_init/wp_keymap_init, so wp_keymap_cursor(ctx) is
   always the single source of truth for "where editing currently
   applies," matching how app_shell.c's pre-M8.5 g_cursor already worked
   (this app has no separate mouse-driven text cursor concept). */
typedef struct wp_app_ctx {
    wp_document     *doc;      /* not owned */
    wp_undo_journal *journal;  /* not owned */
    u8               keymap_id; /* wp_keymap_id; fixed for the process lifetime */
    wp_keymap_state  km;

    plat_window *window; /* may be NULL (e.g. a backend, like Atari today,
                             that owns its native window handle directly
                             rather than through plat_window_create --
                             see docs/atari-platform.md) */
    plat_gc     *gc;     /* may be NULL outside a paint callback */
    plat_font   *font;

    i32 current_size_twips; /* the Format menu's "current input style" state --
                                see docs/atari-platform.md's Format controls section;
                                unchanged in spirit from pre-M8.5 g_current_size_twips */
    u8  align;
    u16 style_flags;    /* wp_style_flags bitfield */

    wp_bool doc_loaded;
    wp_bool quit_requested; /* WP_ACT_FILE_QUIT sets this; the backend's own event
                                loop checks it and performs its own native teardown
                                (menu_bar(REMOVE), wind_close, appl_exit, etc.) --
                                keeps FILE_QUIT itself fully portable */
} wp_app_ctx;

/* Selects keymap_id (see keymap.h -- typically read from config.h's
   wp_config just before this call) and wires the active scheme's own
   .doc/.journal to point at *doc / *journal, then initializes everything
   else to sane defaults (doc_loaded/quit_requested false, style_flags 0,
   current_size_twips/align left for the caller to seed from the
   document's own default style once one is loaded -- mirrors how
   replace_document() seeds g_current_style_flags/g_current_size_twips
   today, see docs/atari-platform.md). window/gc/font are left NULL for
   the caller to fill in. */
void wp_app_ctx_init(wp_app_ctx *ctx, wp_keymap_id keymap_id,
                      wp_document *doc, wp_undo_journal *journal);

#endif /* WP_APP_CTX_H */
