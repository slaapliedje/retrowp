#ifndef WP_ACTIONS_H
#define WP_ACTIONS_H

#include "types.h"
#include "app_ctx.h"

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
    WP_ACT_FILE_OPEN = 0,
    WP_ACT_FILE_SAVE,
    WP_ACT_FILE_QUIT,
    WP_ACT_EDIT_CUT,
    WP_ACT_EDIT_COPY,
    WP_ACT_EDIT_PASTE,
    WP_ACT_FORMAT_BOLD,
    WP_ACT_FORMAT_ITALIC,
    WP_ACT_FORMAT_UNDERLINE,
    WP_ACT_FORMAT_SIZE,
    WP_ACT_FORMAT_ALIGN,
    WP_ACT_TOOLS_SPELLCHECK,
    WP_ACT_TOOLS_THESAURUS,
    WP_ACT_TOOLS_GRAMMARCHECK,
    WP_ACT_TOOLS_CHAT,
    WP_ACT_HELP_CONTENTS,
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

#endif /* WP_ACTIONS_H */
