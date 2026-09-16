# RetroWP on gem4xe (Atari 8-bit)

**Status: RetroWP runs on gem4xe, under both SpartaDOS X and SpartaDOS 3.2.** The full
image loads (`APP_OK`), `wp_menu_tree_init` runs, and GEM draws RetroWP's menu bar
(Desk · File · Edit · Tools · Format · Help) and its document window, all from the ST's
unchanged `app_shell.c`. **Not working yet:** the window's work area isn't
painted. (The title bar was blank until gem4xe taught `WF_NAME` to bring a far title
down to bank `$00` at draw time; see below.) **The long-running "load failure" was
never a failure.** Every probe that reported one looked at the machine once, at a fixed
frame, before a slow floppy read had finished. See "What the load failure really was".
Everything after that section is history, including conclusions later withdrawn.

`make gem4xe` produces `bin/gem4xe/RETROWP.G4A`.

## What the load failure really was (2026-09-15)

For most of this work `RETROWP.G4A` appeared not to load. Later, a SpartaDOS 3.2 read
appeared to fail at exactly byte 24,577. **Neither happened.** Every probe that reported a
failure sampled the machine once, at a fixed frame count. gem4xe writes `sh_desk_blob`
(and, in older builds, `sh_desk_len`) only when a whole read has finished, so a read still
in progress reads as zero, which is indistinguishable from a failure.

The proof came from single boots sampled over time, on gem4xe's 02:10 build, a bare SD 3.2
floppy, and the patched emulator. `farmem.brk` advances one slice at a time during a read,
so a climbing break under a zero blob is a read in progress:

| file | still reading at | finished by |
|---|---|---|
| 24,576 bytes | frame 1,700 (break 8 KB in) | frame 1,800 |
| 24,577 bytes | frame 1,700 | frame 1,800 |
| 48,128 bytes | frame 1,800 (break 26 KB in) | frame 2,000 |
| `RETROWP.G4A`, 202,325 bytes | frame 2,400 (break 119 KB in) | frame 3,000 |

What that explains:

- **The "exact edge" at 24,577.** gem4xe's author sampled the exact 00:47 binary (md5-matched)
  every 25 frames. 24,576 bytes finished at frame 1,800 and 24,577 at frame 1,850; at
  frame 1,800 the larger read was still in progress (blob 0, break `$0493A4`). The
  byte-exact edge was two completion frames falling either side of a single sample. On the
  02:10 build both finish just before 1,800, which is why the edge seemed to vanish.
- **"SDX reads, SD 3.2 fails".** The SDX probe waited 2,400 frames and the SD 3.2 probe
  1,800, so the comparison moved the sampling window along with the DOS.
- **gem4xe's product disk passing.** Its gate polls instead of sampling once.
- **A gem4xe change appearing to remove the failure.** Nothing was removed; later probes
  simply waited 2,400 frames.
- **"RetroWP never loads" (`app_near` reading `$0000`).** A 202 KB read sampled at frame
  1,800, which finishes near frame 3,000.

**Withdrawn:** the SpartaDOS 3.2 read limit, the claim that the DOS was the variable, and any
suggestion of a bug, or a masked bug, in gem4xe's `far_read_file`. **Still standing:** the
emulator binary matters (`/usr/bin/AltirraSDL` lacks the 65C816 CPU fixes; at 12,288 bytes
and frame 1,800 the patched build read 3/3 and the unpatched one 1/3), Calypsi 5.18.1's
regression for images of 3 or more banks, and everything in the port itself.

**The lesson:** a probe that samples once at a fixed time cannot tell "failed" from "not
finished". Poll for completion, or sample a timeline, before calling anything a failure.

## Clean-kit measurements (2026-09-15)

> **Read with the section above.** Every "fails" in this section came from a probe that
> looked once at frame 1,800, before the read had finished. The numbers are what that probe
> saw, not what gem4xe did.

Taken with both confounds described further down removed: a GEM.COM built with
Calypsi **5.18** from a clean gem4xe tree, the **patched** emulator (`KEYRAW`
verified after a reboot), and a quiet machine (load about 1). The blob still lands at
`$0443A4`. Each probe is a truncated prefix of the 5.18-built `RETROWP.G4A`, staged
as `DESKTOP.G4A` on a SpartaDOS 3.2 floppy, with `sh_desk_len` read by symbol.

Series, three repetitions, interleaved:

| bytes | result |
|---|---|
| 12,288 | reads 3/3 |
| 48,128 | fails 3/3 |
| 49,152 | fails 3/3 |
| 65,536 | fails 3/3 |
| 202,327 | fails 3/3 |

Grid, one repetition each (every point has been deterministic on this setup):

| bytes | result |
|---|---|
| 16,384 · 20,480 · 24,576 | read |
| 28,672 · 32,768 · **34,652** · 36,864 · 40,960 · 45,056 | fail |

What this settles:

- **The failure is real.** The unpatched emulator accounted for yesterday's
  flakiness. It does not account for the failure, which remains with every confound
  removed.
- **It is not a bank crossing.** 48,128 fits inside the 48,220 bytes left in bank
  `$04` and fails 3/3. That was a prediction, and it was wrong.
- **It is not an SDFS map-page boundary.** Those fall at 23,808 and 31,744, and
  24,576 reads.
- **The DOS or the medium is implicated by measurement.** 34,652 is the exact size
  of gem4xe's own `desktop.g4a`, which loads off CF under SpartaDOS X. At that size
  a SpartaDOS 3.2 floppy fails.

