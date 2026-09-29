/* SPDX-License-Identifier: GPL-2.0-or-later */
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
   three shortcuts today.
   Find/Replace (M11): Ctrl+F Find (single jump, natural document-
   wraparound, matching vi's own `/`), Ctrl+H Replace, Shift+F4 repeats
   the last executed Find verbatim (real Word's own "repeat last Find or
   Go To action" binding -- F3 is deliberately NOT used for this despite
   being a common assumption from other apps/browsers: real classic
   Word's own F3 is AutoText/glossary expansion, a wholly different
   feature). Both fields are Enter-terminated (real Word's own dialog
   convention, unlike WordPerfect's F2-terminated fields) and always
   start EMPTY on each Ctrl+F/Ctrl+H press -- WordStar's own "always
   fresh prompt" choice, not real Word's actual dialog-remembers-last-
   value behavior, a deliberate simplification since Shift+F4 already
   covers "repeat the last search" on its own. Replace, once both fields
   are submitted, immediately jumps to the first match and pauses,
   standing in for real Word's own persistent Find-and-Replace dialog
   with its Replace/Replace All/Find Next/Close buttons: 'r' replaces
   this match and advances (real "Replace"), 'a' replaces this and every
   remaining match silently (real "Replace All"), 'n' skips and advances
   (real "Find Next"), Escape closes (real "Close/Cancel") -- a keyboard-
   only adaptation of a real 4-button dialog this app has no modal-
   dialog primitive for, the same honest simplification every other
   scheme's own confirm-loop already makes. Forward-only, cursor to end
   of document, no G-equivalent restart-from-top (matching WordPerfect's/
   Emacs's own narrower scope, not WordStar's). */

typedef enum {
    WP_MS_PROMPT_NONE = 0,
    WP_MS_PROMPT_FIND_TEXT,     /* Ctrl+F, or Ctrl+H's own first field */
    WP_MS_PROMPT_REPLACE_TEXT,  /* Ctrl+H's second field */
    WP_MS_PROMPT_REPLACE_CONFIRM /* r/a/n/Escape standing in for the real dialog's buttons */
} wp_msword_prompt_stage;

#define WP_MSWORD_PROMPT_MAX 128

typedef struct {
    wp_document     *doc;      /* borrowed */
    wp_undo_journal *journal;  /* borrowed */
    wp_pos cursor;

    wp_bool selecting; /* the selection is (anchor, cursor) while true --
                           same anchor+cursor shape wp_selection/every
                           other scheme's own region concept already uses */
    wp_pos  anchor;

    /* M11: Find/Replace prompt state. */
    u8      prompt_stage;      /* wp_msword_prompt_stage */
    wp_bool prompt_is_replace; /* WP_TRUE for Ctrl+H, WP_FALSE for plain Ctrl+F */
    u8      find_buf[WP_MSWORD_PROMPT_MAX];
    u32     find_len;
    u8      replace_buf[WP_MSWORD_PROMPT_MAX];
    u32     replace_len;
    u8      last_find[WP_MSWORD_PROMPT_MAX]; /* snapshot taken whenever a search actually
                                                 runs -- Shift+F4's own repeat target */
    u32     last_find_len;
    wp_pos  scope_start;   /* fixed for the life of one Replace run (Ctrl+F's own single
                               jump has no scope -- it wraps naturally, like vi's `/`) */
    wp_pos  pending_match; /* awaiting a confirm keystroke, WP_MS_PROMPT_REPLACE_CONFIRM only */
} wp_msword_state;

typedef enum {
    WP_MSACTION_NONE = 0,   /* consumed internally (motion/edit/selection),
                                or a plain character insertion -- see
                                wp_msword_state's own doc, callers distinguish
                                "inserted a char" the same way every other
                                scheme's own keymap.c arm does */
    WP_MSACTION_BEEP,       /* invalid/unbound chord */
    WP_MSACTION_SAVE,       /* Ctrl-S */
    WP_MSACTION_QUIT,       /* Ctrl-Q -- this scheme had no quit binding at
                                all until now. Harmless on the backends with
                                a menu bar (File > Quit is always there), but
                                DOS has no menu whatsoever, and its title bar
                                advertises "Ctrl+Q: Quit" unconditionally --
                                so under keymap=msword the DOS build could
                                not be exited from the keyboard at all. */
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

/* Writes a NUL-terminated status string into out, truncating to fit
   out_cap -- mirrors every other scheme's own get_status_text shape
   (M11). Outside a Find/Replace prompt this is the fixed "WORD" label
   platform/common/keymap.c used to hardcode directly; inside one, it's
   the real Word-style prompt text ("Find:"/"Find what:"/"Replace
   with:"/"Replace (r/a/n)?") followed by whatever's been typed so far. */
void wp_msword_get_status_text(const wp_msword_state *vs, char *out, u32 out_cap);

#endif /* WP_MSWORD_H */
