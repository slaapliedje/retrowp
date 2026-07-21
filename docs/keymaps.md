# Switchable Keybinding Layouts (M10) + Find & Replace (M11)

## Overview

RetroWP's editing was vi-only through M8.5. M10 makes the keybinding
scheme itself switchable: WordStar, WordPerfect (5.1-era DOS), Emacs,
and classic Microsoft Word now sit alongside vi-mode, selected once at
startup from a small config file and fixed for the process lifetime --
no live switching, matching the plan's own explicit scope. Every scheme
stays 100% keyboard-usable; nothing added here needs a mouse.

M11 closes the one gap M10 explicitly deferred in all five schemes:
Find and Replace, each with its own real, historically-verified prompt
flow rather than one generic mechanism bolted on everywhere -- WordStar's
`OPTIONS?` letters, WordPerfect's F2-terminated fields, Emacs's genuinely
live isearch, and MS Word's keyboard-adapted dialog are each a different,
real shape, not the same UI reskinned five times. The shared plumbing all
five build on is a single promoted engine primitive,
`wp_document_find_text` (`engine/include/editing.h`, `engine/src/
search.c`) -- forward/backward, optional case-fold, optional whole-word,
wraps the document boundary at most once -- plus `wp_document_
match_selection` for turning a found match into something
`wp_edit_delete_selection`/`wp_edit_paste_text` can act on. vi-mode
itself gained the pieces its own `/` search was still missing: `?`
(backward), `N` (repeat, reversed), and `:s/old/new/flags` substitute.

