# Switchable Keybinding Layouts (M10)

## Overview

RetroWP's editing was vi-only through M8.5. M10 makes the keybinding
scheme itself switchable: WordStar, WordPerfect (5.1-era DOS), Emacs,
and classic Microsoft Word now sit alongside vi-mode, selected once at
startup from a small config file and fixed for the process lifetime --
no live switching, matching the plan's own explicit scope. Every scheme
stays 100% keyboard-usable; nothing added here needs a mouse.

Each scheme is verified against real primary sources, not recalled from
memory: the WordStar 3.0 Quick Reference Card and wordstar.org's own
command documentation, the real WordPerfect 5.1 IBM keyboard template,
and the current GNU Emacs Reference Card. Every deliberate
simplification is documented in the relevant engine header's own file
comment (`engine/include/wordstar.h`, `emacs.h`, `wordperfect.h`,
`msword.h`) and summarized again below -- this file is the narrative
account; those headers are the source of truth for exact bindings.

## The config file

A line-based `key=value` text file, read once at startup via
`platform/common/config.h`/`config.c`. Currently the sole key is
`keymap`, one of `vim` (default), `wordstar`, `emacs`, `wordperfect`,
or `msword`/`word`. `#`/`;` lines are comments; unknown keys/lines and
unrecognized values are silently skipped (lenient, matching
`help_doc.c`'s own leniency precedent) -- an existing zero-config
install behaves exactly as it always has.

Parsing (`wp_config_parse`) is pure -- no `mem_alloc`, no platform
calls -- fully host-tested against hand-fed byte arrays
(`host/tests/test_config.c`). `wp_config_load` is the thin per-platform
wrapper: `plat_file_exists` -> open/size/read into a small fixed stack
buffer -> parse -> close. Missing/unreadable/malformed all quietly
yield the default (`vim`), never an error.

Per-platform path, a real, separate decision each (following the
existing `WP_ATARI_SPELL_DICT_PATH` fixed-path convention -- no new
`platform.h` primitive needed):
- **Atari**: `C:\RETROWP.CFG`
- **Amiga**: `PROGDIR:RetroWP.cfg` -- a standard, always-available
  AmigaDOS assign, no dependency on a full `ENV:`/Preferences tree.

Both on-target selfchecks (`wp_atari_config_selfcheck`,
`wp_amiga_config_selfcheck`, wired into `AMISMOKE`/`SMOKE.PRG`) write a
real config file at the real path for each of the five `keymap=`
values and confirm `wp_config_load` resolves every one correctly
through real file I/O -- `test_config.c` already proves the parsing
logic itself exhaustively; these prove the on-target round trip.

## The keymap dispatch seam

`platform/common/keymap.h`/`.c` replaces what used to be a hardcoded
`wp_vimode_state` field on `wp_app_ctx` with a tagged union,
`wp_keymap_state` (one member per scheme: `vi`, `ws`, `em`, `wp5`,
`mw`), selected by `wp_app_ctx::keymap_id` and fixed once
`wp_keymap_init` runs. This is a deliberate tagged union embedded by
value -- not a heap-allocated `void *` -- matching this codebase's
transparent-struct convention throughout (`wp_document`/
`wp_undo_journal`/every scheme's own state are all plain structs, never
hidden behind an opaque handle); there's no dynamic loading here to
justify the indirection, since the scheme is fixed at startup. Each
arm keeps its own `cursor`/`doc`/`journal` fields (mirroring
`wp_vimode_state`'s own shape) so every scheme stays independently
host-testable with no `wp_app_ctx` in the picture at all -- the same
way `host/tests/test_vimode.c` already worked before M10.

Every backend's key dispatch now goes through one call,
`wp_keymap_feed_key(ctx, ev, raw_shortcut_byte)`, returning a
`wp_keymap_outcome {kind, action_id, inserted_char}`:
- `WP_KEYMAP_RESULT_NONE` -- consumed internally, or a plain character
  insertion (`inserted_char` tells the caller whether to re-stamp the
  current input style onto the newly typed run).
- `WP_KEYMAP_RESULT_ACTION` -- `action_id` is a real
  `platform/common/actions.h` `wp_action_id`, dispatched exactly like
  a menu click (`invoke_action` on both backends: handler-or-backend,
  the same path menu clicks already use).
- `WP_KEYMAP_RESULT_QUIT` / `SAVE_QUIT` -- vi-mode's own two exit
  paths; WordStar/Emacs/WordPerfect map their own quit chords onto
  `QUIT` (MS Word has none at all -- see its own section below).
- `WP_KEYMAP_RESULT_BEEP` -- an invalid/unbound chord, cosmetic.

Engine-level scheme modules (`engine/src/wordstar.c` etc.) cannot
reference `wp_action_id` directly -- `engine/` has zero includes of
anything under `platform/common/`, a real, unbroken layering invariant
confirmed before this milestone began. So each scheme keeps its own
small, hand-authored result enum (`wp_wordstar_action_kind` etc.), and
`keymap.c`'s per-scheme `feed_key_*` function is the one place that
translates it into a real `wp_action_id`.

**Vi-mode's own free pass into `wp_action_find_by_shortcut` does not
extend to any other scheme.** `docs/vimode.md`'s own documented fix
made vi-mode require an *unmodified* key for every plain-letter
command, specifically so `Ctrl+<letter>` app shortcuts (Bold, Save,
Cut, ...) stay reachable underneath it. WordStar/Emacs/WordPerfect/MS
Word all define real historical meanings for much of that same
Ctrl-key space themselves (WordStar's `^B` is "reformat paragraph," not
Bold) -- so `raw_shortcut_byte` is deliberately unused by every
non-vi scheme; each is fully responsible for producing every
`wp_action_id` it wants to expose.

No prefix-key trie or shared state machine exists anywhere -- WordStar
(`^K`/`^Q`/`^P`/`^O`/`^J`) and Emacs (`C-x`/`C-c`) each hand-roll their
own `pending_prefix` field, the same shape `vimode.c`'s own `gg`
handling already used before M10. WordPerfect and MS Word have no
prefix mechanism at all. This mirrors a real, on-record precedent
against building a shared registry ahead of need:
`docs/formats/plugin-architecture.md` designed a vtable for format
modules ahead of need and it was never built ("format_txt is still
called directly by name everywhere").

## WordStar (Phase 1)

The "diamond" cursor cluster (`^S`/`^D` char, `^E`/`^X` line/paragraph,
`^A`/`^F` word) plus arrow keys as an added, deliberate convenience
(most WordStar-era keyboards had none). Five real prefix keys; only
`^K`/`^Q`/`^P` have subcommands implemented.

| Command | Key(s) |
|---|---|
| Char left / right | `^S` / `^D` (or arrows) |
| Line up / down | `^E` / `^X` (or arrows) |
| Word left / right | `^A` / `^F` |
| Line start / end | `^QS` / `^QD` (or Home/End) |
| Document start / end | `^QR` / `^QC` |
| Delete char under cursor | `^G` (or Delete key) |
| Delete char before cursor | Backspace |
| Delete word right | `^T` |
| Delete to end of line | `^QY` |
| Delete current line/paragraph | `^Y` |
| Block: mark begin / end | `^KB` / `^KK` |
| Block: copy / move / delete | `^KC` / `^KV` / `^KY` |
| Save (keep editing) | `^KS` or `^KD` |
| Save & exit | `^KX` |
| Abandon (no save) | `^KQ` |
| Bold / Italic / Underline | `^PB` / `^PY` / `^PS` |

Block marking is a real **marker pair** (`^KB` begin, `^KK` end), not
an anchor+extend selection -- copy/move/delete act on the region
between markers, matching WordStar's own real model (no clipboard
concept). Block Move (`^KV`) is the one genuinely hard operation:
correct regardless of whether the destination is before or after the
source block (delete-first when the destination is earlier, since
nothing before it is touched; insert-first-then-rebase when it's
later, since the block's own markers stay valid until the insert
happens after them) -- refusing, not silently miscomputing, the one
narrow edge a simple paragraph-index rebase can't handle: the
destination landing in the exact paragraph a *multi-paragraph* block's
own end boundary sits in.

**No undo binding exists at all** -- classic WordStar (3.x/4.x) never
had one. Find/Replace (`^QF`/`^QA`) are deferred (their own
interactive text-collection sub-state is real, separate work). `^O`
(onscreen) and `^J` (help) are registered as real prefix letters so
pressing them doesn't fall through as a bare insert, but no
subcommands exist under either.

## Real `WP_MOD_ALT` plumbing (Phase 2)

Emacs's Meta and WordPerfect's Alt-Fn chords both need a real Alt
modifier bit, which neither backend ever actually set before this
phase (the bit existed in `wp_key_mod` since M8.5, just unused).
Atari's `atari_translate_key` gained a `kstate` parameter (AES's own
kstate output, bit 3 = Alt, previously captured and discarded by every
caller); Amiga's `translate_rawkey` now also reads
`imsg->Qualifier`'s real `IEQUALIFIER_LALT`/`IEQUALIFIER_RALT` bits
(0x0010/0x0020 -- verified against the real NDK header, not the
0x0200/0x0400 pair some online references cite). Both apply Alt
uniformly at a single exit point, regardless of which branch produced
the event, so a special key, a Ctrl-chord, and a plain codepoint can
each also be Alt-held.

## Emacs (Phase 3)

| Command | Key(s) |
|---|---|
| Char back / forward | `C-b` / `C-f` (or arrows) |
| Line up / down | `C-p` / `C-n` (or arrows) |
| Word back / forward | `M-b` / `M-f` |
| Line start / end | `C-a` / `C-e` (or Home/End) |
| Buffer start / end | `M-<` / `M->` |
| Paragraph back / forward | `M-{` / `M-}` |
| Delete char forward | `C-d` (or Delete key) |
| Delete char backward | Backspace |
| Kill word forward / backward | `M-d` / `M-DEL` |
| Kill to line end (or merge next paragraph if already there) | `C-k` |
| Set mark | `C-SPC` (see reachability note below) |
| Kill / copy region | `C-w` / `M-w` |
| Yank (paste) | `C-y` |
| Exchange point and mark | `C-x C-x` |
| Cancel pending prefix/meta | `C-g` |
| Save / Open / Quit | `C-x C-s` / `C-x C-f` / `C-x C-c` |
| Undo | `C-x u` |
| Bold / Italic / Underline | `C-c b` / `C-c i` / `C-c u` |

Meta is real Alt (Phase 2) **or** the historical `ESC`-then-key
fallback -- a deliberate departure from every other scheme's own
cancel semantic: bare Escape here means "await the next key as a Meta
chord," not "cancel." `C-g` is Emacs's own real cancel key instead.

**Two bindings deviate from real Emacs for a real, structural reason,
not a choice**: `Ctrl+<non-letter>` chords don't survive this
project's own key translation on either backend intact (only
`Ctrl+<letter>` is reliably recoverable -- Ctrl+Space produces ASCII
NUL, which Atari's own `atari_translate_key` treats identically to a
genuinely unmapped key). Real Emacs's own `C-/`/`C-x C-_` (undo) and
`C-SPC`/`C-@` (set mark) both hit this. Undo sidesteps it entirely by
using `C-x u` instead (only the `C-x` prefix itself needs Ctrl). Mark-
setting has no such alternate in real Emacs -- `C-SPC` is implemented
and fully host-tested (tests construct the `wp_key_event` directly,
bypassing translation), but is **not currently reachable from a real
keypress on either shipped backend**, a real, honestly flagged gap.
Kill/copy-region and `C-x C-x` inherit this same practical limitation
until a future backend revisit.

