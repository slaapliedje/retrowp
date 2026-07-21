#ifndef WP_MSWORD_H
#define WP_MSWORD_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keyevent.h"

/* M10 Phase 5: a classic-Microsoft-Word-style keybinding scheme,
   alongside vi-mode/WordStar/Emacs/WordPerfect (docs/keymaps.md).
   Structurally mirrors the other three: a transparent, standalone,
   host-testable state struct with a single wp_msword_feed_key entry
   point, built entirely on the same public editing.h primitives every
   other scheme uses.

   Lowest-novelty of the five schemes by far -- closest to a modern,
   familiar, non-modal editor, and most of its Ctrl-letter bindings
   (B/I/U/S/O/C/X/V) are already platform/common/actions.c's
   g_wp_actions[] own real shortcut_ascii bytes, so this scheme's job
   for those is direct pass-through to the matching wp_action_id (via
   platform/common/keymap.c's own translation, the same way every other
   scheme's whole-command output works -- see keymap.h's own comment on
   why engine/ code can't reference wp_action_id directly). No prefix-
   key mechanism exists at all, like WordPerfect -- every command is
   one keystroke plus modifiers.

   Two things distinguish this scheme's own state, beyond the ones
   above:
   - A real Shift+motion anchor+extend selection (same wp_selection
     shape WordPerfect's own Block mode and Emacs's own mark+point use)
     that Delete/Backspace/Enter/plain typing all consult and consume --
     matching the familiar "select then act, or type to replace"
     behavior most modern editors share. This is the one scheme this
     milestone gives selection a REAL consumer beyond a later explicit
     kill/copy keystroke: typing over a selection replaces it.
   - Real Ctrl-Z/Ctrl-Y undo/redo -- the only scheme in this milestone
     with redo at all (WordStar has no undo full stop; Emacs's own real
     C-/ has no separate redo, "redo" being "undo the undos"; WordPerfect
     5.1's own F1 is a narrower single-deletion Undelete, not general
     undo/redo). Classic Word's own real Ctrl-Z/Ctrl-Y map directly onto
     this engine's own wp_edit_undo/wp_edit_redo (editing.h), already a
     full stack -- nothing to narrow here.

   Deliberately unbound, not silently missing: there is no in-app quit
   keystroke at all. Real classic Word has no standard in-editor
   keyboard shortcut for "quit the application" either (Alt-F4 is an OS/
   window-manager convention, not something this app's own key dispatch
   would normally intercept) -- and every backend's own window-close
   handling (WM_CLOSED-equivalent) already lets the user quit via the
   window chrome regardless of which keymap scheme is active, entirely
   independent of this scheme's own key dispatch. Reusing some other
   scheme's own Ctrl+Q convention here would be inventing a non-
   historical Word binding purely for convenience -- the same discipline
   every other scheme in this milestone already holds itself to. Cut/
   Copy/Paste route through the EXISTING whole-document-scoped
   WP_ACT_EDIT_CUT/COPY/PASTE actions unchanged (this project's own
   documented, pre-existing limitation -- "this app has no selection-
   scoped clipboard yet", platform/common/actions.c's own comment on
   act_edit_copy) -- this scheme's own Shift-selection doesn't feed into
   them, matching vi-mode's identical pre-existing behavior for the same
   three shortcuts today. Find (Ctrl-F) is deferred, matching every
   other scheme's own Find deferral. */

typedef struct {
    wp_document     *doc;      /* borrowed */
    wp_undo_journal *journal;  /* borrowed */
    wp_pos cursor;

    wp_bool selecting; /* the selection is (anchor, cursor) while true --
                           same anchor+cursor shape wp_selection/every
                           other scheme's own region concept already uses */
    wp_pos  anchor;
} wp_msword_state;

typedef enum {
    WP_MSACTION_NONE = 0,   /* consumed internally (motion/edit/selection),
                                or a plain character insertion -- see
                                wp_msword_state's own doc, callers distinguish
                                "inserted a char" the same way every other
                                scheme's own keymap.c arm does */
    WP_MSACTION_BEEP,       /* invalid/unbound chord */
    WP_MSACTION_SAVE,       /* Ctrl-S */
    WP_MSACTION_OPEN,       /* Ctrl-O */
    WP_MSACTION_CUT,        /* Ctrl-X */
    WP_MSACTION_COPY,       /* Ctrl-C */
    WP_MSACTION_PASTE,      /* Ctrl-V */
    WP_MSACTION_FORMAT_BOLD,      /* Ctrl-B */
    WP_MSACTION_FORMAT_ITALIC,    /* Ctrl-I */
    WP_MSACTION_FORMAT_UNDERLINE  /* Ctrl-U */
} wp_msword_action_kind;

/* doc/journal are borrowed, same convention every other scheme uses.
   Cursor starts at doc start; the caller (platform/common/keymap.c)
   overrides this the same way it already does for every other scheme. */
void wp_msword_init(wp_msword_state *vs, wp_document *doc, wp_undo_journal *journal);

/* The entire input surface -- mirrors every other scheme's single-
   entry-point shape exactly. No wp_msword_free is declared: this
   scheme owns no heap memory at all (Cut/Copy/Paste route through the
   existing registry actions rather than a scheme-local register/kill-
   ring -- see this file's own header comment) -- platform/common/
   keymap.c's WP_KEYMAP_MSWORD arm of wp_keymap_free is a deliberate
   no-op case, matching WordStar's own precedent, not an oversight. */
wp_msword_action_kind wp_msword_feed_key(wp_msword_state *vs, wp_key_event key);

#endif /* WP_MSWORD_H */