### On this disk recipe, SpartaDOS 3.2 fails where SpartaDOS X reads

The same floppy images, the same bytes and the same blob address (`$0443A4`), with only
the DOS changed. SpartaDOS X is the vendor's emulator cartridge, with the disk as D1:,
set up the way gem4xe's `m14_sparta.py --sdx=` does it:

| probe | SpartaDOS 3.2 | SpartaDOS X |
|---|---|---|
| 12,288 | reads | reads (control) |
| 24,576 | reads | |
| **24,577** · 25,600 · 26,624 · 27,648 | fail | |
| gem4xe's real `desktop.g4a`, 34,652 | **fails** | **reads** |
| 48,128 | fails | reads |

- **SIO and the floppy transport are cleared.** SDX reads over the same medium.
- **The DOS takes part, but it isn't sufficient on its own.** With identical staging,
  SD 3.2 fails where SDX reads. But see the last point: a differently staged SD 3.2
  disk reads the same size.
- **The edge is exact: 24,576 bytes read, 24,577 fail.** The failure starts on the
  first byte of the 13th 2,048-byte read. 24,576 is `$6000`, or 192 SD 3.2 sectors;
  that's recorded as an observation, not a cause.
- **It is not established as a gem4xe product bug, and the evidence is against it.**
  An earlier draft said GEM.COM could not load its own desktop from an SD 3.2
  floppy. gem4xe's own SD 3.2 product disk (`build/gem-sp.atr`) boots that same
  34,652-byte `desktop.g4a` under SD 3.2 from floppy, with no cartridge, and passes
  its product gate. The differences are all in staging. The probe disks carry only
  GEM.COM, `DESKTOP.G4A` and the boot batch files in MAIN. The product disk puts
  the desktop under `GEM>`, alongside `GEM4XE.CFG`, `LANG.RSC` and more. The leading
  candidate is the pool's state when the desktop is read: `far_read_file` reads in
  slices of `min(pool_room(), 2048)`, and the other files change what's left.
  Unproven. Note that the edge falling at 24,576 doesn't settle it, because that's
  both 12 × 2,048 and 192 × 128, a slice count and a sector count at once.

### Under SpartaDOS X, RetroWP runs

The full `RETROWP.G4A`, using the same floppy recipe as the probes, booted with the SDX
cartridge:

| | |
|---|---|
| read | `sh_desk_len` 202,327, blob at `$0443A4` |
| load | `sh_runs` 1, `sh_lastrc` 0 (`APP_OK`), near region at `$4A00` |
| startup | `wp_menu_tree` at `$4B00`. Its BAR carries `$180` at 640×10, and the Desk title is 48 px wide with its string at `$509C`, in bank `$00`. So `wp_menu_tree_init` ran, and `WP_AES_NEAR` put the strings where gem4xe's ABI reads them. |
| screen | GEM draws the menu bar and the document window, closer and sizer included (`build/gem4xe-baseline/sdxfull-2.png`) |

That makes it a four-bank image, with a 168 KB far region and the large data model,
running through gem4xe's loader.

The committed source builds **202,325 bytes**, two fewer than the 202,327-byte image all
the measurements above were taken on. `mem_gem4xe.c` had been calling `memcpy` without
its prototype, and Calypsi compiles the prototyped call differently. That matters: the
call is `mem_realloc`'s copy, and an argument-passing mismatch there would corrupt a
growing document, on a path startup never exercises. The corrected image was booted
under SDX on gem4xe's rebuilt GEM.COM (02:10, which includes its `APP_E_READ` change)
with the same result: `APP_OK`, `wp_menu_tree_init` ran, menu bar and window drawn
(`build/gem4xe-baseline/committed/`).

What isn't right yet:

- **The window's title bar was empty, and is not any more.** It is worth keeping the
  history, because the fix is not RetroWP's. `app_shell.c:4847` passes a literal,
  `wind_set_str(window_handle, WF_NAME, "RetroWP")`, which under `--data-model=large`
  lives in far memory. Through gem4xe 0.1 the AES kept only the low word of that address
  and drew whatever sat there in bank `$00`; `50a6a0e` then refused a far title outright
  instead of truncating it. Either way the bar was blank, and the fix looked like
  RetroWP's: a persistent `WP_AES_NEAR` title. gem4xe has since taken the other route
  (`src/aes/wind.c`): `WF_NAME` and `WF_INFO` keep the **whole** address — 24 bits of it in a
  32-bit field, the top byte always zero — and
  `w_ptext` copies the string into bank `$00` **at draw time**, because the AES re-reads
  it on every redraw and a one-time bounce would freeze the title. So a large-data
  program's far title now works, and RetroWP needs no change. One limit worth knowing:
  a **far** title is drawn to 40 characters (the margin on both sides of bank `$00` cost
  the other 40). A **near** title is used where it lies and has no cap. "RetroWP" is
  nowhere near either, but a title that showed a document path would want the near
  route.
- **The window's work area isn't painted:** the desktop pattern shows through where
  the document should be.
- **The timer seam was wrong here, and is now its own file.** `timer_atari.c`
  implemented `plat_ticks_ms` and `plat_sleep_ms` on C's `clock()`, which is right on
  the ST and was meaningless on gem4xe: Calypsi's library supplied a `clock()` nothing
  drove, so it linked and returned nonsense from the day the port started. `timer_gem4xe.c`
  replaces it on this target — see "The timer" below for what differs and for the
  32-bit overflow the ST and DOS backends carried with it.
