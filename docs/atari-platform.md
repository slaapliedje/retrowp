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

**No true screen DPI without SpeedoGDOS.** The document model's `size_twips`
(1/20 point, used for both font size and physical page dimensions) has no
legitimate way to become a pixel count on a bare ST without a GDOS install
to query real device metrics. `ATARI_ASSUMED_DPI = 96` (in
`metrics_atari_internal.h`) is a fixed, documented simplification, not a
calibration. This is safe specifically because T4.2's AC ("measured widths
drive correct on-screen wrap matching the engine's layout") is an
*internal-consistency* property: `plat_measure_text` converts VDI pixel
widths to twips using this ratio, and `app_shell.c`'s paint code converts
the resulting layout's twips coordinates back to pixels with the *same*
ratio before calling `plat_draw_text` -- so wherever the engine says a line
breaks is exactly where it's drawn, regardless of whether 96 DPI matches any
real monitor. Proven quantitatively, not just by eyeballing a screenshot:
`wp_atari_layout_selfcheck()` (`platform/atari/app_shell.c`, exercised by
the CI smoke test) imports a short paragraph, relayouts it at a narrow
column through the real VDI seam, and asserts both `line_count > 1` and
every line's `width_twips <= content_width_twips` -- confirmed to actually
catch a regression (deliberately zeroed `plat_measure_text`'s output during
development and watched the check fail, then restored it).

**Ascent/descent approximation.** `plat_font_metrics_get` uses
`vst_point()`'s own `charh`/`cellh` outputs (`charh` as ascent, `cellh -
charh` as descent, leading always 0) rather than `vqt_fontinfo()`'s more
precise `distances[]` breakdown. Simpler and sufficient for layout
correctness (line height only needs to be self-consistent, not exactly
typographically correct); not revisited unless real typography (kerning,
precise baseline alignment across mixed styles) becomes a requirement.

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

### Known gap: menu title text renders as glyph noise in screenshots

`WP.PRG`'s "File" menu title renders as illegible pixel noise in
screenshot captures (`import -window root`), isolated (during the prior
spike investigation) to be independent of the tree's content/width/position
via a single-character-title control experiment -- likely a EmuTOS-desktop
or Hatari screenshot-capture interaction, not a tree-construction bug,
since `wp_menu_tree_selfcheck()` proves the tree's bytes are correct and
`menu_bar()` returns success live. Not investigated further; flagged here
so it isn't mistaken for a real rendering defect if a future task revisits
Atari screenshots.

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
