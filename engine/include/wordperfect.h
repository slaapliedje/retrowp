#ifndef WP_WORDPERFECT_H
#define WP_WORDPERFECT_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keyevent.h"

/* M10 Phase 4: a WordPerfect 5.1-style keybinding scheme, alongside
   vi-mode/WordStar/Emacs (docs/keymaps.md). Structurally mirrors
   wordstar.h/wordstar.c and emacs.h/emacs.c: a transparent, standalone,
   host-testable state struct with a single wp_wordperfect_feed_key
   entry point, built entirely on the same public editing.h primitives
   every other scheme uses.

   Real WordPerfect 5.1 is a function-key + modifier scheme (verified
   against the real 1990 IBM-keyboard template, not recalled from
   memory) that is ALSO authentically menu-driven -- most function keys
   open a full numbered on-screen menu, not a flat command. This
   milestone's own scope decision (see the M10 plan, Phase 4) is a
   deliberate, honest flattening: real F-key/modifier BINDINGS this
   pass, the numbered-menu/prompt UI itself left as an explicit,
   optional future spike -- as much a UI-shape problem as a keybinding
   one, and this is a keybinding-layout milestone. Every simplification
   below is a real, considered choice, not an oversight:

   - Block operations flatten from a real 3-way menu (Ctrl-F4 -> Block/
     Copy/Move/Delete) to ONE action, Move (Ctrl-F4): deletes the active
     block into a register, exactly like a cut. Shift-F10 (Retrieve)
     pastes it back at the cursor -- together these cover the same net
     workflow real WP's own Move does. Pure Copy (leave the original in
     place) and a register-less pure Delete are both real WP options
     this flattening doesn't expose separately -- deferred, not
     invented around.
   - F12 (real WP's own alternate block-toggle key, alongside Alt-F4)
     is not bindable at all in this project: wp_special_key
     (keyevent.h) only defines F1-F10 ("real ST/Amiga/classic-Mac
     keyboards commonly stop at F10" -- that header's own comment).
     Alt-F4 alone covers block-toggle here, a real structural
     limitation, not a choice.
   - Ctrl-Home is real WP's "Go To" (prompts for a page number or
     character to jump to) -- with no such prompt UI this pass, it's
     simplified to a direct jump to document start instead. Document
     END has no equivalent single-key binding here: real WP's own is
     Home Home Down (a double-Home prefix this MVP doesn't implement),
     and Ctrl-End is already real WP's OWN binding for something else
     (delete to end of line) -- deferred, not invented around.
   - Home/End map to a single line start/end (real WP's actual Home
     Home Left/Home Home Right double-press is for skipping past
     hidden formatting codes specifically -- this engine has no
     separate "hidden code" concept for plain cursor motion to skip).
   - F1 "Undelete" restores only the single most recent deletion (this
     engine's undo journal is a real, general undo/redo stack, not
     real WP 5.1's own bounded 3-deletion ring) -- narrower than real
     5.1, documented, not silently faked as equivalent.
   - Italic has no direct key in real WP 5.1 either (it lived under a
     Ctrl-F8 Font/Appearance menu) -- Bold (F6) and Underline (F8) are
     both real, direct WP keys and are implemented; Italic is left out
     entirely rather than inventing a non-historical key for it.
   - Find/Replace (M11): F2 Search forward (`-> Srch:` prompt, verified
     against the WordPerfect Workbook), Shift-F2 Search backward
     (`<- Srch:`), Alt-F2 Replace (`w/Confirm? (Y/N)` -> search text ->
     replace text, each field terminated by pressing F2 again -- real
     WP's own Search key is heavily overloaded: start a search, submit
     a typed field, AND (pressed with an untouched, still-populated
     field) repeat the last search verbatim, exactly the real "press
     +Search twice to find the second occurrence" behavior). No G-
     equivalent "restart from document top" option exists in this
     scheme (WordStar's own OPTIONS? letters don't carry over -- real
     WP 5.1's Replace has no such toggle in the sources checked this
     pass); scope is always cursor-to-end, forward or backward per
     which Search key started it. */

typedef enum {
    WP_WP_PROMPT_NONE = 0,
    WP_WP_PROMPT_REPLACE_CONFIRM, /* "w/Confirm? (Y/N)" -- Alt-F2's own first step */
    WP_WP_PROMPT_SEARCH_TEXT,     /* "-> Srch:" / "<- Srch:" -- shared by plain
                                      Search and Replace's own find-text field */
    WP_WP_PROMPT_REPLACE_TEXT,    /* Alt-F2 only, second field */
    WP_WP_PROMPT_CONFIRM_MATCH    /* per-match Y/N, only when w/Confirm? was Y */
} wp_wordperfect_prompt_stage;

#define WP_WORDPERFECT_PROMPT_MAX 128