One kill slot, not a ring: `M-y` (cycle to an older kill) is out of
scope, so a ring of N slots would behave identically to one slot --
`wp_emacs_state.kill_bytes` is a single `mem_alloc`'d slot, matching
`wp_vimode_state.register_bytes`'s own precedent.

## WordPerfect (Phase 4)

Real WP 5.1 is a function-key scheme that's *also* authentically
menu-driven -- most function keys open a numbered on-screen menu, not
a flat command. This phase's own scope decision: ship real F-key/
modifier **bindings**, skip the numbered-menu UI itself entirely (as
much a UI-shape problem as a keybinding one).

| Command | Key(s) |
|---|---|
| Char left/right/up/down | Arrows (Ctrl+Left/Right for word) |
| Line start / end | Home / End |
| Document start | Ctrl-Home (simplified from real WP's own "Go To") |
| Delete to end of line | Ctrl-End (real WP's own binding for this, not document-end) |
| Delete char forward / backward | Delete / Backspace |
| Delete word backward | Ctrl-Backspace |
| Undelete (single-level) | F1 |
| Bold / Underline | F6 / F8 |
| Save | F10 |
| Exit | F7 |
| Block toggle | Alt-F4 |
| Move block (cut to register) | Ctrl-F4 |
| Retrieve (paste register) | Shift-F10 |

Block mode is a real anchor+extend selection (same `wp_selection`
shape Emacs's own mark+point uses) but activated as an explicit
**toggle** -- Alt-F4 turns it on, Alt-F4 again cancels with no
operation performed -- a real, distinct third selection-activation
pattern alongside vi's modal enter/exit and Emacs's set-and-persist.

Real WP's own Copy/Move/Delete 3-way block menu flattens to one
action, Move (Ctrl-F4: delete into a register), with Shift-F10
(Retrieve) completing the workflow at a new cursor position -- pure
Copy and a register-less pure Delete are real WP options this
flattening doesn't expose separately. **F12** (real WP's own alternate
block-toggle key) isn't bindable at all: `wp_special_key`
(`keyevent.h`) only defines F1-F10, a real structural limitation, not
a choice. Italic has no binding, matching real WP 5.1 exactly (it
lived under a Ctrl-F8 menu with no direct key of its own).

An optional follow-up spike -- the authentic nested numbered-menu/
prompt presentation itself, mirroring the AmigaGuide/`.HYP` spike
precedent -- remains explicitly out of this milestone's Definition of
Done.

## Microsoft Word, classic (Phase 5)

By far the lowest-novelty scheme: closest to a modern, familiar,
non-modal editor. Most Ctrl-letter bindings are already
`platform/common/actions.c`'s own `g_wp_actions[]` real shortcut
bytes, so this scheme's job for those is direct pass-through.

| Command | Key(s) |
|---|---|
| Char/line motion | Arrows (Ctrl+Left/Right for word, Ctrl+Home/End for doc) |
| Extend selection | Shift+ any motion above |
| Delete selection, or char/word if none | Delete, Backspace (Ctrl+ for word) |
| Save / Open | Ctrl-S / Ctrl-O |
| Cut / Copy / Paste | Ctrl-X / Ctrl-C / Ctrl-V |
| Select All | Ctrl-A |
| Undo / Redo | Ctrl-Z / Ctrl-Y |
| Bold / Italic / Underline | Ctrl-B / Ctrl-I / Ctrl-U |

Shift+motion is a real anchor+extend selection that Delete/Backspace/
Enter/plain typing all consult and consume -- typing over an active
selection replaces it, matching the familiar behavior most modern
editors share. This is the only scheme in the milestone that gives
selection a consumer beyond an explicit kill/copy keystroke.

Real Ctrl-Z/Ctrl-Y undo/redo -- the only scheme with redo at all
(WordStar has none full stop; Emacs's own `C-/` has no separate redo;
WordPerfect's F1 is a narrower single-deletion Undelete). Classic
Word's own bindings map directly onto this engine's existing full
undo/redo stack.

**Deliberately has no in-app quit keystroke.** Real classic Word has
no standard in-editor shortcut for it either (Alt-F4 is an OS/window-
manager convention), and every backend's own window-close handling
already lets the user quit regardless of the active scheme, entirely
independent of this scheme's own key dispatch. Cut/Copy/Paste route
through the existing whole-document-scoped `WP_ACT_EDIT_CUT/COPY/
PASTE` actions unchanged -- this project's own pre-existing "no
selection-scoped clipboard yet" limitation (`act_edit_copy`'s own
comment in `actions.c`), matching vi-mode's identical existing
behavior for the same three shortcuts today.

## Real bugs found by the test suite (worth not re-deriving)

- **A classic C string-literal footgun** (Phase 1): `\x` hex escapes
  in a C string literal are greedy about trailing hex-digit
  characters. `"\x0bB"` does not mean "byte 0x0b, then the character
  'B'" -- it means the single byte `\x0BB` (0xBB, truncated), because
  `B` is itself a valid hex digit and gets absorbed into the escape.
  `test_wordstar.c`'s own key-script helper hit this writing `^KB`/
  `^KC` test sequences; every affected site now splits into two
  adjacent string literals (`"\x0b" "B"`), a discipline
  `test_emacs.c`/`test_wordperfect.c`/`test_msword.c` all follow
  proactively.
- **An overly broad refusal guard** in WordStar's `move_block` (Phase
  1): the "refuse if cursor shares the block's end paragraph" check
  meant to guard the one hard multi-paragraph edge case instead
  refused *every* single-paragraph move (the common case), since a
  single-paragraph document trivially always has `cursor.paragraph ==
  end.paragraph`. Fixed to only apply when the block itself spans more
  than one paragraph.
- **A vim-vs-Emacs word-motion semantic mismatch** (Phase 3): Emacs's
  `M-f`/`M-d` initially reused `wp_pos_word_forward` (vim's own 'w'
  motion, which skips through trailing whitespace to land at the START
  of the next word) -- but real Emacs's forward-word lands at the END
  of the current word instead. Killing "Hello World" at the start
  incorrectly left "World" (the space eaten too); switching to
  `wp_pos_word_end` fixed it to the real Emacs result, " World". `M-b`/
  `M-DEL` were unaffected (Emacs's own backward-word already lands
  where vim's 'b' does).

## Explicitly deferred (documented gaps, not oversights)

- Find/Replace, in any scheme -- each would need its own interactive
  text-collection sub-state (mirroring vi-mode's own Command-line
  mode), real separate work out of scope for a keybinding-layout
  milestone.
- WordPerfect's authentic nested numbered-menu/prompt UI (see its own
  section above) -- an optional future spike, not required.
- Emacs's `M-y` (cycle to an older kill) and the kill-ring it would
  need instead of one slot.
- Real on-target reachability of Emacs's `C-SPC` (and by extension
  `C-w`/`M-w`/`C-x C-x`) -- implemented and host-tested, blocked by a
  real backend key-translation limitation, not this scheme's own code.
- Deeper multi-level prefixes no scheme in this milestone implements
  (Emacs's own `C-x 4 f`, `M-g g`; WordPerfect's real menu chains) --
  each scheme's own state machine is single-level by design, per its
  own header comment.
