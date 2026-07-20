# Vi-Mode Editing (M8.5)

> Covers the portable, UI-agnostic vi-style modal editor: motion
> primitives (`engine/src/motion.c`), the key-event type
> (`engine/include/keyevent.h`), and the mode state machine
> (`engine/include/vimode.h`, `engine/src/vimode.c`). Built on top of
> `docs/editing.md`'s general editing primitives (paragraph split/merge,
> `wp_edit_delete_selection`, undo grouping) -- read that first.

## Design Decision: Motions Operate on Logical Paragraphs, Not Wrapped Lines

`wp_pos` addresses `(section, paragraph, run, offset)` -- a tree
position with no concept of a visually-wrapped screen row. The layout
cache (`wp_paragraph.layout`, populated by `wp_paragraph_relayout`) is a
lazy, invalidatable *rendering* artifact keyed to a content width that
can change (window resize) or simply not exist yet (`layout == NULL`
until first painted). Building motions against it would mean:

- A motion function needing a live relayout pass just to move the
  cursor, even in a purely engine-side, no-window context.
- Host-testability (this milestone's hard requirement -- see "Vi-Mode
  State Machine" below) breaking outright: a test would need to
  fabricate a window width and force a relayout before asserting a
  cursor position.
- A second, parallel position-addressing scheme alongside the one
  (`wp_pos`) every other engine module already uses.

**Consequence, stated plainly**: `j`/`k` (and everything built on them)
move by logical paragraph, not by soft-wrapped row -- a real, visible
difference from real vim on a long wrapped paragraph. Accepted as a
known gap, not silently hidden.

## Design Decision: Insertion-Point Caret, Not Vim's On-Character Caret

Real vim's Normal-mode cursor always rests *on* a character (never past
the last one on a line; `Escape` from Insert mode moves the cursor back
one column so it lands on a character again). This engine instead
reuses its existing `wp_pos` convention uniformly: the caret may rest
**one-past-the-last-character**, exactly like an ordinary text-editor
insertion point. This was a deliberate scope decision (confirmed with
the user), not an oversight -- it lets every motion function below
reuse `wp_pos_next`/`wp_pos_prev`/`wp_pos_doc_start`/`end` with zero
special-casing, at the cost of small, real behavioral differences from
real vim around `x` at end-of-line, `$`, and where the cursor lands
after `Escape`. Building faithful vim parity here remains possible
later, entirely inside the vi-mode layer -- none of the engine
primitives below would need to change.

## Motion Primitives (`engine/src/motion.c`)

One motion per call -- count-looping (`3w`, `d2j`) is the vi-mode
layer's responsibility, matching how `wp_pos_next`/`prev` are already
single-step and looped by callers.

### Word classification

`wp_wordclass_of(codepoint)`: ASCII alnum + `_` = WORD, space/tab/CR =
BLANK, everything else = PUNCT. Codepoints >= 128 classify as WORD (a
deliberate simplification -- correct-enough text-character treatment,
not a claim of locale-aware word-breaking).

### Word motions: `w`/`b`/`e` (and `W`/`B`/`E` via `big`)