Each scheme is verified against real primary sources, not recalled from
memory: the WordStar 3.0 Quick Reference Card and Reference Manual, the
real WordPerfect Workbook for IBM PC, the current GNU Emacs Manual, and
(for M11's own MS Word research) the Word 1.0-for-DOS manual alongside
modern Word's own documented Ctrl+F/Ctrl+H/Shift+F4 convention. Every
deliberate simplification is documented in the relevant engine header's
own file comment (`engine/include/wordstar.h`, `emacs.h`,
`wordperfect.h`, `msword.h`, `vimode.h`) and summarized again below --
this file is the narrative account; those headers are the source of
truth for exact bindings.

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
| Find | `^QF` |
| Find and Replace | `^QA` |
| Find/Replace again | `^L` |

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
had one. `^O` (onscreen) and `^J` (help) are registered as real prefix
letters so pressing them doesn't fall through as a bare insert, but no
subcommands exist under either.

**Find/Replace (M11)**: `^QF` prompts `FIND?` then `OPTIONS?`; `^QA`
prompts `FIND?` then `REPLACE WITH?` then `OPTIONS?`. Real `OPTIONS?`
letters implemented: `G` (global -- restart the scan at document
start/end instead of the cursor), `N` (no confirm -- replace every
remaining match silently), `B` (backward), `U` (ignore case), `W`
(whole word). `^QF` is a single jump (real WordStar finds one
occurrence and stops); `^QA` iterates every match in scope, asking a
real `REPLACE (Y/N)?` at each one unless `N` was given. `^L` repeats
whichever ran last with identical find/replace text and options --
no separate "last search" copy exists; the live prompt state simply
isn't cleared between invocations, so `^L` continues from the cursor's
own current, already-advanced position. A real numeric repeat-count
option (`OPTIONS?` also accepts a leading count) exists in real
WordStar but is deliberately not implemented here.

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
| Incremental search forward / backward | `C-s` / `C-r` |
| Query-replace | `M-%` |

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

**Find/Replace (M11)**: `C-s`/`C-r` are genuinely incremental -- every
keystroke re-searches live, no Enter needed, the one scheme in this
milestone whose Find isn't a collect-then-submit prompt. Forward
isearch lands point at the END of the match, backward at its START --
a real, verified Emacs asymmetry. Typing extends the pattern and
always re-searches from the anchor (where `C-s`/`C-r` was first
pressed); pressing `C-s`/`C-r` again with no new typing instead
advances past the last match, and pressing the *other* key mid-search
flips direction, both matching real Emacs. A failing search leaves
the cursor at the last successful match rather than resetting --
real "Failing I-search" leniency, still live and typeable. `C-g`
aborts back to the anchor; `RET`/`Escape` accept the current position.
`M-%` always pauses at the first match (unlike WordStar's own upfront
"confirm every match, or none" choice) with the real `y`/`SPC`
(replace+advance), `n`/`DEL` (skip+advance), `!` (replace all
remaining), `.` (replace+stop), `q`/`RET` (quit) vocabulary -- the
fuller real set (`,`/`^`/`C-r`/`C-w`/`C-l`/`C-h`: replace-then-ask,
back up, recursive edit, redisplay, help) is deliberately out of
scope, the same narrower-subset treatment the kill-ring already got.
Forward-only from point to the end of the document -- real stock `M-%`
has no reverse variant or G-equivalent restart-from-top.

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
| Search forward / backward | F2 / Shift-F2 |
| Replace | Alt-F2 |

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

**Find/Replace (M11)**: `F2` prompts `-> Srch:`; `Shift-F2` prompts
`<- Srch:`; `Alt-F2` prompts `w/Confirm? (Y/N)` then `-> Srch:` then a
"Replace with:" field, each field terminated by pressing `F2` itself
rather than Enter -- real WP's own Search key is heavily overloaded
(start a search, submit a field, AND repeat the last search verbatim).
That overload is exactly why the search/replace text buffers are
deliberately **never cleared** on entering their own field, unlike
every other scheme's always-fresh prompt: an untouched `F2` press right
after entering the field submits whatever was there from the previous
search, the real "press +Search twice to find the second occurrence"
behavior. Replace iterates every match from the cursor to the end of
the document (no G-equivalent restart-from-top exists in this scheme --
no source checked this pass documents one), confirming each with a real
`Replace? (Y/N)` unless `w/Confirm?` was answered `N`, which replaces
every remaining match silently. No case-fold/whole-word toggle exists
in this scheme's own Search prompt either, unlike WordStar's real
`OPTIONS?` letters.

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
| Find | Ctrl-F |
| Replace | Ctrl-H |
| Repeat last Find | Shift-F4 |

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

**Find/Replace (M11)**: `Ctrl+F` is a single jump with natural document
wraparound (matching vi's own `/`); `Shift+F4` repeats the last
executed Find verbatim -- the real Word binding for "repeat last Find
or Go To action." `F3` is deliberately **not** used for this despite
being a common assumption carried over from other apps/browsers: real
classic Word's own `F3` is AutoText/glossary expansion, an unrelated
feature. `Ctrl+H` collects both fields (Enter-terminated, real Word's
own dialog convention, unlike WordPerfect's F2-terminated fields, and
always starts empty rather than real Word's actual field-remembers-
last-value behavior -- `Shift+F4` already covers repeat on its own),
then jumps to the first match and pauses, standing in for real Word's
persistent Find-and-Replace dialog: `r` replaces and advances (real
"Replace"), `a` replaces this and every remaining match silently (real
"Replace All"), `n` skips and advances (real "Find Next"), `Escape`
closes. Forward-only, cursor to end of document, no G-equivalent
restart-from-top, matching WordPerfect's/Emacs's own narrower scope.

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
- **A stale-incremental-build gotcha, not a source bug** (M11 Phase 0):
  this project's Makefile doesn't track header dependencies. Growing
  `wp_vimode_state` (the new substitute fields) changed `wp_app_ctx`'s
  own size -- which every file embedding it by value (through the
  `wp_keymap_state` union) needs to agree on -- but an incremental
  rebuild didn't recompile every one of those, producing a genuine
  `__stack_chk_fail` crash in a completely unrelated test. `make clean`
  before trusting any test run after a struct-layout change is the
  discipline; a clean rebuild reproduced nothing. Worth remembering for
  any future change to a struct this widely embedded.
- **`wp_document_find_text`'s "strictly after `from`" contract bit
  test fixtures repeatedly, not the implementation** (M11, hit in
  vim's/WordStar's/WordPerfect's/MS Word's own test drafts -- Emacs's
  own tests avoided it proactively once the pattern was recognized): a
  cursor sitting exactly on a match's own start byte is skipped by
  design (the same real vim `/` semantic the shared primitive
  inherits) -- a "foo foo foo" fixture with the default doc-start
  cursor was silently testing the SECOND occurrence, not the first.
  Fixed by adding a short distractor prefix (`"zzz foo foo foo"`) to
  the affected fixtures rather than changing the primitive's own
  well-established, intentional behavior.

## Explicitly deferred (documented gaps, not oversights)

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
- **M11's own real, narrower-scope Find/Replace gaps**: no regex in any
  scheme (`wp_document_find_text` is byte-literal by design); WordStar's
  own numeric repeat-count `OPTIONS?` extra; Emacs's fuller
  query-replace vocabulary (`,`/`^`/`C-r`/`C-w`/`C-l`/`C-h` --
  replace-then-ask, back up, recursive edit, redisplay, help); no
  case-fold/whole-word toggle in WordPerfect's or MS Word's own Search/
  Find prompts (WordStar's `U`/`W` options don't carry over); no
  G-equivalent "restart from document top" in WordPerfect/Emacs/MS
  Word's own Replace (WordStar's `G` option doesn't carry over either --
  each scheme's own real historical scope, not homogenized).
