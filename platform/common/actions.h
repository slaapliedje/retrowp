/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_ACTIONS_H
#define WP_ACTIONS_H

#include "types.h"
#include "app_ctx.h"
#include "format_rtf.h"

/* Single canonical action registry (M8.5), replacing what was
   previously two independently-maintained switch statements in
   platform/atari/app_shell.c: one for MN_SELECTED menu dispatch, one
   for wp_dispatch_key's keyboard shortcuts, hand-duplicating the same
   action set. A future backend (Amiga/Mac/DOS) builds its own native
   menu by folding over g_wp_actions[] grouped by .group instead of
   inventing a new data structure -- see docs/atari-platform.md's
   "Boot resolution"/menu sections and CLAUDE.md's platform/common/
   layer rule.

   Build discipline: this file (and actions.c) may call platform.h
   functions but must NEVER #include a native header (gem.h,
   exec/types.h, MacTypes.h, dos.h). This is enforced by construction,
   not just convention -- platform/common/ is compiled by EVERY target's
   own toolchain (host cc, the Atari m68k cross-compiler, and eventually
   Amiga/Mac/DOS cross-compilers); a stray native #include breaks the
   host build immediately, since no such header exists there. */

typedef enum {
    /* New comes first because that is where it belongs in a File menu,
       and because backends that fold over this table in order (Amiga,
       Mac) get the on-screen order straight from here. Inserting rather
       than appending renumbers every id below, which is safe: no action
       id is ever written to disk or the wire -- the config file stores
       keymap NAMES, not action numbers -- and every reference in this
       codebase is symbolic. */
    WP_ACT_FILE_NEW = 0,
    WP_ACT_FILE_OPEN,
    WP_ACT_FILE_SAVE,
    WP_ACT_FILE_SAVE_AS,
    WP_ACT_FILE_QUIT,
    WP_ACT_EDIT_CUT,
    WP_ACT_EDIT_COPY,
    WP_ACT_EDIT_PASTE,
    WP_ACT_EDIT_UNDO,
    WP_ACT_FORMAT_BOLD,
    WP_ACT_FORMAT_ITALIC,
    WP_ACT_FORMAT_UNDERLINE,
    WP_ACT_FORMAT_SIZE,
    WP_ACT_FORMAT_ALIGN,
    WP_ACT_TOOLS_SPELLCHECK,
    WP_ACT_TOOLS_THESAURUS,
    WP_ACT_TOOLS_GRAMMARCHECK,
    WP_ACT_TOOLS_CHAT,
    WP_ACT_TOOLS_INSERTIMAGE,
    WP_ACT_TOOLS_DICTATE,
    WP_ACT_HELP_CONTENTS,
    WP_ACT_HELP_KEYS,
    WP_ACT_HELP_FORMAT,
    /* Paragraph formatting, the ruler bar's buttons (rulerbar.h). Group
       NONE: reached by clicking the ruler bar, and by any backend that
       chooses to bind them; every one has a portable handler. They act
       on every paragraph the selection touches, else on the caret's. */
    WP_ACT_PARA_LEFT,
    WP_ACT_PARA_CENTER,
    WP_ACT_PARA_RIGHT,
    WP_ACT_PARA_JUSTIFY,
    WP_ACT_PARA_SINGLE,
    WP_ACT_PARA_ONEHALF,
    WP_ACT_PARA_DOUBLE,
    WP_ACT_PARA_INDENT_LESS,
    WP_ACT_PARA_INDENT_MORE,
    /* Insert a page break at the caret: the paragraph is split there and
       the part after it starts a new page (page_break_before). At the
       start of a paragraph it toggles that paragraph's break instead. */
    WP_ACT_PARA_PAGE_BREAK,
    WP_ACT_COUNT
} wp_action_id;

typedef enum {
    WP_MENU_NONE = 0,
    WP_MENU_FILE,
    WP_MENU_EDIT,
    WP_MENU_FORMAT,
    WP_MENU_TOOLS,
    WP_MENU_HELP
} wp_menu_group;

typedef wp_status (*wp_action_fn)(struct wp_app_ctx *ctx);
/* NULL means "always enabled" -- most actions. Reserved for a future
   backend wanting to grey out a menu item (e.g. Tools when no document
   is loaded); unused by any wp_action entry this pass, so every
   registered .is_enabled is currently NULL -- not wired into any real
   menu-greying logic yet either (Phase 9's Atari retrofit doesn't call
   it). Declared now so the table shape doesn't need to change later. */
typedef wp_bool (*wp_action_enabled_fn)(const struct wp_app_ctx *ctx);

