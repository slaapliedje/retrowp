# Atari ST Platform Backend (M4)

`platform/atari` is the first real (non-stub) `platform.h` backend, and per
CLAUDE.md's architecture layers it also owns the actual UI: the AES/GEM
application shell, not just the platform.h function bodies. This document
covers the toolchain, the design decisions specific to hand-coded GEM
programming, and the CI recipe for testing under Hatari -- plus the honest
gaps in each.

## Toolchain

- Compiler: `m68k-atari-mint-gcc` (MiNT cross toolchain), targeting plain
  TOS (not FreeMiNT-specific features). The default build (no `-mcpu`/`-m68020-60`
  flags) already targets a real 68000 -- correct for a 1MB ST baseline.
- **No `-mshort`.** `-mshort` (16-bit `int`) is a separate, ABI-incompatible
  multilib (`libgem16.a` is a symlink to it) requiring every linked object
  and library to agree. This project's `types.h` already uses fixed-width
  types (`u32`/`i32` == `long`, unaffected by `-mshort` either way), so
  there is nothing to gain and an easy way to corrupt the ABI by linking
  a `-mshort` object against the default `libgem.a`. Build with default
  `int` throughout.
- `-lgem` is both the AES and VDI binding library; no separate `-lvdi`.
- No resource editor or `.rsc` files exist anywhere in this toolchain (no
  ResourceMaster/Human68k-style tool). For UI this small (a one-title menu
  bar today; File/Edit menus as T4.3 adds Open/Save/Cut/Copy/Paste), a
  hand-built `OBJECT[]` array is the practical choice. `rsrc_load()` would
  be the better tool if this ever needs real multi-field dialogs.
- **`-O2` is required for `host/tools/atari_smoke.c` to work at all**, not
  just for code size. See "Native Features gotcha" below.

## Hand-built menu OBJECT tree