- **The window's work area isn't painted.** Not investigated yet. `WM_REDRAW` goes to
  `redraw_window()` (`app_shell.c:3785`), which draws through VDI calls that run
  on the ST but haven't been checked on gem4xe. gem4xe's `src/hello_app.c`
  is a known-good redraw loop to compare against: `wind_update(BEG_UPDATE)`, walk
  `WF_FIRSTXYWH`/`WF_NEXTXYWH`, intersect each rectangle with the dirty one, clip,
  and paint.

Scripts, logs, screenshots and the exact 202,327-byte baseline image are in
`build/gem4xe-baseline/`.

**`make gem4xe-check` now does all of that on its own** (`tools/ci_gem4xe.py`) —
the check this shell lacked while every other one had it, since it had only ever
been verified by linking. It stages RetroWP as `DESKTOP.G4A`, boots it under the
SDX cartridge, and **polls** instead of sampling: the lesson above, built in. It
waits for `sh_desk_len` and reports how many frames that took (about 3,200 here),
then asserts the read is the whole file, `APP_OK` with a near region,
`wp_menu_tree_init` having run (BAR spec `$180` at 640×10), and ink in the menu
bar's own rows.

Two things it deliberately does **not** assert. The blank title and the unpainted
work area are printed every run, because a gate that fails when somebody fixes
them is worse than no gate. And `sh_runs` is reported rather than checked: the
table above records 1, a live run says 0, because RetroWP is still in its event
loop rather than having returned — asserting the documented figure would have
failed a good run. The read size is compared against the file on disk rather than
a constant, so it followed the 0.2 rebuild from 202,325 to 202,424 by itself.

## What this target is

[gem4xe](../../gem4xe) is a port of GEM — the VDI, then the AES — to an Atari
XL/XE with a 65C816 accelerator (Rapidus), VBXE and Ultimate 1MB. It serves the
same AES that RetroWP's Atari ST shell already talks to.

So this is **not a new front end**. `platform/atari/app_shell.c` is the same file
for both machines; what differs is the CPU and the binding of the same calls.
RetroWP's ST shell makes 42 distinct AES/VDI calls and gem4xe serves 39 of them
outright.

An application links **no part of gem4xe**: it reaches the VDI, the AES and
GEMDOS through three `COP` gates. Through gem4xe 0.1 these were `$73`, `$C8` and `$01`. From 0.2 (master `01c2e17`) they are `$56` (VDI), `$41` (AES) and `$44` (GEMDOS), because `$01` belongs to Rapidus OS and `$80`–`$FF` are reserved by WDC.

## Where the differences live

Everything machine-specific is in `$(RETROPLAT)/backends/atari/`, beside the ST's
own files, selected by gem4xe's own include guard rather than a build flag —
what actually differs is which `gem.h` the include path found.

| file | what it is for |
|---|---|
| `gemcompat.h` | every place two GEMs are not the same shape (below) |
| `mem_gem4xe.c` | a real allocator over far arenas — the substantive one |
| `file_gem4xe.c` | `plat_file` on GEMDOS instead of stdio |
| `clipboard_gem4xe.c` | the scrap file, over GEMDOS and the scrap manager |
| `transport_gem4xe.c` | "no offload device", completely |
| `timer_gem4xe.c` | the tick, and a sleep that yields to the AES |
| `gemcompat_gem4xe.c` | the `evnt_multi` adapter |

`clib_gem4xe.c` was here too, holding `memcmp` and `memchr`; the kit writes both
itself now (under the same licence, for the same reason), so it is gone.

Each is self-guarding, so a wildcard sweep by the ST build or by retroplat's seam
gate compiles them to nothing rather than breaking.

### What `gemcompat.h` reconciles

- **`ob_spec`** — a union on the ST, a bare `LONG` on gem4xe. 79 sites in
  `app_shell.c` now go through `OB_SPEC_SET_STRING` / `OB_SPEC_IS_STRING` /
  `OB_SPEC_SET_INDEX` / `OB_SPEC_ZERO`.
- **`MFDB.fd_addr`** — `void *` on the ST; `uint32_t` on gem4xe, because a
  65816's `void *` under the small data model is sixteen bits and a shrunk first
  field would move every field after it.
- **`evnt_multi`** — gemlib takes ten loose words and a `LONG` timer; gem4xe takes
  two `MOBLK` pointers and the timer already split. A C89 macro has nowhere to put
  the `MOBLK`s, so the adapter is a function.
- **`MENU_INSTALL`, `CHECK`, `UNHIGHLIGHT`, `MFORM`, `FA_ERROR`, `FA_INFO`** —
  gemlib names for values gem4xe leaves as the numbers the AES documents.
- **`wind_create_grect` / `wind_open_grect`** — two gemlib wrappers gem4xe has not
  got yet. It carries five of their siblings; these belong upstream beside them.
- **`OB_SPEC_ZERO`** — `{0}` is valid C89 for a scalar, and Calypsi stops with
  "internal error: unhandled initializer" on it, so the brace is spelled only
  where it is required.
- **`WP_AES_NEAR`** — see below. The one that decides whether anything draws.

## The three findings that shaped the port

### 1. `Mfree` is a no-op

From gem4xe's own header (`src/sys/gemdos.h`):