typedef struct {
    wp_document     *doc;      /* borrowed */
    wp_undo_journal *journal;  /* borrowed */
    wp_pos cursor;

    /* Block mode: a real anchor+extend selection (region = block_start
       to cursor while active), the same shape wp_selection/vi's own
       Visual mode and Emacs's own mark+point already use -- but
       activated as an explicit TOGGLE (Alt-F4 turns it on; Alt-F4
       again turns it off and cancels, no operation performed), a real,
       distinct third UX pattern alongside vi's modal enter/exit and
       Emacs's set-and-persist. */
    wp_bool block_active;
    wp_pos  block_start;

    u8  *block_bytes; /* mem_alloc'd register for Move (Ctrl-F4) /
                          Retrieve (Shift-F10); NULL if empty -- same
                          single-slot discipline wp_emacs_state's own
                          kill_bytes already established */
    u32  block_len;
    u32  block_cap;

    /* M11: Find/Replace prompt state. prompt_find/prompt_replace are
       deliberately NEVER cleared on entry into their own stage (unlike
       WordStar's own always-fresh prompt) -- that persistence IS the
       mechanism behind real WP's "press Search twice" repeat: entering
       SEARCH_TEXT leaves whatever was typed last time already sitting
       in the field, so an immediate F2 with zero new keystrokes submits
       that same text again. */
    u8      prompt_stage;      /* wp_wordperfect_prompt_stage */
    wp_bool prompt_is_replace; /* WP_TRUE for Alt-F2, WP_FALSE for plain F2/Shift-F2 */
    wp_bool prompt_backward;   /* WP_TRUE if Shift-F2 started this search */
    wp_bool replace_confirm_each; /* the answer to w/Confirm? (Y/N) */
    u8      prompt_find[WP_WORDPERFECT_PROMPT_MAX];
    u32     prompt_find_len;
    u8      prompt_replace[WP_WORDPERFECT_PROMPT_MAX];
    u32     prompt_replace_len;
    wp_pos  scope_start;   /* fixed for the life of one Search/Replace run */
    wp_pos  pending_match; /* awaiting a Y/N keystroke, WP_WP_PROMPT_CONFIRM_MATCH only */
} wp_wordperfect_state;

typedef enum {
    WP_WPACTION_NONE = 0,    /* consumed internally (motion/edit/block toggle),
                                 or a plain character insertion -- see
                                 wp_wordperfect_state's own doc, callers
                                 distinguish "inserted a char" the same way
                                 every other scheme's own keymap.c arm does */
    WP_WPACTION_BEEP,        /* invalid/unbound chord, or Ctrl-F4/Shift-F10
                                 with no active block / empty register */
    WP_WPACTION_SAVE,        /* F10 */
    WP_WPACTION_QUIT,        /* F7 */
    WP_WPACTION_FORMAT_BOLD,      /* F6 */
    WP_WPACTION_FORMAT_UNDERLINE  /* F8 */
} wp_wordperfect_action_kind;

/* doc/journal are borrowed, same convention every other scheme uses.
   Cursor starts at doc start; the caller (platform/common/keymap.c)
   overrides this the same way it already does for every other scheme. */
void wp_wordperfect_init(wp_wordperfect_state *vs, wp_document *doc, wp_undo_journal *journal);

/* Frees block_bytes (this scheme DOES own heap memory, like Emacs's
   kill_bytes -- unlike WordStar). Does not touch doc/journal
   (borrowed). Same contract every other scheme's own free function
   already establishes. */
void wp_wordperfect_free(wp_wordperfect_state *vs);

/* The entire input surface -- mirrors every other scheme's single-
   entry-point shape exactly, the same thing that makes
   host/tests/test_wordperfect.c's synthetic-key-script testing
   approach possible. No prefix-key state machine exists in this
   scheme at all (unlike WordStar's ^K/^Q/^P or Emacs's C-x/C-c) --
   the flattened MVP's whole command set is single-keystroke-plus-
   modifier, a genuine structural simplification from real WP 5.1's
   own menu chains, not just an implementation shortcut. */
wp_wordperfect_action_kind wp_wordperfect_feed_key(wp_wordperfect_state *vs, wp_key_event key);

/* Writes a NUL-terminated status string into out, truncating to fit
   out_cap -- mirrors wp_vimode_get_status_text's/wp_wordstar_get_status_
   text's exact signature and role (M11). Outside a Search/Replace
   prompt this is the fixed "WORDPERFECT" label platform/common/keymap.c
   used to hardcode directly; inside one, it's the real WP prompt text
   ("-> Srch:"/"<- Srch:"/"w/Confirm? (Y/N)"/"Replace with:") followed by
   whatever's been typed so far. */
void wp_wordperfect_get_status_text(const wp_wordperfect_state *vs, char *out, u32 out_cap);

#endif /* WP_WORDPERFECT_H */
