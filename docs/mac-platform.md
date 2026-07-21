# Mac Classic Platform Backend (M12)

`platform/mac` is RetroWP's third real `platform.h` backend, targeting
classic Mac OS (68k Toolbox + QuickDraw, System 7.5.3 minimum per the
fixture's own install). This document covers the toolchain, the
BasiliskII test-fixture recipe, and the honest gotchas hit standing both
up -- written incrementally per phase, starting here with Phase 0
(toolchain + CI-fixture bring-up). **Phase 0 is not yet closed**: the
hardest architectural risk (a reliable, hands-free pass/fail signal from
a headless BasiliskII boot) is solved, but one real, unresolved bug
remains in the last step of that chain -- see "Open issue" below.

## Toolchain

- Compiler: [Retro68](https://github.com/autc04/Retro68), the actively
  maintained cross-compiler for classic Mac OS (68k Toolbox, PowerPC
  Classic, Carbon). No prebuilt Linux binaries exist for either tagged
  release -- only a Docker image (unusable here, no `docker`/`podman`
  installed) or building from source. Built `m68k`-only
  (`build-toolchain.bash --no-ppc --no-carbon --ninja`, ~15 minutes) --
  this project has zero use for PowerPC/Carbon, narrowing the build
  materially versus Retro68's own full 3-target CI matrix. Produces
  `m68k-apple-macos-gcc`/`-g++` plus Retro68's own reimplemented `Rez`
  resource compiler, all plain ELF binaries once built.
- Installed at `~/opt/Retro68-build/toolchain/` (~1.8GB). Retro68 source
  at `~/opt/Retro68/` (`git clone --recursive`).
- **Build system is CMake-only.** The real resource-packaging step (the
  exact `Rez` invocation producing `.bin`/`.APPL`/`.dsk`/`.ad` outputs
  together) lives only inside Retro68's own `cmake/add_application.cmake`
  macro, not documented as a standalone command line anywhere. Rather
  than reverse-engineer and hand-maintain that invocation in this
  project's own plain Makefile (the Atari/Amiga pattern), `platform/mac/`
  will get one small, self-contained `CMakeLists.txt` calling Retro68's
  own `add_application()` against their toolchain file -- a deliberate,
  documented, one-target-only exception to this project's
  plain-Makefile-everywhere convention, because Mac resource-fork
  packaging is real, non-optional complexity Retro68 already solved
  correctly.
  ```
  cmake .. -DCMAKE_TOOLCHAIN_FILE=~/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -G "Unix Makefiles"
  make
  ```
- `add_application(target source.c [CONSOLE])`: omitting `CONSOLE`
  produces a plain GUI-less app with no auto stdio window; passing
  `CONSOLE` links Retro68's console library (auto stdio window, but see
  the "unimplemented trap" gotcha below). **`APPL` is not a valid
  keyword** -- passing it as a third argument makes CMake treat it as a
  source filename and fail with "Cannot find source file: .../APPL".
  Just omit the keyword entirely for a non-console app.
- **Retro68 CONSOLE app "unimplemented trap"**: a CONSOLE-type build,
  auto-launched via Startup Items (see below), hit a real classic Mac
  "bomb" dialog -- "Sorry, a system error occurred. 'RetroWP CI'
  unimplemented trap" -- likely the Console library's auto-window-init
  path being incompatible with a Startup-Items launch context (vs.
  interactive double-click launch, which is CONSOLE's normal use case).
  Worked around by building a plain (non-CONSOLE) app using either raw
  Toolbox calls or plain stdio without the console window. This is a
  non-issue for the real backend either way -- Phase 1's actual app will
  use direct Toolbox calls (matching `Samples/WDEF`), not the console
  stdio library.

## BasiliskII test fixture

Emulator: the user's own from-source build at
`~/macemu-mon/BasiliskII/src/Unix/BasiliskII` (upstream `cebix/macemu`,
debug build) -- used throughout rather than the Arch `basiliskii-git`
package, matching the user's own known-working incantation
(`SDL_VIDEODRIVER=x11 padsp ...`; `padsp` becomes unnecessary for
headless CI once `--nosound true` drops the OSS dependency entirely, the
same simplification `ci_amiga_matrix.sh` already makes via
`SDL_AUDIODRIVER=dummy`).

Fixture layout (outside the repo, like the toolchain and every other
backend's emulator fixture):

- `~/opt/mac/fixture/boot.img` -- a disposable, project-owned HFS boot
  volume, always freshly re-copied before each test run
  (`cp ~/Apple/Mac/basilisk-40.img ~/opt/mac/fixture/boot.img`) rather
  than mutated in place. It is derived from the user's own personal,
  already-existing Mac setup (itself built previously from real System
  7.5.3 install media) -- no fresh OS install happened this session; the
  disposable copy is what every test actually touches.
- `~/opt/mac/fixture/rom/{PERFORMA.ROM,maciici.rom}` -- two real ROM
  dumps, read-only. `PERFORMA.ROM` (Gestalt model 14, 68040-class,
  boots System 7.5.3) and `maciici.rom` (Mac IIci, 68030-class, older/
  simpler machine) become the CI matrix's two images -- honestly
  narrower than Atari's 6-image or Amiga's 3-image matrices, since only
  two real ROM dumps exist locally.
- `~/opt/mac/fixture/shared/` -- a plain host directory mounted via
  BasiliskII's `--extfs`, appearing in the guest as a `Unix` volume.
  This is the CI signal channel (see below) and is **not** touched by
  the HFS injector -- the injector only ever writes into `boot.img`, and
  only for the one-time task of placing the launchable app into
  `System Folder:Startup Items`.
- A prefs file, written fresh before every boot (never relying on
  `~/.basilisk_ii_prefs`) -- **passing `--config` pointed at a
  nonexistent file silently falls back to hardcoded defaults and
  ignores every other CLI flag**, a real gotcha found early. Minimal
  working content:
  ```
  disk ~/opt/mac/fixture/boot.img
  extfs ~/opt/mac/fixture/shared
  screen win/512/384
  rom ~/opt/mac/fixture/rom/PERFORMA.ROM
  bootdrive 0
  bootdriver 0
  ramsize 33554432
  modelid 14
  cpu 4
  fpu true
  nocdrom true
  nosound true
  nogui true
  jit false
  keyboardtype 5
  idlewait true
  ```

### Headless driving technique (Xvfb + xdotool + ImageMagick)

Same overall shape as `ci_atari.sh`/`ci_amiga.sh`'s own Hatari/amiberry
harnesses, with Mac-specific gotchas:

- Boot: `Xvfb :N -screen 0 1024x768x24`, then
  `env HOME=$SCRATCH DISPLAY=:N SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy BasiliskII --config $SCRATCH/prefs`.
- Screenshot: `import -window root -display :N file.png` (ImageMagick).
  **`xwd` is not installed** on this machine -- don't assume it.
- Dialog dismiss / window close: poll-until-stable single clicks,
  comparing screenshot MD5 hashes across iterations, breaking when the
  hash stabilizes across 2+ consecutive attempts. `Cmd+W` maps to
  `Alt+W` in this BasiliskII build (reliable for closing Finder windows
  when needed).
- Double-click / open (only needed before the Startup Items approach
  replaced it): a single baseline click, then a burst of ~6-10
  individual clicks at ~200ms intervals reliably triggers double-click
  recognition somewhere in the sequence -- more reliable than precise
  2-click timing, though not fully deterministic.
- **State does not persist across separate tool-call invocations**, even
  under `setsid`/`nohup`/`disown` -- the emulator appears to stall (guest
  clock stops advancing, `xdotool` input has no effect) once the
  originating shell invocation ends. All automation must happen within
  one continuous script run, launch to final check/kill.

### A note on the user's personal disk image

One early test pointed `--config` at a nonexistent path (see the gotcha
above), which may have caused BasiliskII to fall back to the user's real
`~/.basilisk_ii_prefs` -- which points at their personal
`~/Apple/Mac/basilisk-40.img`. That file's mtime was later found changed
during this session. This was disclosed to the user directly; they
confirmed it was not a concern. Every test since has operated exclusively
on the disposable `~/opt/mac/fixture/boot.img` copy, never on the
original again.

## The CI-signaling problem

Neither Retro68 nor BasiliskII has a Native-Features-style guest-to-host
exit/stderr trap (the mechanism `ci_atari.sh` uses via Hatari's
`NF_STDERR`/`NF_EXIT`). This project already solved the identical problem
for Amiga: `amiberry` has no exit-status trap either, and
`amiga_ci_lib.sh`'s answer is a real host-visible shared directory that
the guest writes a sentinel + PASS/FAIL log into.

**That trick does not transfer directly.** Amiga's fixture mounts its
*boot* volume itself as a plain host directory (`uaehf0=dir,...` in
`mounts.uae`) -- Workbench's own `Startup-Sequence` is a host-editable
text file, and the CI hook lives right on the boot volume with zero
extra plumbing. BasiliskII has no equivalent for the boot volume: real
Mac ROM requires booting from a genuine HFS-formatted image, and
`--extfs` only ever mounts a *second*, non-bootable `Unix:` volume from a
host directory. There is no way to get a new file (an app, a script hook)
onto the boot volume itself except by actually writing HFS structures.

Three ways to close this gap were considered: (1) get Finder's own
double-click automation to reliably launch the app every boot, (2) have
the user do one manual, one-time interactive setup step, (3) write a
from-scratch tool that injects the app directly into
`System Folder:Startup Items` via targeted HFS surgery, so it auto-launches
at boot with zero interaction. Finder double-click automation proved
unreliable after extensive iteration (hash-based "did it open" detection
had real false-positive/false-negative modes, and multi-level Finder
navigation compounded the unreliability). The user chose option 3.

## A from-scratch HFS injector

`host/tools/mac_hfs/` is a purpose-built (not general-purpose) HFS
(Mac OS Standard) reader/writer, built to do exactly one thing: insert a
new file record under `System Folder:Startup Items`, via a correct
bottom-up B+tree cascading-split insertion into the volume's Catalog
B-tree, plus Volume Bitmap allocation for the file's data/resource forks.
Reference: *Inside Macintosh: Files*; every byte offset below was
empirically verified against the real fixture, not assumed from the
spec alone.

Key structures (all big-endian, matching this project's own canonical
on-disk byte order elsewhere):

- **Master Directory Block** at absolute byte offset 1024: signature,
  allocation-block size/count/start, next-CNID, free-block count, volume
  name (Pascal string), and -- the one real bug source here -- **two
  separate extent records** at different offsets: the Extents Overflow
  file's own extents, and, 16 bytes further in, the Catalog file's own
  extents. Both files coincidentally had the same size in this fixture,
  which is exactly what let the first bug below go unnoticed until it
  corrupted the wrong blocks.
- **Catalog B-tree**: node 0 is the header node (`BTHdrRec`: depth, root
  node number, leaf record count, first/last leaf node, node size, total/
  free node counts) plus a node-usage bitmap in its third record. Every
  other node is a 512-byte node with a 14-byte descriptor
  (`fLink`/`bLink`/`kind`/`height`/`numRecords`), records packed from
  offset 14 forward, and a trailer table of 2-byte record-start offsets
  counting backward from the end of the node.
- Catalog records: `HFSCatalogFolder` (70 bytes, dirDirID = CNID at
  offset 6), `HFSCatalogFile` (102 bytes, fileID at offset 20, four-byte
  type/creator in the embedded FInfo at offset 4), `HFSCatalogThread`
  (parent ID + name, back-pointer records used for path lookups). Keys
  sort by parent CNID then name.

### The insertion algorithm

`inject_startup_item.py` walks the pristine tree from root to the target
leaf (recording the chosen child index at every level), then works
**bottom-up**: merge the new record into the leaf; if it fits, done. If
not, split the node roughly in half, fix the sibling `fLink`/`bLink`
chain, and propagate a new `(separator_key -> new_node_num)` index record
up to the parent level -- repeating the same fits-or-split test there,
all the way up. If the *root* itself needs to split, a brand-new root
node is allocated and depth increases by one. Every consumed node number
comes from the B-tree's own free-node bitmap (never assumed free).
Header-node bookkeeping (leaf record count, free node count, map bits,
and root/depth if the root split) is updated to match.

**Verified run against the real fixture**: `Startup Items` CNID 265
(under `System Folder` CNID 18); insertion path
`[(123,1), (15,2), (145,1), (108,None)]`; new file CNID 859. Every node
along that path had exactly 2 bytes free, so the insert cascaded all the
way to the root: leaf 108 -> splits, new node 208; index 145 -> splits,
209; index 15 -> splits, 210; old root 123 -> splits, 211; new root 212
created; depth increased 4 -> 5. Resource fork allocated at block 26372
(1205 blocks, for a 1,233,136-byte CONSOLE build) or block 24366 (15
blocks, for a 14,881-byte non-CONSOLE build). Verified self-consistent
by an independent read-back parse (leaf record count correctly
588 -> 589, the new file correctly present under the right parent with
the right fileID and type).

### Three real bugs found and fixed

1. **Wrong MDB offset (silent data corruption).** `_read_mdb()` initially
   read the *Extents Overflow* file's extents instead of the *Catalog*
   file's own extents -- both files were 327,680 bytes, masking the bug
   until the first full run wrote garbage into the wrong allocation
   blocks (0-319, the real Extents Overflow file) while leaving the real
   catalog (blocks 320-639) untouched. Found by cross-checking against
   an independent, already-verified-correct reader. The corrupted test
   copy was discarded before any boot was attempted against it -- no
   boot ever ran against corrupted state. Fixed by adding the missing
   +16 byte offset (past `drXTFlSize`/`drXTExtRec`) before reading the
   catalog's own `ctFlSize`/`ctExtRec`.
2. **Wrong header-field write offsets.** `write_header_node_fields()`
   wrote `leaf_recs`/`first_leaf`/`last_leaf` each 2 bytes too low,
   producing a visibly garbage `leaf_recs` value (38,601,292) after
   injection. Fixed by correcting the offsets; verified by confirming
   the post-injection value exactly matched an independent leaf-node
   scan count.
3. **Missing B-tree node free-space sentinel (100% reproducible boot
   crash).** Real HFS B-tree nodes store `numRecords + 1` offset-table
   entries -- one per record's start, plus a final sentinel marking where
   free space begins. `build_node()` initially wrote only `numRecords`
   entries. This produced a deterministic SIGSEGV during Mac OS boot
   (same faulting instruction every time), root-caused by bisection:
   MDB-only changes booted fine; MDB+Volume-Bitmap changes booted fine;
   catalog-B-tree changes (even with a zero-length file) reproduced the
   crash every time -- isolating the bug to the B-tree write path
   specifically. Fixed by appending the sentinel entry; the `fits()`
   byte-budget check in `inject_startup_item.py` was updated to account
   for it too. After the fix, boot succeeded with zero crashes across
   multiple retests.

**Confirmed via a real screenshot**: Mac OS auto-launched the injected
app from `Startup Items` with zero interactive clicking, and the app's
code genuinely executed -- reaching a real Toolbox-level "unimplemented
trap" bomb dialog (the CONSOLE-library issue above), not a hang or
corruption. This is the hardest part of Phase 0's risk, and it is solved.

## Open issue: ExtFS write persistence

The one piece not yet working: the auto-launched app's file *write*
doesn't reliably show up on the host side. Two implementations were
tried:

- Raw File Manager (`FSOpen`/`Create`/`FSWrite`/`FSClose`), including
  fixing an initial `FSOpen(name, 0, ...)` (mode 0 = `fsCurPerm`, only
  grants write access if the file was already host-writable at open
  time, per BasiliskII's own `extfs.cpp:fs_open()`) to
  `FSOpen(name, 3, ...)` (`fsRdWrPerm`, unconditional). This did not
  reliably fix persistence.
- Plain stdio (`fopen`/`fwrite`/`fflush`/`fclose`), with an else-branch
  writing a `_fail` file for diagnosis if `fopen` itself failed. A clean
  test produced **both** the pass-path and fail-path files simultaneously
  at 0 bytes each -- inconclusive, and not yet explained. (The
  `shared/` directory was not reliably cleared between iterations
  during debugging, so this specific dual-file observation may simply
  reflect stale leftovers from an earlier run rather than one run
  hitting both branches -- that ambiguity itself should be eliminated
  before drawing further conclusions.)

One specific test run did show a correct 4-byte "PASS" file once, but a
careful, deliberately clean rerun (explicit pre-check for no stale file)
produced 0 bytes again -- the single success is most likely stale data
from an earlier lucky run, not evidence the current code path works.

Both raw-Toolbox and stdio paths failing the same way suggests the bug
is not specific to libc's I/O layer. `extfs.cpp`'s `fs_write()`/
`fs_open()`/`fs_create()` were read directly and appear structurally
sound (plain `lseek`+`write`, no obvious stale-cache dependency) --
nothing in that source alone explains the symptom.

Candidate next experiments (not yet tried):
- Clear `shared/` before every run, unconditionally, to remove the
  stale-file ambiguity noted above.
- Replace `for(;;) {}` with a real `ExitToShell()` after the write, in
  case whatever finalizes the host-visible write (rename, size update)
  is deferred until process teardown rather than happening at
  `fclose`/`FSClose` time.
- Write a full allocation-block-sized buffer instead of 4 bytes, in case
  there's a minimum-write-size quirk in the emulated write path.
- Pre-create the target file via the injector itself (already present
  in the catalog with real extents) and have the app open it for
  writing rather than `Create`ing a new file at runtime, to isolate
  whether the create path specifically is where things go wrong.

## Status

**Proven**: the Retro68 + BasiliskII + headless-Xvfb pipeline builds and
runs real m68k classic Mac code, no Docker/podman required; a
from-scratch HFS catalog B-tree writer can correctly and safely inject a
file into `Startup Items` (three real bugs found and fixed along the
way, one of which was a 100%-reproducible boot crash); classic Mac
`Startup Items` genuinely enables hands-free CI automation with zero
Finder interaction, and injected code genuinely executes on boot.

**Open**: reliably getting written file *content* to persist through
`--extfs` back to the host. This is the one remaining piece for Phase
0's original AC (a working, non-flaky PASS/FAIL host-visible signal
across both ROM images). `host/tools/mac_ci_lib.sh`/`ci_mac.sh` are not
yet added -- they'd have nothing reliable to gate on until this is
fixed. `host/tools/mac_hfs/` (the injector) is committed now since it
stands on its own, already verified, and will be needed regardless of
how the write-persistence bug resolves.