`wp_pos_word_forward`/`backward`/`end`. `big = WP_TRUE` merges
WORD+PUNCT into one class (vim's WORD, blank-delimited only); `big =
WP_FALSE` keeps WORD/PUNCT/BLANK as three distinct classes (vim's
lowercase word).

**A word/punct run never spans a paragraph boundary**, even when the
adjacent paragraph happens to start with a character of the identical
class -- crossing into a new paragraph always ends the current run
(`skip_word_run_forward`/its mirrored logic in `word_backward`/`word_end`
in `motion.c`). This was a real bug caught by
`test_motion_word_forward_crosses_paragraph_boundary`: an earlier
version's shared "skip while same class" helper had no paragraph
awareness and silently merged "Hello" + "World" across a paragraph break
into one giant word purely because both start with a WORD-class
character. **Blank-skipping, by contrast, freely crosses paragraph
boundaries** -- a paragraph break behaves like just another blank,
matching how consecutive blank *lines* get skipped through in real vim.

`wp_pos_word_end` lands **immediately after** a word's last character
(this engine's insertion-point caret convention), not *on* it the way
real vim's `e` does.

**Known, accepted simplification**: an empty paragraph does **not**
count as its own word-stop (unlike real vim, where a blank line is a
distinct `w`/`b` stop). Blank-skipping walks straight through one or
more consecutive empty paragraphs to the next real content. Not
exhaustively fixture-matched against real vim's exact multi-blank-line
stop sequence -- best-effort, not a byte-for-byte parity claim.

### Line motions: `0`/`^`/`$`

`wp_pos_line_start`/`first_nonblank`/`end`. "Line" means the current
logical paragraph (see the decision above). `wp_pos_line_first_nonblank`
on an all-blank (or empty) paragraph lands at that paragraph's end,
matching `^`'s real-vim behavior on a blank line.

### Paragraph motions: `j`/`k`, `{`/`}`, `gg`/`G`

`wp_pos_paragraph_down`/`up` reinterpret `j`/`k` at paragraph
granularity per the decision above -- **not column-preserving** (real
vim remembers a "desired column" across ragged-length lines so repeated
`j`/`k` doesn't visually drift left on shorter lines; deferred here,
purely a vi-mode-layer concern if built later, no engine change
implied). Always lands at the target paragraph's start (offset 0),
clamping at the document's first/last paragraph.

`wp_pos_para_block_forward`/`backward` (`}`/`{`): jump to the next/
previous **empty** paragraph, or document start/end if none exists.
Single-section scope, like everything else in this pass (see below) --
a target search that finds nothing in the current section lands at the
document's start/end rather than continuing into an adjacent section.

`gg`/`G` need no new primitive: `wp_pos_doc_start`/`end` (existing)
plus direct `wp_section_paragraph_get` indexing (already public) cover
both without any new motion function.

## Scope: Single-Section Throughout This Pass

Matches `wp_document_split_paragraph_raw`/`merge_paragraph_raw`'s own
established scope (`docs/editing.md`) -- nothing user-facing produces a
multi-section document yet (`format_txt`/`format_rtf` both only ever
build one section), so cross-section motion/edit was never exercised by
anything real. Not revisited here.

## Portable Key-Event Type (`engine/include/keyevent.h`)

`wp_key_event {codepoint, special, mods}` -- pure value type, zero
platform calls, same category as `wp_pos`/`wp_span` already living in
`docmodel.h`. Each platform backend translates its own native input
(Atari AES `evnt_multi`'s packed key field, Amiga `IDCMP_RAWKEY`, Mac
`KeyDown`, DOS INT 16h) into this before any engine-adjacent code sees
it -- see the Atari backend's own translation for the real, empirically-
verified scan-code mapping (not assumed from documentation alone).

## Vi-Mode State Machine (`engine/include/vimode.h`)

Every behavior below is exercised by `host/tests/test_vimode.c` with
zero real platform backend involved -- synthetic `wp_key_event` scripts
in (via the test-only `feed_keys` helper), resulting document/mode/
cursor state asserted directly. This is what makes the state machine
genuinely host-testable, not just designed to be.

### Command reference (MVP)

**Normal mode** -- `h`/`l`/`j`/`k` (char/paragraph motion), `w`/`b`/`e`
and `W`/`B`/`E` (word motion), `0`/`^`/`$` (line), `gg`/`G` (doc/
paragraph), `{`/`}` (paragraph block), a numeric count prefix on any of
these (`3w`, `5j`); `d`/`c`/`y` operators compose with any motion above
(`dw`, `cj`, `y$`, `2d3w`) and double for whole-line linewise action
(`dd`/`cc`/`yy`, `3dd`); `x` (delete char forward); `i`/`a`/`I`/`A`/`o`/
`O` (enter Insert mode); `p`/`P` (put); `u`/Ctrl+R (undo/redo); `v`/`V`
(enter Visual/Visual-Line); `:` (ex-command); `/` (search forward);
`n` (repeat last search).

**Insert mode** -- any printable codepoint inserts; Enter splits a real
paragraph (closing the M8.5 gap that made Enter a hard no-op before this
milestone); Backspace merges into the previous paragraph at a paragraph
start, otherwise deletes one character; Escape returns to Normal and
closes the undo group.

**Visual / Visual-Line mode** -- any motion extends the selection
(anchor fixed, caret follows); `d`/`x`, `y`, `c` operate on the
selection (Visual-Line always linewise regardless of the motions used
to build it); Escape cancels.

**Command-line mode** (`:` or `/`) -- printable chars append; Backspace
deletes a char or exits on an empty buffer; Enter executes; Escape
cancels. Ex-commands: `:w` (`WP_VIACTION_SAVE`), `:q`
(`WP_VIACTION_QUIT`), `:wq`/`:x` (`WP_VIACTION_SAVE_QUIT`), anything
else beeps. No ex-command arguments (`:w filename`) yet --
`wp_vimode_action.arg` is reserved for that.

### The `:w`/`:q`/`:wq` action hand-off seam

`vimode.c` deliberately does **not** call `plat_file_*` itself, even
though other engine code legitimately could (`platform.h` calls are
permitted in engine code generally -- "zero platform calls" bans native
OS/toolkit headers, not the `platform.h` seam; `services/spell.c` is an
existing example). The reason to still avoid it here: `app_shell.c`'s
existing save/load logic already owns real state (remembered file path,
RTF-vs-plaintext-by-extension dispatch, the Save-As file-selector flow)
that `vimode.c` has no business duplicating or getting out of sync with.
Instead, `wp_vimode_feed_key` returns a `wp_vimode_action` the caller
executes with logic it already has. This module has no "unsaved
changes" concept either -- `:q` always returns `WP_VIACTION_QUIT`
unconditionally; the caller decides whether to interpose a dirty-check
dialog.

### Undo grouping strategy

Entering Insert mode (`i`/`a`/`I`/`A`/`o`/`O`, or the tail end of `cw`)
opens an undo group (`ensure_insert_group_open`) on its first mutating
action if one isn't already open; every subsequent keystroke's edit
pushes inside it; Escape closes it. This makes one `u` after typing a
whole sentence undo the entire session as one step -- only possible
because of `docs/editing.md`'s undo-grouping mechanism. `cw`/`cW`/`c` +
any motion open the group **before** the operator's own delete runs (not
after), so the delete becomes an inner group nested inside the outer
Insert-session group -- confirmed load-bearing, not speculative, by
`test_vimode_cw_undoes_as_one_step`.

### Four real bugs caught by the test suite (worth not re-deriving)

- **`cw`/`cW` are NOT literally `c` + `w`.** Real vim has a documented
  special case (`:help cw`): unlike `dw`, `cw` does not swallow the
  trailing whitespace after a word -- it behaves like `ce`
  (`wp_pos_word_end`), not like a bare `w` motion
  (`wp_pos_word_forward`). Missing this made `cwGoodbye<Esc>` on "Hello
  World" produce "GoodbyeWorld" (no space) instead of "Goodbye World" --
  caught by `test_vimode_cw_change_word`. The special case is scoped
  narrowly, only when `pending_operator == WP_VIOP_CHANGE`; bare `w` and
  `d`/`y` + `w` are unaffected.
- **A linewise register always carries a trailing `'\n'`**
  (`wp_document_extract_text` puts a separator after every paragraph it
  joins, including the last -- see `docs/editing.md`), so `paste_text_at`
  splitting on *every* `'\n'` it sees created one extra empty paragraph
  beyond what was actually yanked. Fixed by only splitting on a `'\n'`
  that has real content after it -- a trailing one is absorbed silently.
  Caught by `test_vimode_yy_p_duplicates_line_below` (`yyp` on a
  2-paragraph doc produced 4 paragraphs instead of 3).
- **Visual-mode motions must chain from the selection's current extent,
  not the position `v` was pressed at.** `resolve_motion` always reads
  from `vs->cursor`; Visual mode was updating `vs->visual.caret` on each
  motion but never `vs->cursor`, so a second `w` in `vwwd` re-resolved
  from the *original* start position instead of continuing from where
  the first `w` had already extended to. Fixed by keeping `vs->cursor`
  synced to `vs->visual.caret` on every Visual-mode motion. Caught by
  `test_vimode_visual_mode_delete`.
- **A `Ctrl+<letter>` chord is not the same thing as its bare letter.**
  Found integrating the Atari retrofit (M8.5 Phase 9): `wp_key_event`
  deliberately reuses a Ctrl-chord's plain letter as `codepoint` (mirrors
  real terminal Ctrl-chord ASCII semantics, and is what makes Ctrl+R
  redo work at all), but `resolve_motion`, `feed_normal`'s digit/`g`-
  prefix/`d`-`c`-`y`-operator/command switch, `feed_insert`'s printable-
  insert check, and `feed_visual`'s digit/operator switch all matched on
  `key.codepoint` alone, with no `key.mods` check. A host embedding vi-
  mode alongside its own `Ctrl+<letter>` shortcuts (Bold, Save, Cut, ...
  -- see `platform/common/actions.c`) would have every one of them
  silently swallowed as a vi-mode command instead: Ctrl+B moved the
  cursor backward a word (`b`), Ctrl+U triggered undo (`u`), Ctrl+V
  entered Visual mode (`v`), Ctrl+X deleted a character (`x`), and so on
  -- none of them ever reached the app's own dispatch. Fixed by requiring
  an unmodified key for every plain-letter command in all three modes
  (the sole deliberate exception, Ctrl+R for redo, already checked
  `key.mods` itself). Caught by
  `test_vimode_ctrl_chord_is_not_bare_letter`, added specifically to
  reproduce and pin this down, rather than discovered by an existing
  test (none of Phase 5's own tests ever fed a modified key to anything
  but the explicit Ctrl+R case).

## Atari Retrofit (M8.5 Phase 9)

`platform/atari/app_shell.c` wires vi-mode, the action registry
(`platform/common/actions.c`), and the status line
(`platform/common/statusline.c`) into the real GEM event loop. Given this
project's own documented history of two real regressions from menu/
dispatch restructuring (see `app_shell_internal.h`'s account), this pass
was deliberately scoped to minimize touching `wp_menu_tree[]`'s hand-
built OBJECT array:

- **Keyboard dispatch is fully unified.** `wp_dispatch_key` translates
  the real AES key (`atari_translate_key`, scan-code-first, verified
  against the Atari Compendium -- see `wp_atari_key_translate_selfcheck`
  in `app_shell.c`) to a `wp_key_event`, feeds it to
  `wp_vimode_feed_key`, and falls through to
  `wp_action_find_by_shortcut` for anything vi-mode doesn't consume
  (every `Ctrl+<letter>` app shortcut). This split is safe by
  construction, not convention, now that vi-mode requires an unmodified
  key for its own plain-letter commands (the bug above).
- **Menu dispatch is unified only where it's a 1:1 semantic match.**
  Bold/Italic/Underline, Edit Copy, and File Quit route through
  `wp_action_find(id)->handler(&g_ctx)` from both the menu (`MN_SELECTED`)
  and keyboard paths now -- one implementation, not two. Font Size and
  Alignment's five existing menu items (Size +/-, Align Left/Center/
  Right) deliberately still call the pre-M8.5 local `change_font_size`/
  `apply_alignment` functions directly, unchanged: the registry's
  `WP_ACT_FORMAT_SIZE`/`FORMAT_ALIGN` actions open a real
  `plat_dialog_choice` picker instead (Phase 7), a real, deliberate UX
  difference this pass intentionally does NOT backport onto the existing
  menu items, since doing so would mean removing items from
  `wp_menu_tree[]` -- exactly the kind of OBJECT-array index-arithmetic
  surgery that caused both prior regressions. The two keyboard shortcuts
  (Ctrl+]/Ctrl+L) DO get the new dialog-based behavior, since that only
  changes a dispatch table, not the tree. File Open/Save, Edit Cut/
  Paste, and the four Tools actions stay backend-supplied
  (`handler == NULL`), called directly by id, matching Phase 6's own
  documented scope.
- **`g_ctx.vi.cursor` is the single source of truth for cursor
  position**, replacing the pre-M8.5 `g_cursor` global; `g_ctx.style_flags`/
  `current_size_twips`/`doc_loaded` replace their `g_current_*`/
  `g_doc_loaded` equivalents the same way. `g_doc`/`g_journal` stay as
  plain globals (`g_ctx.doc`/`g_ctx.journal` just point at them) -- every
  pre-existing call site in this file already spells them `&g_doc`/
  `&g_journal`, and there's no real duplication to remove by renaming
  those specifically.
- **A whole-document swap (Open/Paste/Cut/ODT-open) resets vi-mode**
  (`wp_vimode_free` + `wp_vimode_init` in `replace_document`), not just
  the undo journal -- any Insert/Visual/pending-operator state held for
  the *previous* document means nothing once its tree has been freed and
  replaced.
- **The status line is a reserved bottom strip**, painted after the
  document body on every `redraw_window` call, sized from a fixed 8pt
  font's real `plat_font_metrics_get` (not a hardcoded pixel count).
  `wp_vimode_get_status_text`'s one composed string (mode, plus the `:`/
  `/` buffer in Command mode) fills the view's `mode_label` field;
  `pending_count` (the `3` in `3dd`) has no producer wired up here --
  `wp_vimode_get_status_text` doesn't expose it, and reaching directly
  into `vi.count_pending` to show it without the operator it's paired
  with seemed more confusing than showing nothing. A documented, minor
  gap, not fixed this pass.
- **Insert-mode text still gets the "current input style" toggle
  applied** despite insertion now happening inside `vimode.c`'s
  `feed_insert`, not an app-level `do_insert_char`: `wp_dispatch_key`
  calls `apply_current_style_to_run_at(g_ctx.vi.cursor)` right after any
  keystroke that both started in Insert mode and was a real printable
  character -- matching `feed_insert`'s own exact insertion condition,
  so a bare cursor motion never re-stamps a run it didn't just create.

**Real interactive verification was attempted and is a documented,
inconclusive gap, not a claimed pass.** `RETROWP.PRG` was booted under
headless Hatari and driven with synthetic `xdotool key`/`type` injection
(several variants: `key` vs `type`, short vs long settle delays,
`--clearmodifiers`, explicit X11 focus verification via
`xdotool getwindowfocus`, a bare click into the window per this
project's own documented mouse-injection recipe). Screenshots confirmed
the app boots, the menu and document render correctly (including the
pre-existing, separately-documented "only File title renders" AES bug,
unchanged -- not a new regression), and the new status line renders in
the correct reserved strip with real content. But no injected keystroke
reliably produced a visible character in the document across any
variant tried, so the real AES-`evnt_multi`-to-`wp_dispatch_key` path
was not demonstrated end-to-end interactively. This is consistent with,
and reinforces, `docs/atari-platform.md`'s existing T4.3 caveat about
unreliable `xdotool` keystroke fidelity in this exact Hatari/SDL/X11/
Xvfb stack. Given the real environment gap, this pass relies instead on
`wp_atari_key_translate_selfcheck` (rigorous, hand-verified-against-the-
Compendium coverage of `atari_translate_key` itself, run through
`ci_atari.sh`/`ci_atari_matrix.sh` on real Hatari) plus the full 196-test
host suite (including `test_vimode_ctrl_chord_is_not_bare_letter`) for
confidence in the dispatch logic. Closing this gap for real -- a genuine
interactive keystroke-to-character screenshot -- needs either a real
physical Atari/better-behaved emulator input path, or investigating a
proper window manager / Hatari's own remote-control socket for
guaranteed-delivered synthetic input, neither attempted here.

## Explicitly Deferred (documented gaps, not oversights)

Dot-repeat (`.`) -- needs keystroke-log replay against a new cursor/
context, a different mechanism from undo-stack replay; deferred
regardless of the undo-grouping work done here. Macros (`q`/`@`), named
registers (`"a` through `"z` -- only one unnamed register exists),
marks, jumplist (Ctrl-O/Ctrl-I). Regex/case-insensitive search, `:s///`
substitute, ex-command arguments and ranges (`:1,5d`, `:w filename`).
Backward search (`?`) and `N` (repeat search backward) -- `wp_vimode_search`
takes a `forward` parameter for this future extension but currently
ignores it. Visual-Block (Ctrl+V); Visual mode's `>`/`<`/`~`/`r`
(indent, case-toggle, replace-selection) -- plain `d`/`y`/`c` on a
selection are in scope, they're `wp_edit_delete_selection`'s direct
payoff. Column-preserving `j`/`k` (virtual-column memory across ragged
paragraph lengths). Cross-section paragraph merge/delete (matches
`docs/editing.md`'s own scope limit). Full vim on-character caret
parity (a confirmed, deliberate scope decision, not a gap -- see above).
