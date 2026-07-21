#ifndef WP_EMACS_H
#define WP_EMACS_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keyevent.h"

/* M10 Phase 3: an Emacs-style keybinding scheme, alongside vi-mode and
   WordStar (docs/keymaps.md). Structurally mirrors wordstar.h/
   wordstar.c: a transparent, standalone, host-testable state struct
   with a single wp_emacs_feed_key entry point, built entirely on the
   same public editing.h primitives every other scheme uses. Zero
   platform calls anywhere in this module, for the same
   host-testability reason.

   Real, honestly-flagged deviations from stock GNU Emacs (verified
   against the current GNU Emacs Reference Card, not recalled from
   memory) -- see docs/keymaps.md for the full account:
   - Undo binds to C-x u, not C-/ or C-x C-_. Both of those are real
     Emacs bindings, but Ctrl+<non-letter> chords (Ctrl+/, Ctrl+_) don't
     survive this project's own key translation intact on either
     backend (a Ctrl-chord is only reliably recoverable when the
     original key was a plain letter -- see keyevent.h's own
     mods/codepoint contract). C-x u sidesteps this entirely: only C-x
     itself needs Ctrl (a letter, X, works fine), the follow-up 'u' is
     a plain key.
   - Mark-setting (C-SPC, or its alternate C-@) has the exact same
     Ctrl+<non-letter> problem -- Ctrl+Space produces ASCII NUL on real
     hardware, which Atari's own atari_translate_key treats identically
     to a genuinely unmapped key (see that function's own "ascii == 0"
     branch), and Amiga's MapRawKey-based path doesn't fare any better.
     Implemented and fully host-testable regardless (a host test
     constructs the wp_key_event directly, bypassing the real backend's
     translation entirely) -- but NOT currently reachable from a real
     keypress on either shipped backend. A real, honest gap, flagged
     here rather than silently worked around with an invented
     non-historical binding; mark+point, C-w (kill region), and M-w
     (copy region) all inherit this same practical limitation until a
     future backend revisit. C-y (yank) and the word/line-kill commands
     do not depend on mark and are fully reachable.
   - No kill-RING, one kill slot: M-y (cycle to an older kill) is
     explicitly out of scope this pass -- with no cycling, a ring of N
     slots behaves identically to one slot, so wp_emacs_state uses a
     single mem_alloc'd slot (matching wp_vimode_state's own
     register_bytes exactly) rather than the multi-slot ring an earlier
     sketch of this scheme called for. C-w/M-w/M-d/M-DEL all write it;
     C-y reads it. Not a ring -- see this struct's own kill_bytes
     comment.
   - Bold/Italic/Underline (C-c b/i/u) borrow the real Emacs
     C-c <letter> "user prefix" convention -- plain Emacs is a
     text-only editor with no formatting commands of its own to
     inherit, so this is this project's own choice within a real,
     documented Emacs idiom, not an invented one.
   - Meta is real Alt (WP_MOD_ALT, M10 Phase 2) OR the historical
     ESC-then-key fallback -- a deliberate, documented departure from
     every other scheme's own cancel semantic: bare Escape here means
     "await the next key as a Meta chord," not "cancel." No
     timeout-based disambiguation between "Escape as Meta-prefix" and
     "Escape meant to do something else" is attempted this pass (plain
     Emacs itself has none either in a dumb-terminal sense -- Meta and
     Escape really are the same input in classic Emacs). C-g is the
     real Emacs cancel key instead, clearing any pending prefix/meta
     state.
   - Numeric-argument commands (M-0 C-k etc.), multi-buffer commands
     (C-x s, C-x i), page/screen motion (C-v/M-v/C-x [/C-x ]) -- no
     "page" or "screen" concept is surfaced to any keymap scheme -- and
     M-g g (goto-line) -- no line-number concept is surfaced either --
     remain deferred, matching WordPerfect's own menu-UI deferral.
   - Find/Replace (M11): C-s/C-r incremental search (isearch), genuinely
     live -- every keystroke re-searches, no Enter needed, matching real
     Emacs exactly (a structurally different UX from every other
     scheme's own collect-then-submit prompt). Forward isearch lands
     point at the END of the match, backward at its START, real Emacs's
     own asymmetry. M-% query-replace with the real y/SPC (replace+
     next), n/DEL (skip), ! (replace all remaining), . (replace+stop),
     q/RET (quit) vocabulary -- the more advanced real keys (,/^/C-r/
     C-w/C-l/C-h: replace-then-ask, back up, recursive edit, redisplay,
     help) are deliberately not implemented, a narrower scoped subset,
     documented not faked, the same treatment this scheme's own kill-
     ring already got. Query-replace is forward-only from point to the
     end of the document (real stock M-% has no reverse variant and no
     G-equivalent restart-from-top, matching WordPerfect's own -- not
     WordStar's -- narrower scope here). */

typedef enum {
    WP_EMACS_PROMPT_NONE = 0,
    WP_EMACS_PROMPT_FIND_TEXT,     /* M-% first field: "Query replace:" */
    WP_EMACS_PROMPT_REPLACE_TEXT,  /* M-% second field: "Query replace with:" */
    WP_EMACS_PROMPT_CONFIRM_MATCH  /* per-match y/SPC/n/DEL/!/./q/RET */
} wp_emacs_prompt_stage;

#define WP_EMACS_ISEARCH_MAX 128
#define WP_EMACS_QR_MAX      128