typedef struct {
    wp_action_id id;
    wp_menu_group group;         /* which native menu this belongs under, or NONE
                                     (e.g. an action only ever reached via keyboard) */
    const char *label_utf8;      /* e.g. "Open" -- a backend adds its own
                                     padding/mnemonic/ellipsis conventions */
    const char *key_hint_utf8;   /* default DISPLAYED hint, e.g. "Ctrl+O" -- a
                                     backend MAY show a different string (DOS
                                     wants "F3", not "Ctrl+O", for example);
                                     only shortcut_ascii below is the universal,
                                     behavior-defining part */
    u8 shortcut_ascii;           /* 0 = none. Same byte space as the pre-M8.5
                                     wp_dispatch_key: printable ASCII or a
                                     Ctrl-letter control code (0x01-0x1A) */
    wp_action_fn handler;        /* NULL for a backend-supplied action (see below) */
    wp_action_enabled_fn is_enabled;
} wp_action;

/* The one canonical table, defined in actions.c, linked into every
   backend. Portable actions (Format toggles, Edit Copy, File Quit) have
   a real .handler here, implemented in actions.c. FILE_OPEN/FILE_SAVE
   and the four Tools actions have handler == NULL -- these need a
   native file picker (fsel_input on Atari today) or are, in the current
   codebase, deeply GEM/offload-specific multi-hundred-line functions
   (dialog loops, gateway round-trips) that were not re-architected as
   part of this pass; each backend supplies its own implementation
   directly (app_shell.c's existing do_file_open/do_spell_check/etc.),
   consulted by id via wp_action_find() at the call site rather than
   through this table's .handler field. See docs/atari-platform.md. */
extern const wp_action g_wp_actions[WP_ACT_COUNT];

const wp_action *wp_action_find(wp_action_id id);
const wp_action *wp_action_find_by_shortcut(u8 ascii);

/* T174 Dictate: the whole action, portable, in ONE place rather than
   four near-identical per-backend copies.

   It is not registered as a .handler in the table above (it stays
   handler == NULL, a backend-supplied action) for one reason: the
   offload link's endpoint string is genuinely per-target -- "serial:
   aux:9600" on Atari/Amiga, "serial:modem:9600" on Mac, "serial:0:9600"
   on DOS -- and each backend already owns that in its own lazy
   ensure_offload(). So a backend's invoke_backend_action does exactly
   two things for WP_ACT_TOOLS_DICTATE: bring the link up its own way,
   then call this. Everything after that -- the CAPS pre-check, the
   request, the round trip, both dialogs, the insert at the cursor -- is
   identical on every target and lives here.

   Requires an already-connected link: returns WP_ERR (after telling the
   user) if offload_available() is false, rather than trying to open one
   itself. */
wp_status wp_action_dictate(struct wp_app_ctx *ctx);

/* Maps a WP_ACT_HELP_* action to the help-document node it opens, e.g.
   WP_ACT_HELP_KEYS -> "Normal". Returns NULL (and leaves *len alone) for
   any action that is not a help entry.

   Shared rather than repeated per backend because all four open the same
   docs/help/retrowp.help through the same portable viewer, and a node
   name that exists in only three of the four copies fails as a silently
   empty help screen rather than as a build error. The node names are
   real @node identifiers in that file -- wp_help_doc_find_node falls
   back to node 0 on a miss, so a typo here degrades to "the index page"
   rather than to nothing; host/tests/test_menucheck.c pins them against
   the real parsed document so a rename cannot drift past unnoticed. */
const char *wp_action_help_node(wp_action_id id, u32 *len);

/* Asks the writer before their unsaved work is thrown away. WP_TRUE
   means "go ahead"; a clean document says so without any dialog. Every
   path that replaces or abandons the current document -- New, Open,
   Quit -- must gate on this. See the implementation for why Cancel is
   the default and why a failed dialog answers "no". */
wp_bool wp_action_confirm_discard(struct wp_app_ctx *ctx);

/* Takes over *fresh as the whole document: frees the old one and its
   undo journal, resets the active scheme, seeds the current input style
   from the new document's defaults and marks it saved. The one swap
   every load path on every backend uses -- New, Open, startup. Leaves
   the cursor where wp_keymap_init puts it (document start); a backend
   that wants it elsewhere moves it afterwards. Does not relayout. */
void wp_action_replace_document(struct wp_app_ctx *ctx, wp_document *fresh);

/* For every way out other than File > Quit: a scheme's own quit command
   and a window's close box. Asks the same unsaved-changes question and
   sets ctx->quit_requested only if it is safe to leave -- the backend's
   loop decides on that flag, never on having been asked. */
void wp_action_request_quit(struct wp_app_ctx *ctx);

/* For save-and-quit (vi :wq, WordStar ^KX), called after the backend's
   own Save: quits only if the document is no longer modified, so a
   cancelled or failed save leaves the writer where they were. */
void wp_action_request_quit_after_save(struct wp_app_ctx *ctx);

/* Writes buf[0..len) to native_path, replacing the file. On success,
   and only if marks_saved, the document is marked saved; an export the
   program cannot read back (PDF) passes WP_FALSE. Any failure is told
   to the writer, and the document stays modified. */