> Malloc is the far heap: the bump allocator, wound back when the application
> exits, **so Mfree is a no-op and a leak lasts one run**.

That is the right contract for a calculator or a desk accessory. It is the wrong
one for an editor: RetroWP frees on every run split, every undo push, every
relayout, and over a session allocates many times the size of the document it
holds. Against a no-op free, 14.9 MB is not a large heap, it is a long fuse.

So `mem_gem4xe.c` uses `Malloc` for **arenas** and runs a first-fit allocator
inside them — `Malloc` a handful of times per run, `Mfree` never, which is the
usage gem4xe's heap is built for. Arenas are chained, so there is no ceiling.
Each ends in a zero-length USED sentinel that coalescing will not pass, so no
block can ever span the gap between two arenas.

### 2. The data model has to be `large`, and that is what `gemapp.scm` expects

`--data-model=small` leaves `void *` at sixteen bits, which cannot hold what
`Malloc` returns. `--data-model=large` puts every global in far memory, which is
the only reason this fits: an application's near memory is a pool in bank `$00`,
and RetroWP's statics are far larger than that.

This is not a workaround. `gemapp.scm` names the `far` and `zfar` sections for
exactly this model, and GACS — the other program gem4xe exists for — is written
to it too.

### 3. AES-facing data must be in bank `$00` — `WP_AES_NEAR`

gem4xe's ABI passes an address to the AES as sixteen bits and supplies the bank:
`near_of()` takes the low word and the AES reads it in bank `$00`. Under
`--data-model=large` a global goes far unless it says otherwise, so the menu tree
landed at `$051E24` and the AES read whatever was at `$1E24` in bank `$00`.
**Nothing traps; the menu simply does not draw.** `gemapp.scm` says so in as many
words: *"a program that hands the AES a tree or a string must declare those
`__near`"*.

`WP_AES_NEAR` is `__near` on gem4xe and nothing on the ST. It is on
`wp_menu_tree` and the 31 `g_text_*` strings; `wp_menu_tree` now links at
`$001100`. Near budgets rose to match (`BSS` 8192, `BITS` 3072) — GEM.COM gives
an application the pool from `$4800` to `$7FFF`, about 14 KB, so 11520 fits.

**This is not finished.** String *literals* passed straight to the AES —
`form_alert`'s alert strings, `wind_set_str`'s window title, `menu_register`'s —
still live in `cfar`, which is far. They need either a near bounce buffer or a
way to place literals near. That is the most likely reason the first draw does
not complete.

## Sizes, measured

| | |
|---|---|
| far code | 155,105 bytes, over 3 banks (`app-layout-n ... 3 1`) |
| far constants (`cfar`) | 10,903 |
| far statics (`zfar`/`far`/`ifar`) | 6,622 |
| near (bank `$00`) | 11,520 of GEM.COM's ~14 KB pool |
| `RETROWP.G4A` | 202,327 bytes |

A bank holds just under 64 KB, hence three. gem4xe's loader has been exercised on
a spilled link (its `test-m6`).

## How to run it

```sh
make gem4xe-check    # builds the disk, boots it under SDX, checks what it drew
```

or by hand, to watch it:

```sh
make gem4xe
cd ~/dev/gem4xe
python3 tools/mkspdisk.py "<a SpartaDOS 3.2 .atr>" build/gem.xex /tmp/rwp.atr \
    --sectors 2560 --name GEM.COM --boot GEM \
    --add ~/dev/retrowp/bin/gem4xe/RETROWP.G4A DESKTOP.G4A
AltirraSDL --disk /tmp/rwp.atr ...        # or gem4xe's tools/a8test/launcher.py
```

Staging RetroWP **as `DESKTOP.G4A`** makes GEM.COM's shell load it directly,
which is the shortest path to seeing it run.

**Take `gem.xex` from a quiet tree.** gem4xe's gate runs relink `build/gem.xex`
repeatedly, and a copy taken mid-link is torn in ways that fail like anything but a torn
file. Copy it when that tree is idle, or from a `make dist` artefact, and check the copy's
md5 — as the scripts in `build/gem4xe-baseline/` do — rather than reading `build/`
directly.

## Where it stops, exactly

Booting the above under Altirra (VBXE 1.26 + Rapidus, 1088K):

1. SpartaDOS boots, `STARTUP.BAT` runs `GEM`.
2. GEM.COM comes up — the AES is running, the pointer is drawn.
3. **The load of `DESKTOP.G4A` fails. RetroWP is never entered.**

The screen goes white, and an earlier draft of this document read that as RetroWP
running and failing to draw. It is not: it is GEM.COM's own state after a failed
load. Measured, not inferred — `app_near`, the system global gem4xe keeps so a
gate can find a loaded program's near base, reads `$0000`.

**The control matters:** gem4xe's own `m16_desk.g4a`, through the identical
harness and disk recipe, loads and draws its banner. So the harness, GEM.COM and
the disk staging are all sound.

### What is ruled out

- **Not the pool.** `$4800-$8000`, 14,336 bytes, read off the target and confirmed
  by gem4xe's author. A 7,424-byte near build fails identically to an 11,520-byte
  one.
- **Not the header.** `need` and `len` are equal exactly — mkg4a emits precisely
  `need` bytes, so every program has zero slack. (An earlier draft computed a
  12-byte slack on all three programs from `HDR_SIZE 20`; it is 32. Matching
  slack across three files read as corroboration when it was one constant error
  showing up three times.) Magic and version are fine; `app.c:148` has taken v2
  since `40b4888`.