`ob_next`/`ob_head`/`ob_tail` in `OBJECT` are **array indices**, not
pointers. The convention (verified against a real running AES under Hatari,
and cross-checked against EmuTOS's own desktop menu tree source):

- `ob_head`/`ob_tail` on a parent bound its child sibling chain (first/last
  child index).
- `ob_next` on a non-last sibling is the next sibling's index.
- `ob_next` on the **last** sibling in a chain wraps back to its own
  **parent's** index -- there is no `-1` terminator inside a chain.
- Only the true root (no parent) uses `ob_next = -1`.

`menu_bar()` does **not** compute `ob_x`/`ob_y`/`ob_width`/`ob_height` from
string content -- those must be set by hand, in character-cell units
relative to the parent (screen-relative only for the root). This is the
single easiest thing to get silently wrong writing a GEM menu tree from
memory; `wp_menu_tree_selfcheck()` (`platform/atari/app_shell.c`) checks the
linkage explicitly rather than trusting it, and the CI smoke test asserts
`menu_bar()` returns success on the real, live tree.

`OBSPEC` string fields use `.free_string` (a plain `char *`) for hand-built,
non-resource objects. C89 has no designated initializers, so the union in
the static `OBJECT[]` literal is left `{0}` and `.free_string` is patched at
runtime (`wp_menu_tree_init()`) before `menu_bar()` is called.

## Native Features (NF_STDERR / NF_EXIT) gotcha

The CI smoke test (`host/tools/atari_smoke.c`) uses Hatari/ARAnyM's Native
Features illegal-opcode trap (`.word 0x7300` / `0x7301`) to print to the
host's stderr and make the Hatari *process* exit with a pass/fail status
code, so `host/tools/ci_atari.sh` can assert on `$?` the same way
`ci.sh` does for the host suite.

**This silently breaks at `-O0`.** The trap functions must be marked
`__attribute__((noipa))` (so GCC can't inline/clone them away) *and* the
value passed to the trap must appear in the asm's input operand list (so
GCC can't prove it's unused and drop the argument push). Even with both of
those, a build without optimization was observed to make `nf_get_id()`
silently return 0 ("feature not found") -- not crash, not hang, just
silently skip every NF call, which looks identical to "everything after
this line didn't run" if you're not looking closely. Root-caused during
T4.1 development by bisecting a series of shrinking reproductions down to
"same code, `-O0` vs `-O2`, only `-O2` reaches the NF trap." The Makefile's
`ATARI_STRICT_CFLAGS`/`ATARI_RELAXED_CFLAGS` both now always include `-O2`
for this reason -- do not remove it for the smoke test build.

This trap executes a genuinely illegal 68000 opcode. It is test-only code
that must never ship in `WP.PRG` (it doesn't -- only `host/tools/atari_smoke.c`
uses it) and would crash on real hardware or under plain TOS without an
emulator's NatFeat interceptor.

## CI recipe (`host/tools/ci_atari.sh`)

```
Xvfb :97 -screen 0 1280x800x24 &
HOME=<scratch dir> DISPLAY=:97 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  hatari --natfeats on --tos /usr/share/hatari/etos512k.img --machine st \
  --memsize 1 --monitor rgb --tos-res low --zoom 4 --statusbar no \
  --borders no --confirm-quit no --sound off --conout 2 \
  --harddrive <scratch>/c_drive --auto SMOKE.PRG
```

Gotchas that cost real time to isolate, worth not re-deriving:

- **`SDL_VIDEODRIVER=dummy` hangs Hatari indefinitely** (stuck after VDI
  init, no error) in this environment. A real X display is required even
  though nothing needs to be *seen* -- Xvfb is sufficient. `--zoom 4` with
  `--tos-res low` (320x200) exactly matches a 1280x800 Xvfb screen; this
  match doesn't appear to be load-bearing for the hang, but wasn't
  independently varied once a working combination was found.
- **`--auto <name>` needs the in-emulator path exactly as `man hatari`
  says** ("full path it will have under emulation, e.g. `C:\DIR\PROGRAM.PRG`").
  A bare filename combined with `--harddrive` silently creates the virtual
  autostart INF entry (visible in `--log-level debug` output) but the
  desktop never acts on it in this EmuTOS/Hatari combination -- the
  program never runs and Hatari sits at a live, idle desktop forever. The
  positional-argument shortcut (`hatari [flags] /path/to/PROGRAM.PRG`,
  which derives `-d` and `--auto C:\PROGRAM.PRG` automatically) reliably
  works; `--auto SMOKE.PRG` (bare name, no drive/path) *also* reliably
  works once the rest of the recipe above is right. This was not fully
  root-caused (EmuTOS-desktop-vs-real-TOS-desktop INF compatibility is the
  leading theory) -- treat the exact flag combination above as verified,
  not the general mechanism.
- **AUTO-folder placement (`C:\AUTO\*.PRG`) is the wrong mechanism for a
  GEM/AES program** and will hang: programs in `AUTO` run before the
  desktop/AES process exists, so any AES call (`appl_init()` included)
  blocks forever waiting for a peer that isn't there yet. This is only
  correct for pre-desktop utilities, not for anything using `<gem.h>`.
- `etos512k.img` (EmuTOS, GPL) is used deliberately over the genuine
  Atari TOS ROM dumps also present on this machine, to keep CI
  redistribution-clean.

### Known gap: no real mouse-driven end-to-end test

Scripted mouse *steering* (moving the emulated pointer to an arbitrary
target coordinate via `xdotool mousemove`) is unreliable in this headless
Xvfb setup -- absolute moves were observed to only ever affect the X
coordinate, with Y consistently stuck at 0, both via this project's own
testing and independently via a prior spike investigation. Root cause not
found (SDL relative-mouse-mode interaction is the leading theory) and not
pursued further given the cost already sunk relative to the value of a
click-driven test over the recipe above, which already exercises the real
AES calls end-to-end.

Given that, `atari_smoke.c` proves two things separately instead of one
mouse-driven click sequence proving both together:
1. The real AES calls succeed for real: `appl_init`, `v_opnvwk`,
   `menu_bar(MENU_INSTALL)`, `wind_create_grect`, `wind_open_grect` are all
   called live and their return codes checked.
2. The message-dispatch logic (`wp_dispatch_message()` in
   `platform/atari/app_shell_internal.h`) is correct, proven by calling it
   directly with synthesized `WM_REDRAW`/`MN_SELECTED`/`WM_CLOSED` message
   arrays and checking its quit/no-quit return value.

What this does *not* prove: that a real mouse click on the real rendered
"Quit" menu item, routed through a real `evnt_multi()` call, delivers the
`MN_SELECTED` message with the item index this code expects. That
end-to-end path is visually consistent with correct (see `WP.PRG`'s own
window rendering with working mover/closer/sizer widgets and the AES menu
bar showing the running app's name), but is not exercised by an automated
test. If mouse scripting becomes reliable in a future environment, closing
this gap is the natural next hardening step, not a redesign.

## VDI text metrics + glyph draw (T4.2)

`platform/atari/metrics_atari.c` implements `plat_font_open/close`,
`plat_font_metrics_get`, and `plat_measure_text` over VDI; `draw_atari.c`
implements `plat_draw_text`, `plat_fill_rect`, `plat_gc_flush`, and the
UTF-8/native charset conversion pair.

**Real per-axis device DPI (revisits T4.2, post-M8).** T4.2 originally shipped
a fixed `ATARI_ASSUMED_DPI = 96` on the premise that there was "no legitimate
way to query a real physical screen DPI without a GDOS install." That premise
was wrong, and was corrected once a real SpeedoGDOS 5.7 install (from the
user's own original floppies) was available to investigate against: real
per-axis DPI comes from `v_opnvwk()`'s own `work_out[3]`/`work_out[4]`
(pixel width/height in microns) -- a **standard VDI open-workstation output,
not a GDOS-specific one**. Verified empirically on a real Hatari + EmuTOS
boot with *no* GDOS driver attached at all: `work_out[3]=338`,
`work_out[4]=372` microns at 320x200 low-res, giving `DPI_X=75`,
`DPI_Y=68` -- physically sensible and consistent with the ST low-res
screen's known non-square pixel aspect (pixels taller than wide). `vq_gdos()`
returned `0` and `vq_vgdos()` returned `GDOS_NONE` (`-2`) in that same run,
confirming the query needs no GDOS at all.

Because X and Y pixels are *not* square on a real ST, `metrics_atari.c` now
queries and converts each axis separately: `atari_twips_to_px_x/_y` and
`atari_px_to_twips_x/_y` (previously a single unsuffixed pair). Callers were
audited and split by what they actually measure -- ascent/descent/line-height
use the `_y` pair, measured text width and window column width use the `_x`
pair. `ATARI_ASSUMED_DPI` still exists, but only as a per-axis defensive
fallback if a driver ever reports a 0 or unusable pixel size (shouldn't
happen on real hardware, but must never divide by zero) -- it is no longer
the primary source of truth. T4.2's AC (wrap correctness as an
internal-consistency property between `plat_measure_text` and the paint
code's pixel positions) still holds, since both still go through the same
per-axis ratio; the ratio itself is now real instead of assumed. Verified
via `host/tools/ci_atari.sh` (which runs with no GDOS attached, the common
case): `wp_atari_layout_selfcheck()` still passes with the real DPI values
computed above.

**Real ascent/descent via `vqt_fontinfo()` (revisits T4.2, post-M8).**
`plat_font_metrics_get` previously approximated ascent/descent from
`vst_point()`'s own `charh`/`cellh` outputs. It now uses
`vqt_fontinfo()`'s `distances[]` breakdown instead: `distances[1]` is
descent-to-baseline and `distances[3]` is ascent-to-baseline, both **positive
magnitudes** (not signed coordinates) -- verified empirically (not assumed
from the VDI reference alone, since a wrong sign or index would silently
mis-position every line): on the same no-GDOS EmuTOS boot, `distances[3]`
(13) matched `vst_point`'s own `charh` (13) exactly, while `distances[1]`
(2) gave a tighter, more accurate descent than the old `cellh - charh`
approximation (3) would have. `vst_point()` is still called first (required
to select the point size before querying font info); its width/height
outputs are just no longer used for ascent/descent.

**GDOS detection and scalable-font loading (post-M8).** `vq_gdos()` is now
queried once, in `ensure_metrics_vdi()`. If a GDOS is resident,
`vst_load_fonts(handle, 0)` is called so `vst_point()` can hit exact
requested point sizes via the scalable outline renderer instead of snapping
to the nearest built-in bitmap size. If no GDOS is present, or the load call
fails, this is a no-op and behavior is unchanged from before this investigation.
This is deliberately narrow in scope: the document model has no font-family
concept (`plat_font_open` has always ignored its `utf8_font_name` argument --
see `docs/document-model.md`), so this does *not* add typeface selection,
only better size accuracy when a scalable renderer happens to be present.

**Known gap: GDOS-active path is not covered by automated CI.** A real
SpeedoGDOS 5.7 install (from the user's own original floppies, following the
real `INSTALL.PRG` installer under Hatari with real mouse-driven interaction
-- see the mouse-injection technique this investigation also produced,
recorded separately) reproducibly panics during its own AUTO-folder boot
under this Hatari v2.6.1 + EmuTOS 0.9.12 combination (`Panic: Illegal
Instruction`, `pc=00000030`, right after SpeedoGDOS's own cache-loading
banner and immediately after a `have to clip 1 chars from '#ECACHE2#'!`
warning from Hatari's GEMDOS HDD emulation -- `EXTEND.SYS`'s
`CACHEFILE=#ECACHE2#` setting is 9 characters between its delimiters,
exceeding an 8.3 short name, which is the leading suspect). This reproduced
identically against both a fresh copy of the installed `C:` drive and the
original, untouched one, with and without a pre-created `C:\SPDTMP`
directory -- ruling out a simple missing-directory or copy-artifact cause.
The crash happens entirely inside GDOS's own boot sequence, before any
application code (including `WP.PRG`/`SMOKE.PRG`) ever gets control, so it
is a real environment/emulation-compatibility limitation, not something this
project's own code can work around. `host/tools/ci_atari.sh` therefore still
boots with no GDOS attached (the common case for a real 1MB ST anyway); the
GDOS-detection code path (`vq_gdos()` returning nonzero, `vst_load_fonts()`
succeeding) is verified by API contract and by the config files a real
install actually produces, not by an automated end-to-end boot. If a future
Hatari/EmuTOS version resolves this, revisit adding a GDOS-active CI leg.

**Metrics VDI handle is separate from any window's VDI handle.**
`plat_font_open` can be called before a window exists (or independent of
one), so `metrics_atari.c` lazily opens its own virtual VDI workstation on
first use, distinct from the one `app_shell.c` opens for its window. VDI
text attributes (point size, effects) are workstation-global state, not
per-call, so every metrics query re-applies them first -- this matters
because `linebreak.c`'s font cache can hand `plat_measure_text` several
different `plat_font` instances that all share this one handle.

**Color is a black/white threshold, not a real palette search.** VDI text
color is a *palette index* on the ST (16-color low-res palette), not RGB.
`rgb_to_vdi_color()` in `draw_atari.c` maps by luminance threshold to
`G_BLACK`/`G_WHITE` only -- correct for the only case this milestone
exercises (black text on a white page) and explicitly not a nearest-color
search over the full palette. Revisit when color text is a real feature.

**Charset conversion is ASCII-only.** `plat_utf8_to_native`/
`plat_native_to_utf8` pass ASCII (0x00-0x7F) through unchanged and replace
every other codepoint (regardless of its UTF-8 byte length -- correctly
decoded via `utf8_decode`, not treated byte-by-byte) with a single `'?'`,
counted in `unmapped_count`. Correct and sufficient for this milestone's
ASCII sample document; a real Atari ST high-ASCII mapping table is a
deliberate, documented gap, not a silent one.

**The sample document.** `app_shell.c` hardcodes and imports one paragraph
at startup (no Open dialog yet -- that's T4.3), lays it out against the
window's current work-area width, and repaints it on every `WM_REDRAW`,
relayouting again on `WM_SIZED`. There is no scrolling yet: content past
the first screenful is simply not drawn (not wrapped around, not
overwritten) -- correct behavior for a short demo document, not a real
document view. Verified visually: a real screenshot shows the sample
paragraph rendering with sensible word-boundary wrapping, filling the
window without overflowing its right edge.

### Fixed: menu title text was rendering as real glyph noise (post-M8)

The earlier note here ("likely a EmuTOS-desktop or Hatari screenshot-capture
interaction, not a tree-construction bug") was wrong, and was corrected by
actually reading EmuTOS's own AES source (`aes/gemmnlib.c`, `aes/gemobjop.c`)
rather than continuing to trust a screenshot-artifact theory. The real cause:
`mn_bar()`'s install path calls `ob_draw(tree, THEBAR, MAX_DEPTH)`
unconditionally, which walks and draws the *entire* subtree under every
title -- including closed dropdown boxes -- immediately at install time,
with clipping turned off. `just_draw()` (`aes/gemobjop.c`) only skips an
object (and its children) if its `ob_flags` has `OF_HIDETREE` (0x0080,
"object and its children are hidden") set. `wp_menu_tree`'s dropdown boxes
never set it, so every title's Open/Save/Quit-style items got drawn
immediately, overlapping the title text -- the "glyph noise."

Verified two ways, not just theorized: (1) bisection -- a minimal hand-built
probe tree with a bare title (no dropdown) rendered perfectly; the same
title with a populated `G_BOX` child reproduced the exact corruption; an
*empty* box (no items) did not, isolating the trigger to a box with real
children specifically. (2) fix confirmed visually -- adding `OF_HIDETREE`
to `WP_MENU_OBJ_FILE_BOX`/`EDIT_BOX`/`TOOLS_BOX`'s `ob_flags` (`app_shell.c`)
and re-booting under real Hatari shows "File" rendering cleanly, no noise.
`wp_menu_tree_selfcheck()` now asserts `OF_HIDETREE` is set on all three
boxes so this can't silently regress.

One real, source-grounded lead was investigated and discarded along the
way: `menu_fixup()` (called unconditionally by `mn_bar()`) hardcodes
`tree[0].ob_tail` as a "desk accessory" box it rewrites -- a real
mechanism, but empirically confirmed (via before/after field dumps over
NF_STDERR on a real boot) to leave `wp_menu_tree` untouched for our tree
shape, so it is not the cause here and was not worked around.

### Known gap: only the first title actually renders (found post-M8, not yet fixed)

Fixing the glyph-noise corruption above surfaced a second, previously
masked bug: only *one* title (whichever sits at `wp_menu_tree` array index
1) is drawn at all -- Desk/Edit/Tools never appear, verified visually on a
real boot. Root cause, again from reading `ob_draw`/`everyobj`
(`aes/gemobjop.c`) directly: `mn_bar()` hardcodes array index 1 as
`THEBAR`, and `everyobj()`'s walk stops as soon as it would revisit
`tree[THEBAR].ob_next` -- which, when index 1 is itself a title (as in
every version of this tree so far, including the desk-title addition),
equals that title's own immediate next sibling, so the walk terminates
after exactly one object. Tracing `mn_bar()`'s other index-0/1/2 uses
(`THESCREEN`/`THEBAR`/`THEACTIVE`) strongly suggests EmuTOS's own desktop
tree is actually a 3-level structure -- an outer root, a `BAR` container
at index 1 whose *children* are the real titles, and some index-2
bookkeeping object -- not the 2-level (root directly owns titles)
structure this tree has used since T4.1.

A 3-level restructuring was implemented and visually ruled out as
*necessary* for the fix above (the glyph-noise fix needed only
`OF_HIDETREE`), but attempting it broke something else: real Atari CI
(`host/tools/ci_atari.sh`) hung (timeout, not a clean crash) partway
through the smoke test, right after a synthesized `MN_SELECTED` dispatch
that calls the real `menu_tnormal()` AES function -- consistent with some
other `mn_bar()`-internal index-0/1/2 assumption (plausibly the
`ob_actxywh(tree, THEACTIVE, ...)` call) reading nonsense from whatever
ended up at that array slot in the restructured tree. Given the risk of
shipping a hung CI, this was reverted; the codebase currently has the
*working, CI-green, corruption-free* 2-level tree with only one title
visible. Fixing "only one title renders" for real needs the 3-level
structure implemented *correctly* (matching all of `THESCREEN`/`THEBAR`/
`THEACTIVE`'s real roles, not just the ones this investigation identified)
and re-verified against both a real boot screenshot and `ci_atari.sh`
before landing -- a good next task for whoever picks this up, with the
source files above already identified as the ground truth to work from.

## File I/O, clipboard, timer, and basic editing (T4.3)

`file_atari.c` implements `plat_file_*` directly over mintlib's `fopen`/
`fread`/`fwrite`/`fseek`/`ftell`/`remove` -- no raw GEMDOS trap calls needed
(confirmed during the T4.1 toolchain research). `timer_atari.c` implements
`plat_ticks_ms`/`plat_sleep_ms` via `clock()` (the ST's 200Hz system tick),
integer math only, no floats. `clipboard_atari.c` implements the classic
GEM `SCRAP.TXT` convention: `scrp_write()` registers a fixed clipboard
directory (`C:\`) with AES so other GEM apps could interoperate, and reads
fall back to that same fixed directory if `scrp_read()` returns something
unexpected -- reasonable since this app is both producer and consumer of
the clipboard in this environment.

The menu bar grew a second title (File: Open/Save/Quit, Edit: Cut/Copy/
Paste) using the same hand-built `OBJECT[]` approach and index/linkage
convention as T4.1; `wp_menu_tree_selfcheck()` was extended to cover both
titles' full chains.

**File Open/Save use the real GEM file selector** (`fsel_input`), not a
hardcoded path -- `run_file_selector()` in `app_shell.c` handles the classic
gotcha that `fsel_input` rewrites the directory portion of its `path`
buffer in place but leaves the wildcard pattern attached, so the directory
has to be recovered by truncating at the last backslash before appending
the chosen filename.

**Cut/Copy/Paste operate on the whole document, not a selection.** There is
no selection model in the engine yet (`docs/editing.md` already notes this
for multi-run deletes), and building one is out of scope for wiring the
clipboard seam. Copy exports the whole document as plaintext to the
clipboard; Cut does that and then clears the document; Paste replaces the
document with the clipboard's content. This is a deliberate scope
narrowing, not an oversight -- the alternative (a real selection UI) is a
substantially bigger task that belongs with real interactive editing
polish, not this milestone.

**Keyboard editing is scoped to a single paragraph.** `wp_dispatch_key()`
handles Backspace (delete before cursor) and printable ASCII (insert at
cursor) by threading a tracked `wp_pos` cursor through `wp_edit_insert_text`/
`wp_edit_delete_range` -- the same M2 editing engine, unmodified. Enter is
explicitly a no-op: `WP_OP_SPLIT_PARA` is still unimplemented
(`docs/editing.md`), so this app cannot create a new paragraph by typing,
and doesn't pretend to. Backspace across a run boundary is always a valid
single-run delete by construction (the deleted codepoint is entirely
within whichever run `wp_pos_prev()` lands in), so no cross-run looping is
needed despite `wp_document_delete_range_raw`'s single-run-only contract.

**No visible cursor is rendered.** The cursor position is tracked and
edited correctly, but `paint_document()` doesn't draw a caret at it --
computing its on-screen pixel position requires re-measuring text up to
the cursor within its line, which is real extra geometry work deferred as
a documented gap rather than scope-crept into this task. Editing is
verified functionally (typed/deleted content actually changes what's
painted), not visually via a rendered caret.

**Verified two ways**, extending the same two-track approach as T4.1/T4.2:
1. **Automated**: `wp_atari_file_clipboard_selfcheck()` round-trips a
   fixture string through `plat_file_write`/`plat_file_read` at a real
   scratch path and separately through `plat_clipboard_set_text`/
   `get_text`, asserting byte-identical round-trips both ways -- run via
   `host/tools/ci_atari.sh`, and confirmed to catch a regression
   (deliberately dropped the last byte on clipboard read during
   development and watched it fail, then restored it).
2. **Visual/exploratory**: `WP.PRG` running under Hatari with `xdotool key`
   injection to type into the live window produced visible document
   mutation (the sample paragraph's tail was edited on screen), confirming
   the keyboard pipeline (`evnt_multi` &rarr; `wp_dispatch_key` &rarr; engine
   edit calls &rarr; relayout &rarr; repaint) works end to end for real. This
   is reported as suggestive, not rigorous: unlike the mouse-click
   finding in the T4.1 gap above, exact keystroke-to-character fidelity of
   `xdotool key`/`type` injection into this Hatari/SDL/X11 stack was not
   independently verified (the characters that ended up in the document
   did not visibly match the literal string sent), so treat this as "the
   pipeline is wired and live," not as a certified input-fidelity test.
   The automated round-trip check above is the rigorous proof for T4.3's
   AC; this is corroborating evidence on top of it.

## Memory budget pass at 1MB (T4.4)

`wp_atari_memory_budget_selfcheck()` (`app_shell.c`) builds a ~7.5KB, 60-
paragraph document, paginates it at real US Letter dimensions (1in margins)
through `wp_document_paginate()` -- the first time this app exercises
pagination at all; T4.2/T4.3 only ever called `wp_paragraph_relayout()`
directly for the single-window on-screen flow -- performs a batch of
inserts and deletes, saves it, and frees everything.

**GEMDOS `Malloc(-1L)` (largest free block) is real headroom evidence, but
turned out NOT to be a leak signal.** The plan was to compare it before and
after freeing everything as a leak proxy. Empirically, across three
checkpoints (before load, after load+edit, after freeing everything), it
reported the *exact same value* every time (confirmed both with correct
code and with a deliberately introduced leak -- see below) -- mintlib's
`malloc()` claims a heap pool from GEMDOS once at process startup and
manages it internally from then on, so GEMDOS-level free-block queries
don't move for allocation traffic this small. It remains genuinely useful
for a different claim: ~662KB stayed free throughout, out of a real 1MB
Hatari config, which is strong evidence the app is nowhere near exhausting
its budget.

**The real leak signal is `mem_atari_live_count()`** (`mem_atari.h`/`.c`,
new in T4.4) -- an `alloc_count`/`free_count` pair mirroring
`platform/host/mem_host.c`'s pattern exactly, which the host test suite has
relied on since M1. This is the first time that exact style of leak
assertion (`live_count() == 0` after a full lifecycle) has been proven on
the real Atari target rather than just the host stub. Confirmed to
actually catch a leak, not just rubber-stamp: temporarily skipped
`wp_document_free()` in `wp_atari_memory_budget_selfcheck()`, watched the
check fail (while `Malloc(-1L)` stayed exactly as constant as ever,
confirming it would NOT have caught this on its own), then restored it.

**Verified via `host/tools/ci_atari.sh`** (the same automated smoke-test
harness as T4.1-T4.3): `page_count > 1` (genuinely multi-page), comfortable
`Malloc(-1L)` headroom after loading, and `mem_atari_live_count() == 0`
after everything is freed. The three `Malloc(-1L)` readings are logged to
the CI output (not just the pass/fail verdict) so a human reviewing a run
can see the real numbers.

**Known gap, consistent with the rest of M4**: this proves the *document
model and platform.h plumbing* stay within budget at multi-page scale, not
the on-screen editing UI at that scale -- there is still no scrolling
(T4.2/T4.3), so a real user could not actually page through a document this
long in the current UI, only load/edit/save it. Closing that gap is future
polish, not part of this milestone's AC ("opens ... edits, saves -- within
1MB TOS config ... without exhaustion").

This closes out M4: all four tasks (T4.1 AES shell, T4.2 VDI metrics/draw,
T4.3 file/clipboard/timer, T4.4 memory budget) are done, each compiled with
the real cross-compiler, linked into genuine plain-TOS `.prg` binaries, and
verified under Hatari with a mix of automated (Native-Features-driven,
regression-tested) and visual/exploratory checks. See `docs/milestones.md`
for the full per-task scope notes and honest gaps list.

## Real RS-232 offload transport (M6 T6.3)

`platform/atari/transport_atari.c` implements `plat_transport_open/read/
write/close` over the ST's AUX BIOS device (`DEV_AUX` = 1 -- confirmed
against `mint/ostruct.h`, not guessed) using the classic `Bconin`/
`Bconout`/`Bconstat`/`Bcostat`/`Rsconf` calls. Endpoint format is
`"serial:<device>:<baud>"` (platform.h's own doc comment already
established this convention); `<device>` is currently unvalidated beyond
non-empty, since the ST has exactly one serial port. `Rsconf` configures
8N1 at the requested baud, async /16 clock, receiver enabled, no flow
control; the transmit-side `tsr`/`scr` fields are left unchanged
(`RS_INQUIRE`, the same "-1 means leave unchanged" idiom `mint/ostruct.h`
uses consistently elsewhere) since no transmit-side bit list is
documented anywhere on this system.

**No MiNTnet variant.** FreeMiNT's BSD-socket GEMDOS extensions
(`Fsocket` et al, function numbers `0x160+`) are a FreeMiNT-kernel-only
extension -- confirmed by disassembling the actual shipped `libc.a`
(`___socket` traps GEMDOS function `0x160`) and checking that plain
TOS/EmuTOS's function range doesn't include it. Pursuing this would mean
booting real FreeMiNT under Hatari, a materially heavier setup than the
EmuTOS boot this project's CI already uses everywhere else, for a
target (plain TOS) that can't use it anyway -- `plat_transport_open`
returns `WP_UNSUPPORTED` for any endpoint prefix other than `"serial:"`,
honestly, not silently.

### Verified two ways

1. **Automated, end to end, over real emulated RS-232**:
   `host/tools/ci_atari_serial.sh` bridges Hatari's `--rs232-in`/
   `--rs232-out` to `offload/gateway/mock_gateway.c`'s new `--serial-rx`/
   `--serial-tx` mode via two named FIFOs (both opened `O_RDWR`, not
   `O_RDONLY`/`O_WRONLY`, specifically so neither side's open blocks
   waiting for a peer -- the same trick `platform/host/transport_host.c`'s
   `pipe:` endpoint already relies on). `host/tools/atari_serial_smoke.c`
   calls the real `offload_init()`/`offload_query()`/`offload_submit()`/
   `offload_shutdown()` client API -- the same code path
   `engine/src/offload_client.c` and `offload/transport/wire.c` already
   have full TCP-backed test coverage for -- through
   `transport_atari.c`'s real `Bconin`/`Bconout`. It proves: the HELLO/
   CAPS handshake completes and negotiates the gateway's real
   `protocol_version` and `gateway_name` (T6.3's literal AC), AND a full
   `OFFLOAD_THESAURUS` request/response round trip completes correctly --
   going further than the AC strictly requires, reusing the exact
   fixture-backed logic the TCP tests already exercise.
2. **A real bug, found and fixed via the same bisection discipline used
   elsewhere in this project** (the RTF binary-search bug, the `-O2` NF
   trap gotcha): the first attempt at the serial CI script hung, then
   crashed Hatari itself with SIGPIPE. Isolating the cause required
   ruling out three components independently before finding it:
   - Raw `Bconin`/`Bconout`/`Rsconf` mechanics, tested with a minimal
     PING/PONG program bridged through a hand-written Python FIFO
     script -- worked.
   - `plat_ticks_ms()`/`clock()` actually advancing over real time under
     Hatari (a plausible culprit, since `transport_atari.c`'s read/write
     timeout loops depend on it) -- tested directly, confirmed working
     (`CLOCKS_PER_SEC` is genuinely 200, matching the ST's real system
     tick, and `clock()` advances correctly against real wall-clock
     time).
   - `mock_gateway.c`'s new serial mode, tested independently against a
     hand-written Python client speaking the wire protocol directly over
     the FIFOs (no Hatari involved) -- worked, and cross-checked the
     protocol bytes by hand (magic `"WP"`, version 1, CRC) after an
     initial copy-paste bug in the *test script itself* (wrong magic
     bytes) was caught and fixed.

   With all three cleared, adding temporary `fprintf` tracing to
   `mock_gateway.c`'s read loop pinned it down: `poll()` on the gateway's
   read FIFO returned 0 (a clean timeout) after its **hardcoded 5-second**
   wait for the client's first HELLO frame. A full Atari boot + program
   load under Hatari's cycle-exact CPU emulation genuinely takes longer
   than 5 real seconds before the ST program ever reaches its first
   `Bconout` call -- so the gateway gave up, correctly closed both FIFOs,
   and exited (5000ms was calibrated for the existing TCP tests, where
   the client connects near-instantly, and was never revisited for a
   real-hardware-timed scenario). The genuinely nasty part: Hatari's
   *own* subsequent write of the ST program's HELLO bytes to the now-
   reader-less `--rs232-out` FIFO then raised SIGPIPE, killing the whole
   emulator -- not a failed test, a crashed test *harness*, which is why
   it took real bisection rather than reading a stack trace to find.
   Fixed with a new `--hello-timeout-ms` flag on `mock_gateway.c`
   (default 5000, unchanged for the TCP tests; `ci_atari_serial.sh` passes
   25000 for the serial case), documented in the gateway's own file
   comment so this specific failure mode doesn't need rediscovering.

### Known gaps

- **Not wired into `platform/atari`'s UI.** `app_shell.c` doesn't call
  `offload_init`/`offload_submit` anywhere yet -- there's no "Check
  Spelling" or "Thesaurus" menu action wired to the offload link. T6.3's
  own AC is about the transport working, not about a UI feature using
  it; wiring an actual menu action is natural follow-up work (would also
  need T4.3-style keyboard/menu plumbing extended, not just the
  transport).
- **Baud rate is fixed per `plat_transport_open` call, not
  auto-negotiated.** The endpoint string must specify a baud rate the
  gateway side is also configured for (in practice, `9600` throughout
  this project's own testing) -- there's no protocol-level baud
  negotiation, matching how real serial links have always worked (both
  ends configure to a known-shared rate out of band).

## RTF + Spell Check wired into the UI (post-M6)

M5 landed `format_rtf` and `services/spell` as tested, cross-compiling
modules with no UI entry point (`docs/milestones.md`'s M5 artifact note
called this out explicitly). This closes that gap: `platform/atari/
app_shell.c`'s File Open/Save and a new Tools menu now drive both.

**RTF is picked by filename extension, not a format-choice control.**
`fsel_input()` (the classic GEM file selector) has no such control, so
`str_ends_with_ci(fullpath, ".RTF")` in `do_file_open`/`do_file_save`
decides which pair of import/export calls to use -- the same way a user
picks the format in most classic word processors of this era (type or
select a different extension). `replace_document()` was factored out of
the old `set_document_from_text()` so both the plaintext and RTF Open
paths share the same "swap in a freshly built document, reset cursor and
undo journal" logic. Open's file-selector wildcard changed from `*.TXT`
to `*.*` so `.RTF` files are visible at all.

**Spell Check is a new third menu title** (File, Edit, Tools), appended
after Edit in the hand-built `OBJECT[]` tree rather than inserted between
existing titles, so none of T4.1/T4.3's existing menu object indices had
to change -- only `wp_menu_tree_selfcheck()` grew three more linkage
assertions. `do_spell_check()`:
- Lazily opens a dictionary at a fixed path, `C:\WPSPELL.DIC`, via
  `spell_dict_open()` -- there is no shipped dictionary asset (that would
  be a real multi-week undertaking of its own, same reasoning
  `docs/services/spell.md` gives for not being Hunspell-compatible); a
  user who wants real spell-check on `WP.PRG` supplies their own sorted
  word-list file at that path. A missing file is reported through the
  same `form_alert()` path as everything else here, not a crash --
  consistent with this project's "absent gracefully" pattern for optional
  capabilities (CLAUDE.md's Offload Rules say this for the Pi link;
  the same spirit applies to an optional on-disk dictionary).
- Scans the WHOLE document for the first unknown word (same "no
  selection model" scope as Cut/Copy/Paste), tokenizing ASCII-alpha runs
  **within each run only** -- a word split across a style boundary (e.g.
  "wo" bold immediately followed by "rld" plain) is checked as two
  fragments, not reassembled into "world" first. A real, documented gap,
  not an oversight.
- Reports through `form_alert()`: dictionary missing, no misspellings
  found, or the first unknown word plus up to three `spell_suggest()`
  candidates. There is no interactive correction UI (no click-to-fix,
  no next/previous, no in-place highlighting) -- this is a "check and
  report" action, not a red-squiggle spell checker; building the former
  is a substantially bigger feature deliberately left for later, the
  same way T4.3 scoped Cut/Copy/Paste to whole-document operations rather
  than inventing a selection model to unblock them.

**`do_spell_check()` is deliberately never called from any CI path.**
`form_alert()` blocks synchronously waiting for a button click; nothing
in this project's headless Hatari CI can click it, and calling it from an
automated selfcheck would simply hang until `ci_atari.sh`'s outer
`timeout` killed Hatari -- a self-inflicted repeat of the exact SIGPIPE-
adjacent "something hangs and only the timeout notices" failure mode
T6.3 spent real effort tracking down (see above). So the new selfcheck,
`wp_atari_rtf_spell_selfcheck()`, exercises only the non-blocking
building blocks `do_spell_check()` calls, not the menu action itself:
- `str_ends_with_ci()`'s extension matching (including the boundary case
  where the whole string equals the suffix).
- A full RTF round trip through *real* `plat_file_write`/`plat_file_read`
  on this target: `format_rtf_export()` a fixture document, write the
  bytes to a scratch file, read them back, `format_rtf_import()`, export
  as plaintext, and diff against the original. T5.1/T5.2 proved
  `format_rtf_*` cross-compiles clean for m68k-atari-mint; this is the
  first time it has actually *run* on this target rather than just built
  for it.
- Real dictionary lookups (`spell_dict_open`/`spell_check_word`/
  `spell_suggest`) against a scratch dictionary file this test writes
  itself (no shipped asset needed) -- proving `services/spell.c`'s
  binary search over `plat_file_seek`/`read` works on real Atari file
  I/O, not just the host stub.

Run via `host/tools/ci_atari.sh` (added to `atari_smoke.c`'s check list)
and confirmed passing on real Hatari alongside the rest of the smoke
suite; `host/tools/ci_atari_serial.sh` also still passes unchanged,
confirming linking `services/spell` into every Atari binary (`WP.PRG`,
`SMOKE.PRG`, `SERIALTST.PRG` all now pull in `ATARI_SERVICES_OBJ`, per
the `Makefile`) didn't disturb the serial offload path.

## Thesaurus wired into the UI (post-M6, post-Ollama-gateway)

T6.3's own "Known gaps" note (above) called this out explicitly:
"`app_shell.c` doesn't call `offload_init`/`offload_submit` anywhere
yet -- there's no 'Check Spelling' or 'Thesaurus' menu action wired to
the offload link." This closes that gap for Thesaurus (Spell Check
stays on-device, via `services/spell` -- see the section above; there is
still no offload-backed spell action, and that remains a deliberate
choice, not an oversight: `offload/gateway/ollama_gateway.c` itself
doesn't implement `OFFLOAD_SPELL_CHECK`, see
docs/formats/offload-protocol.md).

**A new third title item, Tools > Thesaurus,** alongside Spell Check
(`WP_MENU_OBJ_TOOLS_THESAURUS`, appended after `WP_MENU_OBJ_TOOLS_SPELLCHECK`
in the hand-built `OBJECT[]` tree so no existing index moved -- same
"append, don't insert" discipline as every other menu growth in this
file). `wp_menu_tree_selfcheck()` grew the matching linkage assertions.

**Looks up the word immediately BEFORE the cursor**, not a word under a
mouse-driven selection -- there is still no selection model at all
(`docs/editing.md`), and after typing a word the cursor naturally sits
right after it (this app has no other way to move the cursor -- no
arrow-key navigation either), so "the word I just typed" is what this
app can actually express. `extract_word_before_cursor()` walks backward
over any trailing non-alpha bytes first, then over the alpha run behind
them, so it finds the nearest preceding word even from a couple of
spaces past it (deliberate -- the same forgiving spirit as many editors'
Ctrl+Backspace -- pinned down by `wp_atari_thesaurus_selfcheck()`'s
offset=6 case, not just the immediately-adjacent case). Scoped to the
CURRENT RUN only, the same boundary `do_backspace()` already has.

**Connects lazily, once, to a fixed endpoint** (`serial:aux:9600`) on
first use -- `ensure_offload()` mirrors `get_spell_dict()`'s "try once,
remember the result" pattern. An absent/unattached device fails within
`offload_client.c`'s own bounded 2s+2s connect+handshake timeout, a
one-time cost, not a stall on every click; a real device once found stays
connected for the rest of the session. `offload_shutdown()` is called
unconditionally at app exit (safe even if `ensure_offload()` was never
reached -- it's a no-op against a NULL transport).

**Timeout is deliberately generous** (`WP_ATARI_THESAURUS_TIMEOUT_MS` =
90s): a real gateway's cold model load can take tens of real seconds
(measured directly while building `ollama_gateway.c` -- see
docs/formats/offload-protocol.md), and this is a user-initiated action
with visible feedback (the AES stays responsive to nothing else while
blocked, admittedly, but a slow real answer beats a fast wrong timeout).

**Reports through `form_alert()`**, same as Spell Check: no offload
device, the connected gateway doesn't support Thesaurus (a real,
expected case -- e.g. `mock_gateway.c`'s SPELL_CHECK-only builds, or any
future gateway that only implements other ops), or the synonym list
itself (decoded from the same length-prefixed word-list wire shape
`docs/formats/offload-protocol.md` documents, reusing `be_read16`).

**Verified two ways, deliberately not three:**
1. `wp_atari_thesaurus_selfcheck()` (run via `ci_atari.sh`) proves
   `extract_word_before_cursor()` -- the new pure-logic piece -- against
   fixture cursor positions: a word right at the cursor, no word at the
   very start of a run, the "skip trailing whitespace" case above, and a
   run with no alpha content anywhere before the cursor. No offload calls
   here at all.
2. The offload_client.c <-> real-gateway round trip over real emulated
   RS-232 is NOT re-proven here -- it's already covered end to end by
   T6.3's `atari_serial_smoke.c`/`ci_atari_serial.sh`, calling the exact
   same `offload_init`/`offload_submit`/`offload_shutdown` API this menu
   action uses. Re-running that from a second selfcheck would only add a
   guaranteed-to-fail ~2s connect/handshake timeout to `ci_atari.sh`
   (which has no gateway attached) for zero new coverage -- a deliberate
   scope call, not a missing test.

**Known gap, honestly not closed here:** `do_thesaurus()` itself (the
`form_alert()`-driving wrapper) has never actually been run and
observed, by this project or by hand -- consistent with
`do_spell_check()`'s identical, already-accepted gap above. It is not
just untested by CI: unlike Quit/WM_REDRAW's synthesized-message
dispatch tests, there is no safe way to exercise this one even manually
through this project's existing tooling, because `form_alert()` is a
real blocking AES call waiting for a real click or keypress -- routing a
synthesized `MN_SELECTED` message into `wp_dispatch_message()` the way
the Quit test does would block inside that same call with nothing to
unblock it, hanging Hatari exactly like the T6.3 SIGPIPE incident this
document already describes at length. Verified instead, transitively:
`extract_word_before_cursor()` (selfcheck above) and the
`offload_init`/`offload_submit`/`offload_shutdown` sequence
(`atari_serial_smoke.c`, T6.3) are each proven correct on their own; this
function is a thin, mechanical composition of the two plus string
formatting, not independently re-verified end to end. A future task
could close this with real `xdotool` mouse-click injection into a form
alert if that proves reliable enough (T4.1's existing gap note is
skeptical it will be, in this headless environment) -- not attempted
here.

## ODT + PDF wired into the UI (M7 T7.4)

Closes M7's own T7.4 note ("Engine-side wiring: 'Open ODT' ships bytes,
receives model") the same way Thesaurus closed T6.3's: File Open/Save
now recognize `.ODT` (open and save, via `offload/gateway/odt_gateway.py`
-- `docs/formats/odt.md`) and `.PDF` (save only -- PDF has no importer,
by design, `docs/formats/pdf.md`), extending the SAME extension-sniffing
dispatch `do_file_open`/`do_file_save` already used for `.RTF` vs
plaintext. No new menu items, no format-choice dialog -- typing a
different extension into the file selector's filename field is still
how this app picks a format, the same classic-GEM convention the RTF
wiring already established.

**`do_odt_open()`** reads the whole `.odt` file (capped at
`WP_ATARI_ODT_MAX_FILE_SIZE`, 60000 bytes -- comfortably under
`WIRE_MAX_PAYLOAD`'s 64K with room for the format-id byte and frame
overhead; real-world ODT files from other tools commonly run 10-50KB
even for short documents, thanks to `styles.xml`/`meta.xml`/thumbnail
bloat this project's OWN minimal ODT writer doesn't produce), ships it
through `OFFLOAD_CONVERT_IMPORT`, and feeds the RTF that comes back
straight into the ALREADY-TESTED `format_rtf_import()` -- no new
parsing logic on the engine side at all, exactly as
`docs/formats/offload-protocol.md`'s "RTF is the interchange format"
design decision promised it would be.

**`do_odt_pdf_save()`** is the mirror: `format_rtf_export()` the current
document (the same bytes the local `.RTF` Save path already writes),
ship those through `OFFLOAD_CONVERT_EXPORT` with `format_id` = ODT or
PDF, write whatever comes back straight to disk. This app never parses
the `.odt`/`.pdf` bytes it writes -- they're opaque, gateway-produced
output, same as they are for `do_odt_open`'s input in reverse.

**A real engine-side change was needed to make this actually work, not
just compile.** `engine/src/offload_client.c`'s `OFFLOAD_MAX_RESPONSE_PAYLOAD`
was 4096 bytes -- plenty for THESAURUS/SPELL_CHECK's word-list replies,
but a real gateway-produced ODT or PDF for even a modest document
routinely exceeds that (an early, larger `host/tests/test_convert.c`
fixture hit this exact ceiling during T7.3 development -- `offload_submit`
returned `WP_RANGE`, not a gateway bug). Raised to 32768: still bounded,
still freed immediately after copying into `resp->result` (which is
sized to the ACTUAL response length, not this constant), still a small
fraction of the 1MB Atari ST budget as a temporary allocation.

**Real RS-232 is slow for whole-file transfer, and this app's timeouts
say so honestly.** THESAURUS/SPELL_CHECK payloads are a few bytes;
ODT/PDF payloads are whole files. At 9600 baud with 8N1 framing (10 bits
per byte, `transport_atari.c`'s `RS_8BITS | RS_1STOP`), real throughput
is about 960 bytes/sec -- a modest multi-KB document can genuinely take
tens of real seconds to transfer each direction. `WP_ATARI_CONVERT_TIMEOUT_MS`
is set to two minutes accordingly, deliberately far more generous than
Thesaurus's already-generous 90 seconds (which is sized for a slow
COLD-MODEL-LOAD, not a slow TRANSFER -- a different bottleneck, both
real, both worth naming precisely rather than picking one big number for
every offload feature).

**Verified two ways**, the same split as every other offload-backed
feature in this app:
1. `host/tools/atari_convert_smoke.c` / `host/tools/ci_atari_convert_serial.sh`
   -- genuinely NEW coverage, not a repeat of T6.3's
   `atari_serial_smoke.c` (which only ever proved THESAURUS, a tiny
   single-word payload, over serial). Bridges Hatari's real emulated
   RS-232 to a real `offload/gateway/odt_gateway.py` instance (which
   already supports `--serial-rx`/`--serial-tx`, unchanged from T7.2) via
   the same two-FIFO mechanism `ci_atari_serial.sh` uses, and drives the
   real `offload_init`/`offload_submit`/`offload_shutdown` API end to
   end: builds a fixture document, `format_rtf_export()`s it,
   `CONVERT_EXPORT`s to ODT over real serial (checked for a real ZIP
   magic), `CONVERT_IMPORT`s those SAME ODT bytes back over the same
   connection (proving the request/response loop handles more than one
   request, and closing the round trip with no external tool needed),
   `format_rtf_import()`s the result and asserts every attribute
   (alignment, bold, exact text) survived, then separately
   `CONVERT_EXPORT`s to PDF over real serial (checked for the real
   `%PDF` magic). A real test-authoring bug was caught here too: a
   hardcoded expected string length off by one (30 vs the fixture's
   actual 29 characters) -- the same class of mistake this project's
   history keeps finding in hand-counted test literals, caught the same
   way, by actually running the test rather than trusting the arithmetic.
2. `do_odt_open()`/`do_odt_pdf_save()` themselves are never exercised by
   any CI path, for the identical reason `do_thesaurus()`/
   `do_spell_check()` aren't: they end in `form_alert()`, a real blocking
   AES call this headless environment cannot safely drive to completion.
   Verified instead, transitively, exactly like Thesaurus: the
   `offload_client.c` round trip is proven end to end (above, AND by the
   whole-file real-transfer case specifically, which Thesaurus's
   single-word case never exercised), and `format_rtf_import`/
   `format_rtf_export` are proven independently (M5). These two functions
   are a thin, mechanical composition of already-proven pieces plus file
   I/O and string formatting, not independently re-verified end to end
   as a running menu action.

## Grammar Check wired into the UI (M8 T8.1, offline fallback M8 T8.2)

Closes the "engine/UI wiring" half of T8.1's own AC ("Paragraph in,
structured issues out; ST renders them") -- the protocol and both
gateways (fixture-backed and real-Ollama-backed) were already done and
verified (`docs/formats/offload-protocol.md`, `docs/milestones.md`);
this is the part that makes it a real menu action on this target.

**A new third Tools item, Tools > Grammar Check**, appended after
Thesaurus (`WP_MENU_OBJ_TOOLS_GRAMMARCHECK`) in the hand-built
`OBJECT[]` tree -- same "append, don't insert" discipline as every
earlier menu growth in this file, so no existing index moved. The Tools
box grew from 2 rows to 3 and widened from 14 to 16 character cells
("  Grammar Check" is longer than "  Spell Check"/"  Thesaurus").
`wp_menu_tree_selfcheck()` grew the matching linkage assertions, and
`ci_atari.sh`'s existing "menu tree ob_next/head/tail linkage" check
exercises them on every run.

**A materially different UI shape from every offload action before
it.** Thesaurus and Spell Check each report ONE thing via a single
`form_alert()`. Grammar Check gets back a LIST of positioned issues --
GEM has no control that shows an arbitrary-length list, and this
project has no resource-editor toolchain to build one with (see the
`OBJECT[]` comment at the top of this document), so `do_grammar_check()`
instead cycles through issues one at a time, each its own
`[ Apply | Skip | Stop ]` `form_alert()`. This is the first menu action
in this app that can MUTATE the document from inside a loop of blocking
dialogs, which is where most of the real design work in this task went:

- **The paragraph sent is the CURRENT one** (`g_cursor.section`/
  `g_cursor.paragraph`), concatenated across every run via a new
  `extract_current_paragraph_text()` -- no engine primitive already does
  this (only whole-document plaintext export exists,
  `engine/src/format_txt.c`), so this mirrors that function's inner loop
  narrowed to one paragraph.
- **Wire offsets are never trusted against the LIVE document.** Every
  issue's `start_offset`/`span_len` is paragraph-relative and refers to
  the ORIGINAL paragraph text as sent -- stable for as long as the
  dialog loop runs, since a fix is applied to `g_doc`, never to the
  local snapshot buffer used for display. A new
  `map_paragraph_offset_to_run()` maps a (possibly delta-adjusted, see
  below) paragraph-relative range onto a single run's index and
  run-local offsets, re-walking `g_doc` fresh on every call rather than
  holding a pointer across an earlier edit (`wp_paragraph_run_add` can
  reallocate `para->runs` -- `docmodel.h` says so explicitly, and
  `edit.c`'s own `run_insert_at` relies on the same fact). It returns
  `WP_FALSE` -- decline to auto-apply -- if the range crosses a run
  boundary, since `wp_edit_delete_range`'s contract is single-run-only
  (`docs/editing.md`) and forcing a bad range through risks corrupting
  text this action never meant to touch.
- **Applying more than one fix in the same paragraph needs bookkeeping
  two different ways.** Issues are processed in ascending
  `start_offset` order (a plain insertion sort over up to
  `WP_ATARI_GRAMMAR_MAX_ISSUES` = 16 entries -- the gateway doesn't
  promise emission order), tracking a running `i32 delta` (the net byte-
  length change from every EARLIER applied fix in this same pass) so a
  later issue's ORIGINAL offset can be translated to its CURRENT
  position in the live, already-partially-edited paragraph. Separately,
  an LLM occasionally flags overlapping spans (two issues both touching
  the same word); this is checked against already-applied ranges in
  ORIGINAL-paragraph coordinates (stable, never shifted, so the overlap
  test is a simple interval comparison) BEFORE attempting the
  delta-adjusted apply -- applying a second fix into text a first fix
  already replaced would silently corrupt the document, and this is the
  one failure mode `map_paragraph_offset_to_run`'s own boundary check
  cannot be relied on to always catch (a paragraph that is entirely one
  run will happily accept an offset/length pair that no longer
  corresponds to real, intended text).
- **`g_cursor` is reset to this paragraph's start (run 0, offset 0),
  never left as-is, after any fix is applied.** Its old run/offset may
  now index into a `para->runs` array that `wp_edit_delete_range`/
  `wp_edit_insert_text` resized or reordered elsewhere in the SAME
  paragraph -- a stale index there is a real out-of-bounds risk on the
  very next keystroke, not a cosmetic issue. Run 0 / offset 0 is
  guaranteed to exist afterward because Grammar Check only ever performs
  a delete immediately followed by a non-empty insert (Apply is only
  offered when the gateway sent a non-empty suggestion), and
  `insert_span_at_pos`'s own `para->run_count == 0` case
  (`engine/src/edit.c`) creates a fresh run rather than requiring one to
  already exist.
- **Every quote/message/suggestion shown in a dialog is truncated for
  DISPLAY ONLY**, via a new `append_str_n()` (a thin wrapper over the
  existing `append_str()`), to keep one long field from crowding the
  rest of a `form_alert()` off the ST's tiny screen -- the wire values
  actually written to the document, and the byte-exact substring search
  the GATEWAY already performed against them
  (`docs/formats/offload-protocol.md`), are never touched by this.
- **Timeout and connection handling are identical to Thesaurus**
  (`WP_ATARI_GRAMMAR_TIMEOUT_MS` = 90s, `ensure_offload()`'s lazy-once
  connect) -- same cold-model-load rationale, no new design needed
  there.

**Verified two ways, deliberately not three, same split Thesaurus
established:**
1. `wp_atari_grammar_check_selfcheck()` (run via `ci_atari.sh`) proves
   the three new pure-logic pieces against a hand-built fixture: a
   2-run paragraph ("plain " + "bold", built directly via the
   structural mutation API rather than through `format_txt_import`,
   which never produces more than one run) proves
   `extract_current_paragraph_text()` concatenates across the style
   boundary correctly, and proves `map_paragraph_offset_to_run()` both
   accepts a range fully inside one run AND declines a range straddling
   the boundary. A hand-built canned response buffer proves
   `decode_grammar_issue_at()`'s wire-walking is correct, mirroring
   `host/tests/test_offload.c`'s `grammar_issue_at()` on the host side
   but exercised here as this app's own independent decode loop, on
   real m68k hardware emulation.
2. The `offload_client.c` <-> real-gateway round trip is NOT re-proven
   here -- `OFFLOAD_GRAMMAR_CHECK` is submitted through the exact same
   generic `offload_submit()` path every other op uses (no per-op wire
   framing), and that path is already proven correct over real emulated
   RS-232, for arbitrary payload sizes, by T6.3's `atari_serial_smoke.c`
   and T7.4's `atari_convert_smoke.c`. Re-proving it a third time for
   this one specific op would add nothing new.

**Known gap, honestly not closed here, same shape as every offload
action before it:** `do_grammar_check()` itself has never been run and
observed end to end -- it ends in a loop of blocking `form_alert()`
calls, which this headless environment cannot safely drive to
completion (see the SIGPIPE incident this document describes at length
under T6.3). Verified instead, transitively: the three pieces above are
each independently proven, and `wp_edit_delete_range`/
`wp_edit_insert_text` are proven independently by the engine's own host
test suite (`host/tests/test_edit.c`) and by this app's own
`do_backspace()`/`do_insert_char()` already running correctly under
`ci_atari.sh`. `do_grammar_check()` is a mechanical composition of all
of these plus dialog/button-index handling that has no CI-safe way to
be exercised -- not a smaller gap than Thesaurus's identical one, and
not silently different in kind.

### Offline fallback: `services/grammar` (M8 T8.2)

CLAUDE.md's Offload Rules are explicit that the engine must stay fully
functional with no offload device attached. Before T8.2, "no device
attached" for Grammar Check meant `do_grammar_check()` failed outright
with a "No offload device available" alert right after the paragraph-
extraction step -- functional in the sense that it didn't crash, but
not a working feature. `services/grammar/grammar_lite_check()` (its own
design doc: `docs/services/grammar.md`) is what it falls back to now: a
small, purely mechanical, rule-based checker (doubled words, multiple
spaces, space before punctuation, repeated `!`/`?`, a missing paragraph-
initial capital) that needs no network, no LLM, and no semantic
understanding of the text at all.

**The fallback is announced, not silently swapped in.** When
`ensure_offload()` fails, `do_grammar_check()` shows one
`form_alert()` ("No offload device attached. Using basic on-device
grammar checks instead.") before running `grammar_lite_check()`, so the
user knows they are getting a narrower check than the real AI-backed
path, not a silent quality downgrade.

**One struct refactor made this a clean fallback instead of a parallel
code path.** `wp_atari_grammar_issue` originally stored `message`/
`suggestion` as raw pointers into the offload response buffer
(`resp.result`) plus explicit lengths -- tied to that buffer's lifetime
and shape. It now stores both in small fixed, owned buffers (`char[80]`,
sized deliberately larger than `WP_ATARI_GRAMMAR_DISPLAY_LEN` since the
SUGGESTION field is what actually gets inserted into the document on
Apply, not just displayed), filled either by `decode_grammar_issue_at()`
(the wire path) or directly from `grammar_lite_check()`'s own output
(the local path). Every downstream piece -- the ascending-offset sort,
the overlap guard, the run-boundary mapping, the Apply/Skip/Stop dialog
loop -- operates on this one common shape and has no idea which source
an issue came from. A `used_offload` flag gates only the two things that
genuinely differ: whether `offload_response_free()` needs to run at the
end, and whether the request/response round trip or the local scan
populated `issues[]` in the first place.

**Scope boundary: this only covers "no device attached," not "the
device failed mid-request."** A gateway that connects successfully but
then times out or returns `OFFLOAD_UNSUPPORTED` still gets the original
hard-failure alert, not a fallback -- T8.2's own AC is specifically "no
device attached," and conflating a live gateway's failure with an
absent one would hide a real problem (a misconfigured or crashed
gateway) behind what looks like normal degraded-mode operation.

**Verified two ways:**
1. `services/grammar`'s own rule logic is exhaustively covered by
   `host/tests/test_grammar.c` (part of `make test`) -- one positive/
   negative pair per rule plus a combined-paragraph test, all offsets
   verified with `python3 -c "print(s.index(...))"` rather than hand-
   counted (`docs/services/grammar.md`).
2. `wp_atari_grammar_check_selfcheck()` (run via `ci_atari.sh`) now
   ALSO makes one real call into `grammar_lite_check()` and checks its
   output -- not redundant with (1): that suite proves the rule logic
   exhaustively but only as compiled by the host `cc`, while this proves
   the exact same source still behaves correctly once cross-compiled
   for m68k and actually run under real CPU emulation, the same
   "don't assume host behavior transfers" check
   `wp_atari_rtf_spell_selfcheck()` already applies to
   `spell_check_word`/`spell_suggest`.

The `do_grammar_check()` wrapper's own untested-by-CI status (above) is
unchanged by any of this -- the fallback branch inside it is exercised
by the same transitive reasoning as the offload branch always was.

## Chat wired into the UI (M8 T8.3)

**IMAGE is deliberately not part of this.** M8's own milestone text
pairs "CHAT / IMAGE ops behind capability flags," but this app has no
image type anywhere -- not in the document model, not in any format
importer/exporter, and not in `platform.h`'s draw seam (`plat_draw_text`
is the only drawing primitive that exists). Building IMAGE's wire
plumbing with nowhere to put the resulting bytes would be a checkbox
feature, not a real one -- see `docs/formats/offload-protocol.md`'s own
`OFFLOAD_IMAGE` section, including a design note for whenever this
project's targets can genuinely support it (a real IMAGE feature would
need the GATEWAY to downconvert to something each vintage target can
actually display -- ST low-res is 16 colors from a 512-color palette,
and the other planned platforms have their own, often tighter,
ceilings -- not just ship whatever a modern generator produces).

**A new fourth Tools item, Tools > Chat**, appended after Grammar Check
(`WP_MENU_OBJ_TOOLS_CHAT`) in the hand-built `OBJECT[]` tree -- same
"append, don't insert" discipline as every earlier menu growth in this
file. `wp_menu_tree_selfcheck()` grew the matching linkage assertions.

**`do_chat()` reuses two already-proven pieces instead of inventing
new ones.** The prompt is the current paragraph, via
`extract_current_paragraph_text()` (Grammar Check's own extraction
helper, T8.1) -- not a purpose-built free-text input field. This
project's toolchain note (above, "no resource editor... a hand-built
`OBJECT[]` array is the practical choice... `rsrc_load()` would be the
better tool for anything with real dialog layout, which this project
doesn't need yet") already named a real multi-field GEM text-entry
dialog as out of scope for anything built so far, and Chat doesn't
change that: reusing an already-tested extraction path costs nothing
new, where a genuine text-input dialog would be real, unproven GEM UI
machinery for a milestone whose own AC is about capability GATING, not
input ergonomics. The reply is shown read-only via `form_alert()` --
Chat never mutates the document, matching Thesaurus's "look something
up, show it, done" pattern rather than inventing an insert-into-
document flow.

**The actual point of T8.3, and the one genuinely new piece of client
behavior:** `do_chat()` is the first offload action in this app that
calls `offload_query()` and checks the CAPS handshake's
`ops_supported` bitmask BEFORE submitting a request. Every earlier
feature -- Thesaurus, Grammar Check, ODT/PDF -- submits unconditionally
and only handles `OFFLOAD_UNSUPPORTED` reactively, after a full request/
response round trip; functionally fine, but it wastes a round trip
against a gateway that could have said no immediately. A new
`offload_caps_supports(caps, op)` helper (`(caps->ops_supported &
(1uL << op)) != 0`) makes this check a one-line, easily-selfcheck-
tested call: a gateway that doesn't advertise CHAT gets an immediate,
honest "This gateway doesn't offer Chat" alert with zero network
traffic, genuinely satisfying "present only when the gateway
advertises it, absent gracefully otherwise" rather than approximating
it after the fact.

**Verified two ways, same split as every offload action before it:**
1. `wp_atari_chat_selfcheck()` (run via `ci_atari.sh`) proves
   `offload_caps_supports()` against a hand-built `offload_capabilities`
   value with a known bitmask (CHAT set, THESAURUS set, GRAMMAR_CHECK
   and IMAGE clear) -- there is little else new and non-blocking to
   test here, since prompt extraction reuses Grammar Check's already-
   selfchecked `extract_current_paragraph_text()` and the response has
   no structure to decode at all (the reply is shipped back verbatim).
2. `host/tools/ollama_gateway_smoke.c`, extended to exercise all three
   real ops over one connection, confirms against a LIVE Ollama server
   that `ollama_gateway.c` genuinely advertises `OFFLOAD_CHAT` (and
   genuinely does NOT advertise `OFFLOAD_IMAGE`) and that a real prompt
   ("In one short sentence, what is a good synonym for happy?") gets a
   real, sensible reply back through the exact same `offload_client.c`
   API this menu action uses. All 15 checks passed on a live run,
   including the two new capability-advertisement assertions.

**Known gap, honestly not closed here, same shape as every offload
action before it:** `do_chat()` itself has never been run and observed
end to end -- it ends in `form_alert()`, a real blocking AES call this
headless environment cannot safely drive to completion. Verified
instead, transitively: `offload_caps_supports()` (selfcheck above),
`extract_current_paragraph_text()` (Grammar Check's own selfcheck), and
the `offload_init`/`offload_query`/`offload_submit`/`offload_shutdown`
sequence (proven generic over real emulated RS-232 by T6.3/T7.4) are
each independently proven; `do_chat()` is a thin, mechanical composition
of all three plus string formatting, not independently re-verified end
to end.
