/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_APP_CTX_H
#define WP_APP_CTX_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keymap.h"
#include "viewport.h"
#include "prompt.h"
#include "platform.h"

/* Room for a native path in wp_app_ctx. Longer than any target needs
   (8.3 names on the ST and DOS; AmigaDOS and the Mac stay well under)
   and still small against test_keymap.c's sizeof(wp_app_ctx) ceiling. */
#define WP_APP_PATH_MAX 256

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
    wp_font current_font; /* the input style's face; 0 = the target default */

    /* The input style above is what newly typed text gets only while it
       is ARMED: a Bold, Size or Font chosen with nothing selected arms it
       at the caret, typing there keeps it armed, and moving the caret
       anywhere disarms it -- typed text then takes its neighbour's style,
       the engine's own rule, and the input style is re-read from the text
       at the caret (wp_action_after_key). Without that, typing into a
       paragraph of Times from a Word file came out in the default face. */
    wp_bool input_armed;
    wp_pos  input_at;

    wp_viewport view;   /* the scrolled view: see viewport.h. The backend sets
                           view.line_px after wp_app_ctx_init and records
                           view.view_h_px in its painter; everything else is
                           shared code. */

    char doc_path[WP_APP_PATH_MAX]; /* the file the document came from or was
                                       last saved to, as a native path; "" for
                                       a document that has no file yet (New,
                                       the startup sample). Save writes here
                                       without asking; Save As and Open
                                       replace it. Kept by actions.c's file
                                       flow, read by wp_action_document_name. */

    /* The backend's lazy-once offload bring-up (each has its own endpoint
       string and its own ensure_offload()), for the shared code that
       needs the link on demand: the file flow's ODT/PDF conversion.
       NULL means the link is whatever offload_available() says. */
    wp_bool (*ensure_offload)(void);

    /* The machine's own printer, as a file to write -- the ST's "PRN:",
       the Amiga's "PRT:", DOS's "PRN" -- and the code page it reads
       (codepage.h; 0 is plain ASCII). NULL when there is none to print
       on without a gateway. File > Print (wp_action_file_print). */
    const char *printer_path;
    u16         printer_codepage;
    /* Whether that printer will take bytes now: on, selected, paper in.
       Asked before the printout is opened, because a port with no
       printer on it does not fail -- TOS waits on every character, DOS
       stops for "Not ready writing device PRN" -- and the program would
       freeze. NULL when the shell cannot ask, and the printer is tried. */
    wp_bool   (*printer_ready)(void);
    /* What that printer understands (wp_prn_driver) and whether to print
       on both sides (wp_prn_sides). When printer_choosable, File > Print
       lets the writer change them and saves the choice to config_path
       (wp_config_save_printer) -- the ST and DOS, where a word processor
       carried its own printer drivers. Not on the Amiga, whose Printer
       preferences choose the printer and RetroWP sends printer.device's
       own codes. */
    u8          printer_driver;
    u8          printer_sides;
    wp_bool     printer_choosable;
    const char *config_path;

    wp_prompt prompt;   /* the status line's one-line text prompt (prompt.h) */

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