`app.c` sets `app_near` at line 174, *before* `far_alloc_banks` at 176. So the
failure is at or before `pool_alloc` — `APP_E_SHORT` (which given the header
maths means a short read), `APP_E_POOL`, or `app_load_file` bailing earlier with
`APP_E_FILE`.

### First: the measurements below are suspect

Everything in this section was measured with `/usr/bin/AltirraSDL`, which is
**not** the emulator gem4xe's own gates use (`~/dev/altirra-patched/AltirraSDL`,
a different binary entirely). `tools/a8test/launcher.py` resolves
`ALTIRRASDL` from the environment and falls back to the bare name on `PATH`;
nothing was set, so it silently took the wrong one.

That build lacks AltirraSDL PR #88 — measured, not inferred. Booting each binary
and asking it directly:

    /usr/bin/AltirraSDL                KEYRAW ABSENT   -> no PR #88
    ~/dev/altirra-patched/AltirraSDL   KEYRAW PRESENT  -> has PR #88

and #88 carried KEYRAW, the Ultimate 1MB switches **and the two 65C816
native-mode CPU fixes** as one change. (The check needs no GEM.COM and no disk
of substance: `launch()` with any image, then `b.has_keyraw()`.) So these runs executed
65816 native-mode code on a core with both bugs, the second of which is

> an IRQ storm that pushes four bytes per fetch until the stack has wrapped
> through bank 0

— timing-dependent (gem4xe's own notes warn that a passing gate is *not*
evidence an emulator has the fix) and corrupting the whole of bank `$00`, which
is where an application's pool, `far_read_file`'s slice buffer, its locals and
the stack all live.

A flaky, load-correlated failure in a read loop whose buffer sits in bank `$00`
is precisely that bug's signature. It also explains the fact nothing else could:
why gem4xe's own 34,652-byte desktop reads fine on **its** emulator.

**The single-variable test is to change only the binary** — same GEM.COM, same
disk, same probe, `ALTIRRASDL=~/dev/altirra-patched/AltirraSDL`. That is the
first thing to run when gem4xe's kit is rebuilt. Every probe script in this
project's scratch harness now pins it.

The lesson worth keeping: when a harness rejects a command the tree's own
documentation says it supports, that is not an inconvenience to route around —
it is the environment saying it is not the environment you think it is. That
`KEYRAW` failure appeared hours before any of the measurements below and was
treated as a nuisance.

### The emulator does matter — measured

One probe file, one GEM.COM, one disk, interleaved, only the binary moving:

| | unpatched | patched |
|---|---|---|
| run 1 | 12,288 | 12,288 |
| run 2 | **0** | 12,288 |
| run 3 | **0** | 12,288 |

Patched 3/3, unpatched 1/3. So a meaningful part of what was measured below was
the CPU bug, not gem4xe.

### But the absolute sizes are contaminated too, by a second thing

The GEM.COM available when that toggle ran was built with Calypsi **5.18.1**,
which miscompiles gem4xe's loader for applications spanning three or more banks
(its author isolated it: byte-identical program, 5.18 system passes, 5.18.1
system fails). `far_read_file` lives in `src/sys/app.c`, the very object whose
code differs between the two compilers — so read thresholds measured on that
build are measuring two faults at once.

On 5.18.1 + patched emulator: 12,288 read; 48,128 / 49,152 / 65,536 / 202,327
failed. A tempting prediction died there and should not be trusted either way —
the blob lands at `$0443A4`, leaving exactly 48,220 bytes in bank `$04`, so
48,128 should have fitted and 49,152 should have crossed and failed. **Both
failed.** On a clean system that would have killed the bank-crossing story; on
that one it proves nothing.

The series is to be re-run on a 5.18-built kit. Until then RetroWP's own status
is "does not load, cause not established".

### What was measured on the unpatched emulator (historical)

The desktop is not loaded from a file by the shell's launch path at all:
`sh_ldapp` takes it from `sh_desk_blob`/`sh_desk_len`, which `sh_init`
(`shel.c:369`) fills with one `far_read_file` call. That call returns 0.

Because `sh_init` reads the file before anything validates it, the read can be
bisected with truncated files — the header check fails afterwards, but the read
is what is being measured:

| bytes | result | |
|---|---|---|
| 8,192 | reads | 3 runs, all pass |
| 10,240 | **unstable** | 2 passes early, 4 failures later |
| 10,244 | reads | one sample only |
| 10,752 / 11,264 / 11,776 | fails | |
| 12,288 | fails | 4 runs, all fail |
| 16,384 / 61,440 / 202,327 | fails | |

So: **at or below 8,192 it reads reliably; at or above 10,752 it never does; and
there is an unstable shoulder around 10,240.**

An earlier draft of this document reported a clean 10,240-pass / 12,288-fail
bracket. That was wrong — not the failing side, which has held over four runs,
but the passing side: 10,240 later failed four times running. The controls rule
out the obvious explanations. `gem.xex` and `gem.sym` were mtime-identical across
every run; the `.atr` is md5-identical before and after a boot, so the emulator
is not writing to it; the image is rebuilt from identical inputs each time.