typedef struct {
    wp_document     *doc;      /* borrowed */
    wp_undo_journal *journal;  /* borrowed */
    wp_pos cursor;

    u8      pending_prefix; /* 0 = none, else 'x' or 'c' (C-x/C-c awaiting
                                its own follow-up key) -- single-level,
                                same shape wordstar.h's own pending_prefix
                                uses; deeper real Emacs prefixes (C-x 4 f
                                etc.) are out of scope, see this file's
                                own header comment */
    wp_bool pending_meta;   /* WP_TRUE right after a bare Escape, awaiting
                                the key that completes an M-<key> chord */

    wp_bool mark_set;       /* the region is (mark, cursor) when true --
                                same anchor+cursor shape wp_selection/vi's
                                own Visual mode already use */
    wp_pos  mark;

    u8  *kill_bytes; /* mem_alloc'd; NULL if empty -- single slot, not a
                         ring; see this file's own header comment on why */
    u32  kill_len;
    u32  kill_cap;

    /* M11: C-s/C-r incremental search. isearch_mode is 0 (inactive),
       'f' (C-s, forward), or 'r' (C-r, backward) -- pressing either key
       again while active repeats in THAT key's direction, so C-r
       mid-forward-search flips direction, matching real Emacs. Typing a
       character (or Backspace) always re-searches from isearch_anchor
       (where the search began) with the new pattern; an explicit C-s/
       C-r repeat instead searches from isearch_last_match, advancing
       past it -- two genuinely different "search from" points real
       isearch also distinguishes. A failed search leaves the cursor/
       last-match state untouched (real Emacs's own "Failing I-search"
       leniency -- still-typeable, not auto-cancelled). */
    u8      isearch_mode;
    u8      isearch_buf[WP_EMACS_ISEARCH_MAX];
    u32     isearch_len;
    wp_pos  isearch_anchor;
    wp_bool isearch_have_match;
    wp_pos  isearch_last_match;

    /* M11: M-% query-replace prompt state. Always pauses at the first
       match (or BEEPs if none) -- unlike WordStar/WordPerfect's own
       upfront "confirm every match, or none" choice, real query-replace
       ALWAYS starts interactive; "replace the rest silently" is reached
       mid-loop via '!' instead. */
    u8      prompt_stage;   /* wp_emacs_prompt_stage */
    u8      qr_find[WP_EMACS_QR_MAX];
    u32     qr_find_len;
    u8      qr_replace[WP_EMACS_QR_MAX];
    u32     qr_replace_len;
    wp_pos  scope_start;   /* fixed for the life of one M-% run: point when M-% began */
    wp_pos  pending_match; /* awaiting a confirm keystroke, WP_EMACS_PROMPT_CONFIRM_MATCH only */
} wp_emacs_state;

typedef enum {
    WP_EMACSACTION_NONE = 0,     /* consumed internally (motion/edit/pending state),
                                     or a plain character insertion -- see
                                     wp_emacs_state's own doc, callers distinguish
                                     "inserted a char" the same way keymap.c's other
                                     arms do: checking pending state/mods/codepoint
                                     BEFORE the call, not a dedicated result kind */
    WP_EMACSACTION_BEEP,         /* invalid/unbound chord, or an operation needing
                                     an unset mark (C-w/C-x C-x with no mark) */
    WP_EMACSACTION_SAVE,         /* C-x C-s */
    WP_EMACSACTION_OPEN,         /* C-x C-f */
    WP_EMACSACTION_QUIT,         /* C-x C-c */
    WP_EMACSACTION_FORMAT_BOLD,      /* C-c b */
    WP_EMACSACTION_FORMAT_ITALIC,    /* C-c i */
    WP_EMACSACTION_FORMAT_UNDERLINE  /* C-c u */
} wp_emacs_action_kind;

/* doc/journal are borrowed, same convention every other scheme uses.
   Cursor starts at doc start; the caller (platform/common/keymap.c)
   overrides this the same way it already does for vi-mode/WordStar
   when a backend wants a different default. */
void wp_emacs_init(wp_emacs_state *vs, wp_document *doc, wp_undo_journal *journal);

/* Frees kill_bytes (unlike WordStar, this scheme DOES own heap memory
   -- see wp_emacs_state's own comment). Does not touch doc/journal
   (borrowed). Call before re-initializing onto a replacement document
   or at shutdown -- same contract wp_vimode_free/wp_keymap_free
   already establish. */
void wp_emacs_free(wp_emacs_state *vs);

/* The entire input surface -- mirrors wp_wordstar_feed_key's single-
   entry-point shape exactly, the same thing that makes
   host/tests/test_emacs.c's synthetic-key-script testing approach
   possible. */
wp_emacs_action_kind wp_emacs_feed_key(wp_emacs_state *vs, wp_key_event key);

/* Writes a NUL-terminated status string into out, truncating to fit
   out_cap -- mirrors every other scheme's own get_status_text shape
   (M11). Outside isearch/query-replace this is the fixed "EMACS" label
   platform/common/keymap.c used to hardcode directly; inside one, it's
   the real Emacs minibuffer-style prompt ("I-search:"/"I-search
   backward:"/"Query replace:"/"Query replace with:"/"Query replacing
   (y/n/!/.)?" ) followed by whatever's been typed so far. */
void wp_emacs_get_status_text(const wp_emacs_state *vs, char *out, u32 out_cap);

#endif /* WP_EMACS_H */
