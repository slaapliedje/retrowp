# Help System (M8.5 Phase 10)

## Format: a small AmigaGuide-shaped subset

`docs/help/*.help` files are plain UTF-8 text, line-oriented, parsed by
`platform/common/help_doc.c`:

```
@node NodeName "Display Title"
Body text, any number of lines.

{Link Label|TargetNodeName}
@endnode
```

- `@node NAME "Title"` starts a node. The quoted title is optional --
  a node with no `"..."` falls back to using its name as the title, so
  every node always has a non-empty display title. `NAME` must not
  contain whitespace (it's read as the next space-delimited token).
- `@endnode` ends the current node. A node still open at end-of-source
  is implicitly closed rather than treated as an error.
- A body line whose *entire trimmed content* is `{label|target}` is a
  link, not text: pressing Enter on it (in the pager) jumps to the node
  named `target`. Anything else -- including a `{...}` with no `|`
  separator -- is plain text.
- A blank line is preserved as one empty row (paragraph spacing), not
  collapsed away.
- Anything before the first `@node` (an `@database` header line, blank
  lines, comments) is silently skipped.

This is deliberately close to AmigaGuide's own real syntax (`@node`/
`@endnode` are AmigaGuide's actual keywords) but simplifies its inline
link markup: AmigaGuide writes `@{"label" Link target}`; this format
writes `{label|target}`. The simplification is shallow on purpose -- a
future AmigaGuide export only needs to transform link lines
mechanically (strip the braces, quote the label, insert `Link`), not
redesign anything about node/link structure. See "The `.HYP` research
spike" below for the same claim verified against real ST-Guide syntax,
not just AmigaGuide's.

Parsing is deliberately lenient (matches `format_txt_import`/
`format_rtf_import`'s own established stance): this is authored content
a human edits by hand, not a user-facing document format needing
round-trip fidelity or strict validation. A missing `@endnode`, stray
top-level text, or an empty source all degrade gracefully rather than
failing to load.

## Storage: reusing `wp_strpool`

All text (node names, titles, line text, link labels/targets) lives in
one `wp_strpool` (`engine/include/strpool.h`), referenced via `wp_span`
the same way the engine's own document model stores run text -- reused
directly rather than reinvented, since a help doc's storage need
(append-only UTF-8 bytes, referenced by offset+length) is identical in
shape. `platform/common/` is allowed to depend on `engine/include/`'s
public headers (not `engine/src/`'s private ones); the array-growth
policy (`node_table_grow`/`line_table_grow` in `help_doc.c`) mirrors
`engine/src/growth_internal.h`'s own 1.5x geometric policy but is its
own small private copy, since that header is explicitly not part of
the public engine surface.

## The pager (`platform/common/help_view.c`)

A simple, full-screen, scrolling, read-only viewer -- deliberately
**not** built on `wp_vimode_state`: a read-only browsing widget
(motion, "follow this link", "go back") has no Insert/Visual/Command-
line concept, so reusing vi-mode's state machine here would be a
category error, not a reuse win. It gets its own small state
(`wp_help_view_state`) and its own `wp_key_event`-driven input handler,
reusing only the portable key-event *type* Phase 4 already defined.

**Navigation**: Up/Down (or k/j) move a "selected link" index among the
current node's link-kind lines only; Enter follows the selected link,
pushing the current node onto a small fixed-size history stack;
Backspace pops it ("back"); Page Up/Down scroll by a fixed approximate
page (the view has no idea how many rows the viewport actually fits
until paint time, so this is an approximation, not synced to the real
page size); Escape or `q` requests the caller close the viewer.

**Deliberately not done**: auto-scrolling to keep the selected link on
screen. Doing so needs the same wrap-layout knowledge only
`wp_help_view_paint` currently computes, and only at paint time once
viewport width is known. Help content authored for this project is
short enough per node that this is a low-cost simplification; Page
Up/Down remain available to reach a link scrolled out of view. Also not
done: rendering the current node's own title anywhere in the pager (the
view starts straight into body text) -- a real, minor, cosmetic gap,
not fixed this pass.

### Word-wrap and the twips/pixel boundary

`wp_help_view_paint` takes `content_width_twips` (for wrap-width
comparisons) *separately* from `x_px/y_px/w_px/h_px` and
`line_height_px` (for actual drawing position). This isn't incidental
complexity -- `plat_measure_text` (the only way to measure real glyph
widths, CLAUDE.md rule 4) always reports widths in **twips**, and
`platform/common/` has no twips↔pixel conversion of its own (that ratio
is real-DPI-dependent and backend-specific, e.g.
`platform/atari/metrics_atari.c`'s `atari_twips_to_px_x/y`). The caller
(`paint_help` in `platform/atari/app_shell.c`) does that conversion
itself, the same way `statusline_height_px()` already does for the
status bar's own height -- `platform/common/` code is never the one
doing twips↔pixel math, only the backend is.

The wrap algorithm itself (`tokenize_line` + `pack_next_row`, exposed
together as `wp_help_wrap_line`) mirrors the SHAPE of
`engine/src/linebreak.c`'s own tokenize-then-greedily-pack algorithm
(pre-measure each word/space token once, then accumulate known widths
rather than repeatedly re-measuring a growing candidate substring) --
simplified for this use case: single font (no run/style spanning),
plain ASCII-space word boundaries only (help content is authored plain
text, not arbitrary user prose needing Unicode word-break rules). A
single word wider than the available width still gets its own row
rather than looping forever or producing zero rows (the same single-
overlong-word allowance `linebreak.c`'s own docs/layout.md documents).
Bounded by fixed stack buffers (`WP_HELP_MAX_WRAP_TOKENS`,
`WP_HELP_MAX_WRAP_ROWS_PER_LINE`) -- no dynamic allocation anywhere in
the paint path, generous enough for any realistic authored help line.

Exposed as its own testable function (rather than kept `static` inside
`paint()`) specifically so its correctness is provable directly:
`host/tests/test_help_view.c` asserts exact, hand-computed wrap points
against the host backend's deterministic fake-monospace
`plat_measure_text` (the same precedent `test_layout_greedy_wrap_hand_
computed_fixture` set for the main engine's own wrap code), and
`wp_atari_help_selfcheck` (`platform/atari/app_shell.c`, run via
`ci_atari.sh`/`ci_atari_matrix.sh` on real Hatari) re-proves it against
REAL Atari VDI-measured font metrics -- deliberately not just trusting
that host's simplified round-number metrics generalize.

## Delivery: compiled in, not read from disk

Unlike the optional spell dictionary (`C:\WPSPELL.DIC`, a real runtime
file dependency with a documented "missing file degrades gracefully"
path), help content ships **compiled into every target**. A new host
tool, `host/tools/gen_help_header.c`, converts a `.help` source file
into a generated C header holding its raw bytes as a plain numeric
array (sidesteps C string-escaping entirely -- no quote/backslash/
control-character handling to get right) plus a length constant. The
Makefile runs this once per build (`$(GENERATED)/help_retrowp.h` from
`docs/help/retrowp.help`) and both the host test build and the Atari
cross-compile `#include` the result as ordinary generated source.
`docs/help/retrowp.help` stays the single, real, human-editable source
of truth -- nothing is hand-duplicated as an escaped string a
contributor has to remember to keep in sync.

`test_help_content.c` parses the REAL shipped array (not a synthetic
fixture) and confirms every `{label|target}` line's target resolves to
a real node -- a genuine authoring mistake (a typo'd target, a missing
`@endnode`) fails the build, rather than being discoverable only by
clicking through the real viewer.

## Atari wiring

`platform/atari/app_shell.c` parses the embedded content once at
startup (`g_help_doc`/`g_help_doc_loaded`). Help is a whole separate UI
mode, `g_help_active`: while set, `wp_dispatch_key` routes every key to
`wp_help_view_feed_key` instead of vi-mode, and `redraw_window` paints
`paint_help()` (the pager plus a small hint bar reusing
`wp_statusline_paint` -- generic by design, "usable by anything that
wants a labeled status strip, not hard-wired to vi-mode specifically",
per its own header comment) instead of the document body.

**F1 only, no Help menu item this pass** -- the same reasoning as the
Atari retrofit's own scope decision (docs/atari-platform.md): adding a
new top-level menu title would mean touching `wp_menu_tree[]`'s
hand-built `OBJECT` array, the exact kind of index-arithmetic surgery
that caused two real, silent regressions on other TOS/EmuTOS versions
earlier in this project's history. F1 needed its own special case in
`wp_dispatch_key` regardless of the menu question: it's a
`wp_special_key` (`WP_KEY_F1`), and the action registry's
`shortcut_ascii` field can only represent an ASCII byte, not a special
key -- `WP_ACT_HELP_CONTENTS`'s `key_hint` already said `"F1"` since
Phase 6, but nothing could actually dispatch it until this phase.

**Confirmed live on real Hatari**: F1 opens the viewer, body text
word-wraps correctly, and the link list (with its `> ` marker prefix)
and hint bar all render -- see the screenshot referenced from this
session's own record. This was the one piece of Phase 9/10's UI work
that a live interactive test actually confirmed end-to-end, once a test
harness mistake (see below) was found and fixed. Link-selection
*highlighting* specifically (pressing j/k to visibly move the gray
highlight between links) was not independently reconfirmed live in the
same pass -- the keystroke didn't register in that attempt, most likely
the same `xdotool`-into-headless-Hatari fidelity gap documented in
`docs/atari-platform.md`'s T4.3 section, not a demonstrated bug. Covered
instead by `test_help_view_link_selection_clamped_at_bounds` and the
paint crash-safety test.

*Test-harness note*: the live F1 verification above only worked once a
wrong assumption was corrected -- Hatari's F11 key is "toggle
fullscreen", not "take a screenshot" (the actual hotkey is `Alt_R+g`,
i.e. AltGr+g, confirmed from `man hatari`). An earlier attempt using F11
as the screenshot trigger produced confusing, inconsistent-looking
results (the window's own fullscreen state toggling mid-test) that were
initially, incorrectly suspected to be a real rendering bug.

## The `.HYP` research spike

The plan called for a real spike -- not an assumption -- into whether
this project could realistically produce real Atari ST Guide (`.HYP`)
files, before committing engineering time to it. `HCP.TTP` is the
historical ST Guide compiler; **HypView** (github.com/th-otto/hypview,
GPL, C + autotools) maintains a modern, still-developed reimplementation
called `hcp`.

**What the spike did**: cloned HypView, ran `./autogen.sh && ./configure
--disable-nls && make -C hyp hcp` -- a clean build using only tools
already present (`autoconf`/`automake`/`libtool`/`pkg-config`, `glib-2.0`
already installed), no special setup. Confirmed the real native ST-Guide
source syntax directly from `hcp`'s own compiler source
(`hyp/hcp_comp.c`): `@node <name> [<title>]` and `@{"text" link <name>
[<line>]}` -- both close enough to this project's own `{label|target}`
format that translating one to the other is a genuinely mechanical,
per-line transform (strip braces, quote the label, insert `link`), not
a redesign, exactly as `docs/help/`'s own format was designed hoping
for. Hand-wrote a trivial fixture using that real syntax and compiled
it: `hcp fixture.hyp -o retrowp_test.hyp` exited 0, and `file` correctly
identified the output as an "ST-Guide file". Built `hypinfo` (also part
of HypView, a separate introspection tool) from the same source tree and
used it to independently read the compiled file back:

```
@database: RetroWP
:hcpversion: 6
:totalnodes: 4
:nodes: 3
```

-- confirming a genuinely well-formed ST-Guide database, not just an
exit code of 0.

**Real result: worth pursuing as a dedicated follow-up, not attempted
further in this MVP pass.** The spike's job was to answer "is this
worth investing in" before spending real engineering time on it -- the
answer is yes, a real host-buildable, working compiler exists and
accepts a syntax this project's own format was already designed to
translate to easily. What's still real, separate work, explicitly not
done here: a `{label|target}` → hcp-syntax translator (comparable in
size to `gen_help_header.c`), wiring `hcp` as a Makefile-invoked build
dependency the same way `gen_help_header` already is, and -- following
this project's own "verify against the real target" discipline -- an
actual interactive click-through in a real ST Guide viewer (a `.ACC`
desk accessory, not available in this pass's environment) or EmuTOS's
own hypertext viewer, not just structural validation via `hypinfo`. If
that follow-up is picked up: the universal pager (this file's own
subject) is the correct, permanent fallback for every platform that
doesn't get a native export, not something `.HYP` export would replace.

## Explicitly deferred (documented gaps, not oversights)

AmigaGuide export (low risk when Amiga's own port starts -- the format
is plain text, no compiler needed, the closest mechanical translation
of the three native formats this project might eventually target).
Mac/DOS help delivery (no research performed yet). Real `.HYP` export
(see the spike above -- a real, evidenced "yes, do this" recommendation,
not done in this pass). A Help menu item (F1 only -- see "Atari
wiring"). Rendering the current node's title in the pager. Auto-scroll-
to-selected-link. Named search/full-text search across the whole help
database (only per-node browsing exists).