The instability tracks **machine load** — the early passes were on a quiet
machine, the later failures at load average 28-32 with three emulator instances
live. That fits a timing-sensitive CIO status rather than a size check, with the
byte count only setting how many reads are gambled on: 8,192 is four reads and
never loses, 12,288 is six and never wins, 10,240 is five and lands on edge.

It is **not** a bank crossing: on a successful 10,240-byte read the blob starts at
`$0443A4`, with 48,220 bytes left in bank `$04`. And it is not a ragged final
slice — 10,240 and 12,288 are both exact multiples of the 2,048-byte slice.

gem4xe's own programs are 4,367 and 2,762 bytes, so nothing in that tree reaches
the shoulder; its shipping desktop is 34,652 and reads fine off a CF card under
SpartaDOS X.

That last fact is the one a size story could never accommodate, and the load
correlation explains it: **a CF volume does not go through SIO and a `.atr`
floppy does.** SIO is timing-sensitive and an emulator under load renders it
differently, so the leading hypothesis is a transient SIO-level error surfacing
as a CIO status outside the three the read loop accepts.

Which makes the defect a design one rather than an arithmetic one.
`far_read_file` treats any such status as fatal: it sets `total = 0`, discards
every byte already read, and returns 0 — which `sh_init` reports as
`APP_E_FILE`, indistinguishable from the file not existing. A transient glitch
99 reads into a 202 KB file therefore presents as "missing file". A loader that
retried the failed read would be correct under every story considered so far.

Note also that `sh_lastrc`, the obvious probe, is blind to this: `sh_init`'s
`APP_E_FILE` never reaches `sh_ldapp`'s `sh_lastrc = st`. The measured values are
`sh_runs=0, sh_lastrc=0, sh_lastret=0` — "nothing has happened yet". The reading
that matters is `sh_desk_blob`.

RetroWP's staging is ruled out: `DESKTOP.G4A` extracted back out of the `.atr`
with gem4xe's own `tools/atr.py` is byte-identical to the built file, 202,327
bytes both ways.

### For the record, RetroWP is also much larger than anything gem4xe has run

| | blob | far | banks | far bank fixups |
|---|---|---|---|---|
| RETROWP.G4A | 198,231 | 168,007 | 4 | 7,441 (fw=3) |
| m16_desk.g4a | 4,367 | 1,519 | 1 | 52 |
| m29_big.g4a | 2,762 | 1,030 | 2 | 60 |

45x anything gem4xe has loaded as an application. A 16-bit count anywhere in that
path would show at this size and at no other — the same shape as the bug
`app.c:192` already records, where `memcpy_far`'s `size_t` truncated a 115 KB
image silently.

### The next step

The read is gem4xe's to fix and its author has the bracket and a repro that needs
no RetroWP at all.

**Tiering is still RetroWP's job, on its own merits.** gem4xe has no 198 KB
ceiling by design, so this image is not too big because the loader says so — it
is too big because a first Atari 8-bit build carrying RTF, WordStar,
WordPerfect, MS Word, spell, grammar, help *and* offload is indefensible on its
face, and prime directive 5 says features must tier. That work should wait until
the read is fixed, though: until then a size decision cannot be told apart from a
bug. The code is already tree-shaken, so trimming means dropping features, not
adding flags.

### Two things fixed along the way, both real

`do_edit_paste`'s 4 KB stack buffer is now `static` — unremarkable on a 68000,
larger than the entire near pool on a 65816, where the stack is in bank `$00` by
hardware and cannot be moved out.

`GEM4XE_STACK` is 4096. That figure was measured while the "it runs but does not
draw" reading was still current, so it does not yet mean what it appeared to; it
is kept because it is generous and costs nothing against a 14 KB pool. It will
need re-measuring once the program actually runs.

### Checked and clean