wp_status wp_action_write_file(struct wp_app_ctx *ctx, const char *native_path,
                                const u8 *buf, u32 len, wp_bool marks_saved);

/* A one-line message with an OK button, through plat_dialog_choice --
   the one dialog primitive every backend implements. Keep it short:
   Amiga shows it as a requester title. */
void wp_action_tell(struct wp_app_ctx *ctx, const char *msg);

/* ---- Files ------------------------------------------------------------
   The whole of File > Open / Save / Save As except the picker, which is
   the one genuinely native part. Every backend used to carry its own copy
   of the read-sniff-import and export-write sequences, and they drifted:
   three of the four could not open RTF (it came up as raw markup) and
   wrote plain text whatever the name said, and none remembered the file
   it was editing, so every Save was a Save As. Here the format follows
   the name and the content the same way everywhere -- WordStar by sniff,
   .RTF, .MD, .ODT and .PDF by extension, plain text otherwise -- and the
   path is kept in ctx->doc_path. ODT and PDF go through the offload
   gateway (RTF both ways on the wire, see docs/formats/offload-protocol.md),
   which the backend brings up through ctx->ensure_offload; without a
   link the writer is told and nothing changes. The ST had this to itself
   until 2026-10-28; the protocol being what the serial link was chosen
   for, every target gets it now. */

/* The backend's native file picker. Fills path_out (cap bytes, NUL-
   terminated) with a full native path and returns WP_TRUE, or WP_FALSE
   when the writer cancelled. `save` picks the Save flavour. */
typedef wp_bool (*wp_pick_path_fn)(struct wp_app_ctx *ctx, wp_bool save,
                                   char *path_out, u32 cap);

/* Reads native_path into a fresh document and makes it THE document
   (wp_action_replace_document), then remembers the path. RTF pictures
   (from a .RTF, or from the RTF a .ODT comes back as) go through `sink`
   when one is given (format_rtf_import_images) and are dropped
   otherwise. Every failure is told to the writer and leaves the document
   and the remembered path as they were. Does not ask about unsaved work:
   the caller does, before its picker. */
wp_status wp_action_load_file(struct wp_app_ctx *ctx, const char *native_path,
                              wp_image_sink *sink);

/* Exports by the name's extension (.rtf is RTF, .md Markdown, .odt and
   .pdf through the gateway, anything else plain text), writes through
   wp_action_write_file (so the document is marked saved only once the
   bytes are on disk) and remembers the path -- except for PDF, which
   the program cannot read back: that is an export, the document stays
   modified and keeps its name. RTF pictures come from `img_src` when
   given, else they are left out. */
wp_status wp_action_save_file(struct wp_app_ctx *ctx, const char *native_path,
                              wp_image_source *img_src);

/* File > Open, whole: the unsaved-changes question, then a name from a
   pending ex command (":e name") or the picker, then wp_action_load_file.
   WP_OK when nothing was done (declined, cancelled). */
wp_status wp_action_file_open(struct wp_app_ctx *ctx, wp_pick_path_fn pick,
                              wp_image_sink *sink);

/* Where a Save goes: a pending ex-command name (":w name") first; else,
   unless save_as, the remembered name; else the picker. WP_FALSE means
   there is nothing to do -- no document, or the writer cancelled. Split
   out from wp_action_file_save for a backend that wants a look at the
   name before the shared exporter sees it. */
wp_bool wp_action_save_path(struct wp_app_ctx *ctx, wp_bool save_as, wp_pick_path_fn pick,
                            char *path_out, u32 cap);

/* File > Save (save_as false) or Save As (true), whole. */
wp_status wp_action_file_save(struct wp_app_ctx *ctx, wp_bool save_as, wp_pick_path_fn pick,
                              wp_image_source *img_src);

/* Sets or clears a left tab stop at x_twips (from the column's left
   edge, snapped to the nearest eighth of an inch) on every paragraph the
   selection touches, else the caret's: a stop already within an eighth
   of an inch of it is removed, otherwise one is added. What a click on
   the ruler's scale does. WP_RANGE when the paragraph already has
   WP_MAX_TABS stops. */
wp_status wp_action_toggle_tab(struct wp_app_ctx *ctx, i32 x_twips);

/* For a backend that loaded or saved the document by a route of its own. */
void wp_action_remember_path(struct wp_app_ctx *ctx, const char *native_path);

/* The remembered path's last component -- what a title bar shows -- or
   "" when the document has no file. */
const char *wp_action_document_name(const struct wp_app_ctx *ctx);

/* Case-insensitive "ends with", for extensions: wp_path_ends_with_ci(p,
   ".RTF") matches "notes.rtf" and "NOTES.RTF" alike. */
wp_bool wp_path_ends_with_ci(const char *path, const char *suffix);

#endif /* WP_ACTIONS_H */
