# DOS Platform Backend (M14)

`platform/dos` is RetroWP's fourth real `platform.h` backend, targeting
plain 16-bit real-mode DOS text mode (no VGA graphics mode assumed) --
architecturally the most different target yet: a fixed 80x25
character-cell screen, no window manager, no proportional fonts, and no
OS-level clipboard. This document covers the toolchain, the DOSBox CI
mechanism, and the honest gotchas hit standing both up -- written
incrementally per phase, matching `docs/mac-platform.md`'s/
`docs/amiga-platform.md`'s own convention.

## Toolchain

[Open Watcom v2](https://github.com/open-watcom/open-watcom-v2) is the
real, actively-maintained fork of the classic Watcom C compiler,
cross-compiling directly from Linux to genuine 16-bit real-mode DOS
`.EXE`/`.COM` files (`-bt=dos`) -- the same "modern cross-compiler, real
vintage target" shape `m68k-amigaos-gcc`/Retro68's `m68k-apple-macos-gcc`
already established, not a resurrection of the actual historical Watcom/
Turbo C binaries CLAUDE.md names only as *style* precedent. Installed via
the AUR `openwatcom-v2` package (a binary distribution, actively
maintained) into a user-local prefix, `$HOME/opt/watcom` -- extracted
directly from the built package rather than `pacman -U`'d system-wide,
since that needs no root/sudo step and matches `AMIGA_TOOLROOT`'s own
user-local-prefix convention more closely than a system package would.

Unlike gcc-family cross-compilers, Watcom's compile (`wcc`) and link
(`wcl`) are separate tools with their own flag dialect: `-bt=dos` (target
OS), `-za -zastd=c89` (strict ISO C89), `-we -wx` (warnings as errors, at
max level), `-0` (8086 baseline, matching this project's widest-
compatibility ethos). `wcl` additionally needs the `WATCOM` environment
variable set and `wlink` on `PATH` to locate its own runtime libraries at
link time -- confirmed directly (a clean-environment link fails with
"cannot open clibs.lib" otherwise), not assumed.

**Memory model is `-mm` (medium: far/multi-segment code, near/single-
segment data), not `-ms` (small)** -- found directly, not assumed: linking
the full engine (every format importer/exporter, all five keybinding
schemes) under `-ms` overflowed the small model's one 64KB code segment
by ~16KB at the very first real link attempt. Medium model's per-function
far calls solve that; the document model's own data isn't yet large
enough to need far data pointers too (`-ml`, large model -- a real
escalation this project would rather avoid unless something concrete
forces it).

## DOSBox CI mechanism

`dosbox`, `dosbox-x`, and `dosbox-staging` are all real, current, and
already installed. Automation turned out to be **simpler** than Hatari's/
amiberry's/BasiliskII's own bespoke CI work, because DOSBox's own CLI
already provides everything needed as documented, built-in flags:

- `--exit`: terminates the DOSBox process cleanly once its `-c` commands
  finish -- no fixed sleep-then-kill, no Native-Features-style exit trap.
- Directory mounting (the `MOUNT C "<path>"` command, or the bare `PATH`
  argument): a **live passthrough** to a real host directory, not a
  copied disk image -- a DOS program's own file write lands on the real
  host filesystem in real time, with no bespoke shared-folder-emulation
  setup needed at all.
- `--noprimaryconf --nolocalconf`: bypasses the user's own personal
  DOSBox config (`~/.config/dosbox/dosbox-staging.conf`, `~/.dosbox`,
  `~/dosbox.d` -- a real, personal setup found on this machine, off-
  limits for CI the same way the Mac/Amiga milestones' own personal
  emulator setups were) entirely, a hard guarantee rather than just a
  careful-paths convention.

`host/tools/dos_ci_lib.sh`'s `run_dos_smoke_once` boots `bin/dos/
SMOKE.EXE` under DOSBox (Xvfb-backed, matching every prior backend's
headless-display precedent) with a fresh scratch directory mounted as
`C:`, and reads pass/fail from a plain log-file convention (mirroring
`amiga_smoke.c`'s own `report()`-to-a-file pattern) rather than an
emulator exit-code trap -- `host/tools/ci_dos.sh` is the resulting real,
passing CI entrypoint.

## Architecture fit

`platform/host/metrics_host.c`'s fake-monospace stub, not any of the
three real GUI backends' own proportional-font metrics, is the closest
existing structural precedent for `metrics_dos.c`: plain BIOS text mode
has exactly one fixed ROM character cell, no real size/style variation.
`plat_dialog_choice`'s own doc comment in `engine/include/platform.h`
already named "a numbered text prompt on DOS" as the anticipated shape
for this target, predating this milestone.

## Phase 1: video mode, direct video-memory rendering, keyboard event loop

`platform/dos/app_shell.c` sets 80x25 text mode (`INT 10h` AH=00h),
writes directly to `B800:0000` video memory (the standard, expected DOS
text-mode technique -- BIOS teletype output is character-by-character
slow, never how a real vintage word processor would draw a full screen),
and reads keys via `INT 16h` -- all through Watcom's `<i86.h>`
(`int86`/`union REGS`/`MK_FP`), not inline assembly. No engine/platform.h
linkage yet at this phase; deliberately as raw and minimal as Phase 0's
own smoke test.

### Verified

A new headless-DOSBox keystroke-injection technique was found this
phase (`xdotool key --window <id>` + `import -window <id>` for
screenshots -- no window manager needed under bare Xvfb; `xdotool
windowactivate` fails there but is harmless to ignore since the window is
already the only one, and key delivery works regardless). Confirmed on
real captured screens: printable-character echo, Enter (new line),
Backspace (erase), and Escape (clean exit -- screen clears, control
returns to the DOS prompt).

## Phase 2: fixed-cell metrics, direct video-memory drawing, file/clipboard/timer

`metrics_dos.c` uses a real, historically-grounded monospace convention
-- 10 characters per inch, 6 lines per inch (144/240 twips per cell), not
an arbitrary ratio -- and deliberately ignores a font's requested
`size_twips` for measurement/rendering (plain BIOS text mode has exactly
one fixed ROM cell; a documented gap, not silent). `draw_dos.c` writes
straight to `B800:0000`, with a real nearest-color match across the 16
standard CGA/EGA/VGA colors (genuinely more than the monochrome ST/Mac
backends can do), and BOLD maps to the intensity attribute bit (the
standard DOS-text-mode convention) -- italic/underline have no real
visual representation in this mode and are an honest, documented gap.

`file_dos.c`/`clipboard_dos.c`/`timer_dos.c` turned out to be a **hard
prerequisite for Phase 2 itself**, not deferred work: every backend
links its whole `ENGINE_OBJ` as one indivisible unit, so simply calling
`wp_paragraph_relayout` pulls in the entire engine, including
`offload_client.c`'s references to `plat_transport_*` -- meaning transport
(`transport_dos.c`) also had to exist, at least as a stub, before
anything could link at all. `file_dos.c` is a near-verbatim port of
`platform/atari/file_atari.c` (both just wrap the cross-compiler's own
standard libc `fopen`/`fread`/`fwrite`/`fseek`/`ftell`/`remove`).
`clipboard_dos.c` mirrors `platform/host/clipboard_host.c`'s own in-
process-buffer shape exactly -- DOS is single-tasking with no OS-level
clipboard service to integrate with, but `platform.h`'s clipboard
interface never mandates cross-app sharing, only persistent storage
during a session, so this is a fully conformant implementation, not a
shortfall. `timer_dos.c` mirrors `platform/atari/timer_atari.c`'s own
libc `clock()` use exactly.

### Three real bugs found and fixed

1. **Small memory model overflow** (see Toolchain above) -- switched
   `-ms` to `-mm`.
2. **A silent stack overflow**, found directly, not by inspection:
   `wp_paragraph_relayout`'s own 512-entry `token_t` stack array
   (`engine/src/linebreak.c`) overflowed Open Watcom's default DOS
   stack, crashing the guest partway through the very first relayout
   call with no error message at all (DOSBox itself exited cleanly,
   `--exit`'s own contract, giving no hint anything had gone wrong).
   Root-caused by adding incremental progress markers to a debug log
   file and bisecting exactly which call never returned. Fixed with a
   real, larger linked stack (`wcl -k32768`), not by shrinking the
   engine's own token buffer -- this is exactly the kind of host-only
   assumption (a modern `ulimit -s` most systems set generously) this
   project's cross-compile discipline exists to catch.
3. **`plat_window_create` clearing the screen it was called on top of**:
   found during Phase 2's own visual verification (a title bar drawn at
   startup disappeared once `render_fixture_paragraph`'s own
   `plat_window_create` call ran). Root cause: BIOS `INT 10h`'s video-
   mode-set service always blanks the display as a side effect, even
   when re-requesting the mode already active -- `plat_window_create`
   was redundantly re-invoking it. Fixed by never setting video mode in
   `plat_window_create` at all; that responsibility belongs solely to
   `app_shell.c`'s own one-time startup sequence.

### Verified

`wp_dos_layout_selfcheck` (mirroring the other three backends' own
selfchecks) asserts **exact** hand-computed line widths, not just
bounds -- possible here, unlike on real GDOS/diskfont/QuickDraw fonts,
precisely because DOS's cell is exactly fixed. `wp_dos_file_clipboard_
selfcheck` (mirroring `wp_atari_file_clipboard_selfcheck`'s exact shape)
proves a real file and clipboard round trip. Both pass on real DOSBox via
`ci_dos.sh`. `render_fixture_paragraph` was additionally verified via a
real captured screenshot: a fixture paragraph, laid out by the real
engine, wraps and renders correctly at a real column width through the
actual `plat_draw_text` seam -- not just `wp_dos_layout_selfcheck`'s own
pass/fail claim.

## Phase 4: serial transport (INT 14h)

`transport_dos.c` implements the offload transport seam over BIOS `INT
14h`'s classic serial services (`AH=00h` init, `01h` send, `02h`
receive, `03h` get status), mirroring `transport_atari.c`'s role and
`"serial:<device>:<baud>"` endpoint convention exactly (`<device>` here
is the BIOS COM port number, 0-3 for COM1-4 -- real PC hardware can have
several, unlike the ST's single fixed AUX port). Zero changes to
`offload/transport`, `offload_client.c`, or `offload.h` -- the same
drop-in-Nth-implementation property every prior backend already proved.

### A real, confirmed DOSBox limitation, not a bug in this code

DOSBox's default `serial1=dummy` device (confirmed via `config
-writeconf`, not assumed) is meant to behave as a real, if disconnected,
UART -- and its `AH=03h` status poll does: it correctly reports
"transmit holding register empty, ready to send" (`0x60`) immediately
after init. But `AH=01h` (send) unconditionally returns `AH=0x80`
(timeout) regardless of that ready status, for every byte, every time.

Reproduced and isolated with a minimal, from-scratch raw `INT 14h`
program with no dependency on this backend's own code at all (just
`init` -> loop of `send` calls), ruling out any subtlety in
`transport_dos.c`'s own polling logic. Two further hypotheses were
tested and both ruled out directly: CPU-speed-dependent BIOS retry-loop
miscalibration (a real, known class of DOSBox/BIOS timing issue) --
tested at `cpu_cycles=330` (era-appropriate speed), same failure; and a
missing modem-control handshake (DTR/RTS/OUT2 never raised, since base
`INT 14h` has no dedicated call for it) -- tested via a direct Modem
Control Register write (`outp(0x3FC, 0x0B)` for COM1), same failure
regardless. `serial1=nullmodem` was also tried as an alternative backend,
but its own TCP-server-socket startup blocks waiting for a peer to
connect, adding real complexity without yet proving the underlying send
path works either.

This is a real limitation of DOSBox's own BIOS-level serial emulation for
its `dummy` device specifically -- not a bug in `transport_dos.c`'s use
of `int86`, not a fixable timing or handshake issue on this side. The
same category of finding as Mac's own BasiliskII serial-read-hang gap
(`docs/mac-platform.md`'s Phase 4): a genuine emulator limitation,
honestly documented rather than silently worked around or hidden behind
a weakened implementation.

Because of this, `wp_dos_transport_selfcheck` (mirroring
`wp_amiga_transport_selfcheck`'s shape) still calls `plat_transport_
write` for real (proving it returns a well-defined status without
hanging or crashing) but does not assert `WP_OK` from it. What it does
assert as hard requirements, all fully verifiable in this environment:
open succeeds; a subsequent read correctly times out (nothing is
attached to echo a response) within the deadline logic's own generous
bounds; and -- with no live gateway attached -- `offload_init` fails
within its documented budget (`CONNECT_TIMEOUT_MS` + `HANDSHAKE_TIMEOUT_
MS`, `engine/src/offload_client.c`) and `offload_available`/
`offload_shutdown` behave correctly.

### Verified

`wp_dos_transport_selfcheck()` passes reliably via `ci_dos.sh`
(`transport: PASS`). `make dos-engine-check`, `make test` (376/376), and
the Atari/Amiga/Mac engine-checks all stay green throughout.

## Phase 5: `platform/common` retrofit -- keys, actions, status, help

`app_shell.c`'s Phase 1 raw keyboard-echo loop is replaced with the real
thing: BIOS `INT 16h` scan+ASCII+shift-state translated to a portable
`wp_key_event` (`dos_translate_key`) and fed through `platform/common/
keymap.c`'s dispatch -- the same vi-mode/WordStar/Emacs/WordPerfect/MS
Word schemes every other backend already uses, zero scheme-side
changes. Real PC scan codes (set 1) turned out nearly identical to the
ST's own IKBD table `atari_translate_key` already uses -- not a
coincidence: the ST's scan codes were themselves modeled on the PC's.
`dialog_dos.c` implements `plat_dialog_message`/`plat_dialog_choice` as
a modal, one-line prompt on the bottom screen row -- the "numbered text
prompt on DOS" shape `engine/include/platform.h`'s own doc comment
already anticipated, predating this milestone. File Open/Save use their
own free-text filename prompt (`prompt_for_filename`), since
`plat_dialog_choice` explicitly has no free-text variant, matching how
every other backend's own file picker is entirely backend-supplied
too. The document body paint loop was built on the real `wp_line_next_
fragment`/`wp_justify_space_extra_twips` fragment walk from day one
(T13.2's own shared primitive), not the older whole-line-buffer
approach the other three backends each had to later fix -- column
advances are a plain byte count rather than a `plat_measure_text` call,
correct specifically because DOS's font is genuinely, always monospace.

### A real bug found and fixed: text you typed never appeared

The single most significant bug found this phase. Typing a character
correctly reached the document model (`wp_edit_insert_text`, a real
byte-level edit via vi-mode's own Insert handling) but never appeared
on screen. Root cause, confirmed by direct code search across
`engine/src/vimode.c`, `engine/src/edit.c`, and `platform/common/
keymap.c`: **nothing in the shared engine or keymap dispatch path ever
calls `wp_paragraph_relayout` after an edit** -- only a whole-document
replace (Cut/Paste, File Open) does, via `replace_document`'s own
`relayout_document` call. `platform/atari/app_shell.c`'s own
`paint_document` has the identical shape: it walks `para->layout`
directly with no relayout call of its own anywhere in the per-keystroke
path either. This was found here because Phase 5 was the first time
this project screenshot-verified live, interactive typing end-to-end
rather than just a preloaded fixture -- it is plausibly a **latent gap
on Atari/Amiga/Mac too**, not something specific to DOS, simply never
caught there because their own interactive verification never happened
to type new characters into an already-loaded document and check the
result on a real screen. Recorded here as a real, honest finding for
whoever next touches those backends' own editing loops, not silently
assumed to be DOS-only.

Fixed for DOS by relayouting the whole document once before every
paint (`redraw_screen`, right before `paint_document_body`) -- not the
cheapest fix (`wp_document_paginate`'s own `layout_dirty` check would
redo only the actually-edited paragraph), but correct, and the sample
document is short enough that the cost isn't observable.

### Another real bug found and fixed: invisible status-line text

`wp_statusline_paint` calls `plat_draw_text` with black (0,0,0) text
over its own just-painted light-gray background fill. `plat_draw_text`
was unconditionally forcing a **black background** on every cell it
touched (`attr = fg & 0x0F`, background nibble always 0) -- fine for
document-body text (always drawn on an already-black cell), but it
silently overwrote the status bar's own light-gray fill with solid
black-on-black, making the mode text completely invisible. There is no
equivalent failure mode on a real bitmap display (Atari/Amiga/Mac just
draw glyph strokes over whatever pixels were already there), but a DOS
text-mode cell has no such thing as drawing "just the foreground" --
the whole cell's attribute byte is replaced on every write. Fixed by
reading back each cell's *existing* attribute byte first
(`dos_get_cell_attr`) and preserving its background nibble, only ever
replacing the foreground.

### Verified

Interactively confirmed on real captured DOSBox screenshots, not just
`wp_dos_*_selfcheck`'s own pass/fail: real vi-mode Insert-mode typing
appears correctly and the status line correctly switches `NORMAL` <->
`INSERT`; the help pager shows real compiled-in content, real link
navigation (`j`/`k`/Enter/Backspace), and a working hint bar; Ctrl+Q
cleanly quits back to the DOS prompt. Three new selfchecks pass via
`ci_dos.sh`: `wp_dos_key_translate_selfcheck` (scan-code table against
real PC scan-code-set-1 values, mirroring `wp_amiga_key_translate_
selfcheck`), `wp_dos_config_selfcheck` (real file round trip, all five
`keymap=` values, mirroring `wp_atari_config_selfcheck`), and
`wp_dos_help_selfcheck` (real compiled-in help content + real wrapping,
mirroring `wp_atari_help_selfcheck`). `make test` (376/376) and the
Atari/Amiga/Mac engine-checks all stay green throughout.

## Phase 6: CI matrix + docs + milestone closeout

`host/tools/ci_dos_matrix.sh` mirrors `ci_atari_matrix.sh`'s/
`ci_amiga_matrix.sh`'s/`ci_mac_matrix.sh`'s own shape: build `SMOKE.EXE`
once, then boot it under every DOSBox `--machine` profile in a curated
list, printing a per-profile PASS/FAIL line.

### A real, confirmed exclusion: Hercules

The M14 plan's own original scope named "hercules/cga/ega/svga" as the
matrix. Hercules was tried first and produces a **solid black screen**
on real `RETROWP.EXE` (not just SMOKE.EXE's own selfchecks) -- confirmed
directly via a headless-DOSBox screenshot, not assumed. DOSBox's own log
explains why: `--machine hercules` reports "Hercules 640x350 monochrome
text mode 07h", not the 80x25 16-colour mode 03h this backend's
`set_text_mode()` requests via `INT 10h AH=00h AL=03h`. Real Hercules
adapters never supported color text mode 03h at all -- only mode 7 (MDA-
compatible monochrome text, matching real historical Hercules hardware)
and their own bundled graphics mode -- and mode 7's own video memory
lives at segment **B000**, not the B800 segment `draw_dos.c` hardcodes.
This is a genuine, structural CGA/EGA/VGA-vs-MDA/Hercules incompatibility,
not a bug in this code: the Architecture-fit section above already named
MDA/Hercules mono-at-B000 as a real, documented, not-required-for-v1
alternate before this phase ever ran, so this confirms a known, already-
scoped-out gap rather than surprising one. Same category of finding as
Amiga's own Kickstart 3.0 exclusion (`docs/amiga-platform.md`) and
Atari's TOS 1.00 exclusion (`docs/atari-platform.md`): a real structural
incompatibility, honestly excluded with a documented reason, not silently
dropped from the list.

`cga`/`ega`/`svga_s3` are the three real DOSBox machine profiles that
share mode 03h's color/B800 model this backend actually implements --
confirmed via real screenshots of `RETROWP.EXE` under all three (correct
title bar, word-wrapped sample document text, and status line, not just
`ci_dos_matrix.sh`'s own selfcheck pass/fail) before the matrix script
was written.

### Verified

`sh host/tools/ci_dos_matrix.sh`: `DOS MACHINE MATRIX: 3 passed, 0
failed` (cga, ega, svga_s3). Full regression sweep, all green: `make
test` (376/376); `ci_atari_matrix.sh` (6/6 TOS images); `ci_amiga_matrix.sh`
(3/3 Kickstart ROMs); `ci_mac_matrix.sh` (2/2 Mac ROMs). `git diff --stat
-- engine/ services/` shows exactly one file, `engine/include/layout.h`
-- pre-existing, unrelated T13.2 (M13a) work already on disk before this
milestone began (the shared `wp_line_next_fragment`/
`wp_justify_space_extra_twips` primitives DOS's own Phase 5 paint loop
reuses, not introduced by it); confirmed via `git log` that this hunk
predates M14 entirely. No `engine/`/`services/` file was touched by any
DOS phase.

## Status

**Proven, Phases 0-6, M14 complete**: the Open Watcom + DOSBox +
headless-Xvfb pipeline builds and runs real 16-bit DOS code; the whole
portable engine links and runs correctly against a genuinely fixed-cell,
monospace-only, no-clipboard-service target; file, clipboard, timer, and
(with the one documented DOSBox-side limitation above) serial transport
all work; the shared keymap/action/status/help retrofit is real,
interactively verified, and found two real bugs along the way (one of
them plausibly shared with the other three backends); the machine-type
matrix passes across every color-text DOSBox profile this backend
targets, with Hercules's real, structural incompatibility confirmed and
honestly excluded rather than silently skipped. `engine/`/`services/`
remain untouched by this milestone, the same portability proof M9/M12
each gave for Amiga/Mac.

## Real-app CI gate, and the empty-page defect it found (T169, T171)

`ci_dos_matrix.sh` booted `SMOKE.EXE` and nothing else. That binary
links the same engine but never enters text mode, never paints a screen
and never reads a key, so an app completely broken for a user passed
3/3. `run_dos_app_once` (`host/tools/dos_ci_lib.sh`) now boots the real
`RETROWP.EXE` against every machine profile, asserting the same three
signals the Atari gate documents in full (startup marker, event count
advancing under injected keystrokes, painted-fragment count nonzero).

Two DOS-specific notes:

- The gate cannot use `--exit` the way `run_dos_smoke_once` does: the
  real app never returns, so DOSBox is backgrounded and killed once the
  signals are read.
- The mounted `C:` is a live host-directory passthrough, so both markers
  are read straight off the host with no shared-folder setup -- the same
  property that made `DOS_CI.OUT` work for the smoke gate.

### What it found immediately: the document body is never painted

On its first real run the gate went red on **all three** machine
profiles (`cga`, `ega`, `svga_s3`) with:

    startup sentinel: present
    still responsive: yes
    painted text fragments: 0
    rendered document: NO -- app came up but painted an EMPTY page

The app draws its title bar and its `NORMAL` status line and responds to
every keystroke, but the document body is blank. `docs/screenshots/
dos.png`, this project's own committed reference shot, shows the sample
paragraph rendering correctly -- so this is a regression from a state
that once worked, not a limitation.

Confirmed **on pristine HEAD** (`git checkout HEAD -- platform/dos/
app_shell.c`, rebuilt, booted), with no CI trigger file present so the
gate's own hook is inert, and with no keystrokes injected. It is not an
artifact of the new gate and not a screenshot-timing artifact.

Temporary paint-loop counters reported through the marker file gave:

    para=1  line=0  draw=0  fontfail=0  paratotal=1  width=11520

The whole document has exactly **one** paragraph (every other backend's
sample has several), that paragraph has a layout object with **zero**
lines, and the paint loop itself is healthy -- it is reached, opens
fonts without failure, and simply has nothing to walk. The column width
is correct at 80 x 144 twips. So the fault is upstream of painting, in
document construction or relayout. Tracked as its own task; note that
`relayout_document` is called as `relayout_document(g_content_width_
twips)`, passing the same global it then assigns, so its `width_changed`
test is always false and it relies entirely on `layout == NULL` or
`layout_dirty` -- a paragraph with a non-NULL but empty layout and a
clear dirty flag would be skipped forever, which matches `line=0`.

The smoke half of every profile still passes. That is exactly the point:
`SMOKE.EXE` cannot see any of this.