Calypsi 5.18 has a codegen bug (gem4xe `tools/ccbug` B15, upstream #88) where
`P->a = P->b OP e` — same near pointer both sides, one binary operator — compiles
as `P->a OP= e`, silently, at every optimisation level. RetroWP has no instance of
that shape in the engine, `platform/common`, the ST shell or the retroplat
backend.

## Building it needs gem4xe's kit, which is a build product

`GEM4XE_SDK` defaults to `~/dev/gem4xe/build/gem4xe-sdk`. That is not a
checked-in directory — `make sdk` in the gem4xe tree creates it and `rm -rf
build` there removes it again, so it can simply be absent. `make gem4xe` checks
for it first and says so; without that check the first failure is `gem.h: file
not found` from a backend source, which points at the wrong tree entirely.

For a build that does not depend on another tree's live build directory, unpack
`gem4xe-sdk.tar.gz` somewhere stable and point `GEM4XE_SDK` at that.

### gem4xe 0.2 needs a rebuild, and a fresh kit first

gem4xe 0.2 moved the call gates and the `.g4a` format number (now 3 or 4). It refuses a
format 1 or 2 image with a "built for an older gem4xe" alert, and an older gem4xe refuses
a new one. **Every `RETROWP.G4A` built before 0.2 is refused**, the measured baseline and the
committed image included.

No source or Makefile change is needed. RetroWP issues no `COP` of its own: its only
assembly is the kit's `gemabi.s` and `crt_gemapp.s`. And gem4xe's `tools/mksdk.py` still
ships all seven files `make gem4xe` links from, at the same paths (`include/gem.h`,
`lib/gemlib.c`, `lib/clib.c`, `lib/gemabi.s`, `lib/crt_gemapp.s`, `lib/gemapp.scm`,
`tools/mkg4a.py`).

But the kit is a build product, and `make gem4xe` uses whatever is unpacked in
`GEM4XE_SDK`. **Regenerate it first** (`make build/gem4xe-sdk.tar.gz` in the gem4xe tree).
Otherwise the build succeeds against the old `gemabi.s` (`cop #0x73` / `#0xc8` / `#0x01`)
and produces an image 0.2 will refuse, with nothing at build time to say so.

On 2026-09-15 at 10:41 the unpacked kit in `~/dev/gem4xe/build/gem4xe-sdk` was
regenerated from 0.2. I checked it: `gemabi.s` is `cop #0x56` / `#0x41` / `#0x44`, and
`mkg4a.py` stamps format 3/4. So a clean `make gem4xe` now builds a 0.2 image. The kit
regenerated at 12:12 (gem4xe `63e121e`) also carries the guard: `mkg4a.py` refuses to pack
a program whose `vdi_call`/`aes_call`/`dos_call` aren't 0.2's gates, and says to rebuild
its objects. So the silent failure above is now an error on the host.

### Rebuilt for 0.2 (2026-09-16)

`make gem4xe` on Calypsi 5.18, against the kit unpacked at 21:03 on 2026-09-15:

    RETROWP.G4A   202,424 bytes   format 4   near 11,520   far 168,077   4 banks

It packs through gem4xe's new gate check, so its calls really are 0.2's. Booted against
gem4xe's 00:13 GEM.COM on a bare SpartaDOS 3.2 floppy: read 202,424 bytes, `APP_OK`,
near region at `$4C00`, `wp_menu_tree_init` ran, and the menu bar and document window
drew — this time **with the title**.

One caveat on that build: the unpacked kit was already behind its own sources. Four
gem4xe commits since `63e121e` touch them, including `dfdef44`, which adds the scrap
manager. When the kit is regenerated, RetroWP should be rebuilt against it, and
retroplat's `clipboard_gem4xe.c` can then stop working around the absence of
`scrp_read`/`scrp_write`.

### Rebuilt for the `4994e46` kit (2026-09-16)

    RETROWP.G4A   203,352 bytes   format 4   near 11,520   far 168,751   4 banks

Four changes, all of them the kit catching up with what this port had to work around:

- `timer_gem4xe.c` replaces `timer_atari.c` on this target (below).
- `clib_gem4xe.c` is gone; `memcmp` and `memchr` come from the kit's own `clib.c`.
  Checked in the link map rather than assumed: both resolve to `build/gem4xe/clib.o`,
  not to `clib-lc-ld.a`, which is the whole point of them.
- `gemcompat.h`'s `wind_create_grect`/`wind_open_grect` macros are gone; `gem.h`
  declares the functions.
- `clipboard_gem4xe.c` uses the real scrap manager, so RetroWP's clipboard is now the
  machine's rather than its own private file. Two details it has to get right: the path
  buffer crossing into the AES must be `__near` (a path is longer than the far-string
  bounce, and `gem.h` says so where it declares the calls), and the fallback directory
  is `Dgetdrv`'s rather than a literal `C:\` — gem4xe maps a GEM drive letter onto a
  SpartaDOS device by position (`src/sys/dos.c`), so `C:` is `D3:`, which a one-floppy
  machine has not got.

The gate's disk went from 2,560 sectors to 4,096 in the same pass. GEM.COM and
RETROWP.G4A together no longer fit 320 KB — 23 sectors free, 28 more wanted — and a
gate that fails with "disk full" while both sides are healthy is measuring the disk.

#### `make gem4xe-check` fails on this kit, and not because of this rebuild

Against gem4xe `4994e46`, RetroWP reads, loads, draws its menu bar, opens its window
**with the title** — and then returns from its event loop. `app_near` is `$0000` and
`sh_runs` is 1, which is `app_free` having wound the pool back; `sh_lastrc` is `APP_OK`
and `sh_lastret` is 0, so nothing failed and nothing trapped. It starts up correctly and
then leaves, cleanly.

Bisected before blaming it on the rebuild, because two things changed at once — this
port's four changes and gem4xe's kit:

| kit | `gem.xex` | image | result |
|---|---|---|---|
| 0.2 | `30048b2a` | 202,424 | in its event loop (`sh_runs` 0), PASS |
| `0f3c984` | `a469e696` | 202,424 | in its event loop, PASS |
| `4994e46` | `cfb888b8` | 202,424 | returns (`sh_runs` 1) |
| `4994e46` | `cfb888b8` | 203,352 | returns (`sh_runs` 1) |

The 202,424 image is byte-identical in the first three rows — the file that passed on
the older kits is the file that returns on the newest. So the variable is the kit, and
`0f3c984` is clean, which puts it on `4994e46`. Each run booted the `gem.sym` belonging
to the `gem.xex` under it: symbols from one build read against another answer garbage,
which would have looked exactly like this.

Reported upstream with the bisect. Nothing to do here until it comes back.

The general lesson is the one this port keeps relearning: when the thing under test and
the thing it runs on both move, a failure names neither. Keep a verified snapshot of
every kit (`build/gem4xe-baseline/kit-*/`, each with its `MD5SUMS`), because the control
run needs the old one, and gem4xe's `build/` will not have it.

### No C library heap

gem4xe `fe75914` makes `malloc` and `free` fail at link time, with an undefined
`gem4xe_has_no_heap__use_GEMDOS_Malloc`, because an application's heap was always zero bytes
and every call used to return NULL. RetroWP is unaffected. The engine allocates only through
`mem_alloc`, `mem_gem4xe.c` builds its arenas on GEMDOS `Malloc`, and the committed image's
link map references no `malloc`, `free`, `calloc` or `realloc`.

### The timer: a link refusal, then a real clock, and a bug of our own

For a few kit revisions `clock()` was a deliberate link-time refusal
(`gem4xe_has_no_clock__use_evnt_timer_or_vex_timv`), on the grounds that the library's
`clock.o` is Apache-2.0 object code that would link quietly into a GPLv2 binary and then
answer a number with no meaning, because nothing on the machine drives the library's tick.

Since gem4xe `4994e46` it is real: `gemlib.c` implements it over `Tgettimeofday`
(GEMDOS `0x155`, MiNT's call, served from the ~4 kHz POKEY timer the pointer sampler
already runs), in `CLOCKS_PER_SEC` units. So `timer_atari.c` links again. Two things
about it differ from the ST and should not be inherited by assumption:

- It counts from the program's **first call to `clock()`**, not from power-on. mintlib's
  is `_hz_200` since the machine came up. Only deltas are taken here, so it does not
  matter — but a caller that expects a large value at startup would be wrong.
- `CLOCKS_PER_SEC` is **1000**, not mintlib's 200. Verified by compiling a probe under
  this target's own flags rather than by reading `calypsi/config.h`, which `time.h` does
  not include; `clock_t` is `int64_t`. There is no risk of the kit and the caller
  disagreeing about the rate, because this build compiles the kit's `gemlib.c` and
  `clib.c` itself, from the same headers.

That second point exposes a defect in **our** line, not gem4xe's:

    return (u32)(((u32)c * 1000UL) / (u32)CLOCKS_PER_SEC);

At 1000 the ratio is the identity, so the multiply buys nothing — and costs ten bits of
range. `(u32)c * 1000` overflows at `c = 4,294,967`, so **71.58 minutes** into a session
`plat_ticks_ms` stops being monotonic and begins answering garbage. The truncation to
`u32` is not the problem: at this rate `c` is milliseconds and fits for 49 days. The same
line is in the ST and DOS backends, where a slower `CLOCKS_PER_SEC` merely moves the
cliff out — about six hours at mintlib's 200 Hz. The fix is what the kit's own `clock()`
does for the same reason: divide first, or split whole seconds from the remainder.

`plat_sleep_ms` busy-spins, which was correct on the ST and is not here: a spin that
never calls the AES starves the desk accessories and the AES's own timer events.
`evnt_timer(lo, hi)` is the same span and yields.

Both were latent — nothing in the gem4xe build calls the timer seam, since
`transport_atari.c` is excluded — and both are fixed in `timer_gem4xe.c`, behind the
`ATARI_GEM4XE_ONLY` filter so that a gem4xe fix changes no ST behaviour. The overflow
is fixed in `timer_atari.c` and `timer_dos.c` too, where it was the same line.

The linker discards both functions today, nothing having called them, so the link does
not prove they resolve. `nm` on `timer_gem4xe.o` does: it defines `plat_ticks_ms` and
`plat_sleep_ms` and leaves `clock` and `evnt_timer` undefined, and both of those are
`T` in the kit's own `gemlib.o`, which is already on the link line.

### Toolchain

`CALYPSI65816` defaults to `~/dev/toolchains/calypsi-65816`, which is a symlink
the machine's owner repoints. It moved from 5.18 to 5.18.1 during this work.

The hazard there is not the version but the **stale objects**: existing `.o`
files are up to date by every timestamp `make` can see, so a plain build after a
toolchain change links output from two compilers and says nothing. Whenever the
symlink moves, delete `build/gem4xe` rather than trusting mtimes. The measurements
in this document are unaffected either way — the probes are truncated files that
exercise `far_read_file` and stop at the header check, so RetroWP's own code is
never executed in any of them.

## Licence

An application links no part of gem4xe, but it **does** compile in the kit
(`gemlib.c`, `gemabi.s`, `crt_gemapp.s`, `clib.c`), and the kit is GPLv2.
`RETROWP.G4A` is therefore a GPLv2 binary, while the engine and retroplat sources
it is built from remain under their own licences.

`memcmp` and `memchr` are written in `clib_gem4xe.c` rather than taken from
`clib-lc-ld.a` for the same reason the kit writes its own `memcpy`: Calypsi's come
from Apache NuttX, and Apache 2.0 cannot be linked into a GPLv2 binary. Without
them the linker satisfies both from the library quietly — quietly being the
problem.

The kit writes both itself now, for that same reason, so `clib_gem4xe.c` is gone: its
copies would be duplicate symbols, and the licence argument that put them there is
satisfied by gem4xe's own implementations, which are GPLv2 like the rest of the kit.
Verified in the link map rather than assumed — `memcmp`, `memchr`, `strcpy` and
`strcat` all resolve to `build/gem4xe/clib.o`. What still comes from `clib-lc-ld.a` is
the compiler's own support code (`pseudoRegisters.o`, `integer.o`, `memory.o`,
`vswitch*.o`, `controlFlow.o`, `spill.o`, `initialize.o`), which is what the kit's own
`clib.c` comment says to expect and is not the C library.

RetroWP has no top-level `LICENSE` file. It needs one, and which it should be is
not a decision this document can make.
