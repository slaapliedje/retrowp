#ifndef WP_WORDSTAR_H
#define WP_WORDSTAR_H

#include "types.h"
#include "docmodel.h"
#include "editing.h"
#include "keyevent.h"

/* M10 Phase 1: a WordStar-style keybinding scheme, alongside vi-mode
   (vimode.h) as one of several switchable layouts (docs/keymaps.md).
   Structurally mirrors vimode.h/vimode.c: a transparent, standalone,
   host-testable state struct with a single wp_*_feed_key entry point,
   built entirely on the same public editing.h primitives vi-mode uses
   -- no new engine-level structural logic, only a different key->motion/
   edit mapping. Zero platform calls anywhere in this module, same as
   vimode.c, for the same host-testability reason.

   Verified against primary sources (the WordStar 3.0 Quick Reference
   Card, wordstar.org's own command documentation), not recalled from
   memory -- see docs/keymaps.md for the full account and every
   deliberate simplification this MVP makes:
   - The "diamond" cursor cluster (^S/^D char, ^E/^X line/paragraph,
     ^A/^F word) plus arrow keys as an added, ahistorical-but-deliberate
     convenience (most WordStar-era keyboards had no arrows at all;
     modern vintage-adjacent keyboards do, and refusing to support them
     would be user-hostile for no real benefit).
   - Five real prefix keys (^K block/file, ^Q quick-movement, ^P
     print/format, ^O onscreen, ^J help) -- only ^K/^Q/^P have any
     subcommands implemented this pass; ^O/^J are registered (so
     pressing them doesn't fall through as a bare character insert,
     matching their real historical existence) but every follow-up key
     BEEPs, an honest, checked scope limit, not a silent gap.
   - The prefix's second keystroke works with or without Ctrl held
     (^KB and ^K then plain B are equivalent) -- WordStar's own real
     convention, reproduced by case-folding the follow-up key regardless
     of its mods.
   - Block marking is a MARKER PAIR (^KB begin, ^KK end), not an
     anchor+extend selection -- copy/move/delete act on the region
     between markers, matching WordStar's own real "block" model (no
     separate clipboard concept).
   - No undo binding at all -- classic WordStar (3.x/4.x) never had one;
     inventing one would misrepresent the scheme being emulated.
   - Find/Replace (^QF/^QA) are explicitly deferred: implementing them
     needs their own interactive text-collection sub-state (mirroring
     vi-mode's own Command-line mode), a real chunk of additional work
     out of scope for this phase -- not silently dropped, a documented
     gap like WordPerfect's own menu-UI deferral (Phase 4). */

typedef struct {
    wp_document     *doc;      /* borrowed */
    wp_undo_journal *journal;  /* borrowed */
    wp_pos cursor;

    u8 pending_prefix; /* 0 = none, else the ASCII prefix letter
                           ('k'/'q'/'p'/'o'/'j') awaiting its second
                           keystroke -- single-level, mirrors vimode.c's
                           own pending_prefix field exactly (currently
                           only 'g' there, for gg) */

    /* WordStar's own real block model: two independently settable
       markers, not an anchor+extend wp_selection. Either may be set
       before the other, and either may land before or after its
       counterpart in document order -- every block operation
       normalizes at use time (see wordstar.c's own block helpers). */
    wp_bool block_begin_set;
    wp_pos  block_begin;
    wp_bool block_end_set;
    wp_pos  block_end;
} wp_wordstar_state;

typedef enum {
    WP_WSACTION_NONE = 0,      /* consumed internally (motion/edit/pending-prefix),
                                   or a plain character insertion -- see
                                   wp_wordstar_state's own doc, callers distinguish
                                   "inserted a char" the same way keymap.c's VI arm
                                   does: checking pending_prefix/mods/codepoint
                                   BEFORE the call, not a dedicated result kind */
    WP_WSACTION_BEEP,          /* invalid/unbound prefix follow-up key */
    WP_WSACTION_SAVE,          /* ^KS or ^KD -- "save, keep editing" (real WordStar
                                   distinguishes these two -- resume-editing vs.
                                   back-to-opening-menu -- this app has no opening
                                   menu to return to, so both map to the same
                                   portable Save action) */
    WP_WSACTION_QUIT,          /* ^KQ -- abandon edit, no save */
    WP_WSACTION_SAVE_QUIT,     /* ^KX -- save & exit */
    WP_WSACTION_FORMAT_BOLD,      /* ^PB */
    WP_WSACTION_FORMAT_ITALIC,    /* ^PY */
    WP_WSACTION_FORMAT_UNDERLINE  /* ^PS */
} wp_wordstar_action_kind;

/* doc/journal are borrowed, same convention wp_vimode_init already
   uses. Cursor starts at doc start; the caller (platform/common/
   keymap.c) overrides this the same way it already does for vi-mode
   when a backend wants a different default (e.g. Atari's own
   replace_document lands the cursor at doc end instead). */
void wp_wordstar_init(wp_wordstar_state *vs, wp_document *doc, wp_undo_journal *journal);

/* The entire input surface -- mirrors wp_vimode_feed_key's single-
   entry-point shape exactly, the same thing that makes
   host/tests/test_wordstar.c's synthetic-key-script testing approach
   possible. No wp_wordstar_free is declared: this scheme currently owns
   no heap memory at all (no register/kill-ring, unlike vi-mode's own
   yank register) -- platform/common/keymap.c's WP_KEYMAP_WORDSTAR arm
   of wp_keymap_free is a deliberate no-op case, not an oversight. */
wp_wordstar_action_kind wp_wordstar_feed_key(wp_wordstar_state *vs, wp_key_event key);

#endif /* WP_WORDSTAR_H */
