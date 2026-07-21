# Mac Classic Platform Backend (M12)

`platform/mac` is RetroWP's third real `platform.h` backend, targeting
classic Mac OS (68k Toolbox + QuickDraw, System 7.5.3 minimum per the
fixture's own install). This document covers the toolchain, the
BasiliskII test-fixture recipe, and the honest gotchas hit standing both
up -- written incrementally per phase, starting here with Phase 0
(toolchain + CI-fixture bring-up). **Phase 0 is closed**: a reliable,
hands-free, non-flaky PASS/FAIL signal from a headless BasiliskII boot
is proven across both ROM images, and `host/tools/ci_mac.sh` is a real,
passing CI entrypoint.

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
  project's own plain Makefile (the Atari/Amiga pattern), Mac CI fixtures
  (`host/tools/mac/`) and eventually `platform/mac/` itself each get one
  small, self-contained `CMakeLists.txt` calling Retro68's own
  `add_application()` against their toolchain file -- a deliberate,
  documented, Mac-only exception to this project's
  plain-Makefile-everywhere convention, because Mac resource-fork
  packaging is real, non-optional complexity Retro68 already solved
  correctly.
  ```
  cmake -S host/tools/mac -B build/mac -DCMAKE_TOOLCHAIN_FILE=~/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake -G "Unix Makefiles"
  cmake --build build/mac
  ```
- `add_application(target source.c [CONSOLE])`: omitting `CONSOLE`
  produces a plain GUI-less app with no auto stdio window; passing
  `CONSOLE` links Retro68's console library (auto stdio window, but see
  the "unimplemented trap" gotcha below). **`APPL` is not a valid
  keyword** -- passing it as a third argument makes CMake treat it as a
  source filename and fail with "Cannot find source file: .../APPL".
  Just omit the keyword entirely for a non-console app.
- **Retro68 CONSOLE app "unimplemented trap"**: a CONSOLE-type build,
  auto-launched via Startup Items, hit a real classic Mac "bomb" dialog
  -- "Sorry, a system error occurred. 'RetroWP CI' unimplemented trap" --
  likely the Console library's auto-window-init path being incompatible
  with a Startup-Items launch context (vs. interactive double-click
  launch, which is CONSOLE's normal use case). Worked around by building
  plain (non-CONSOLE) apps throughout. Non-issue for the real backend
  either way -- Phase 1's actual app will use direct Toolbox calls
  (matching `Samples/WDEF`), not the console stdio library.
- **Per-topic headers are thin wrappers around one master header.**
  `Processes.h`, `AppleEvents.h`, etc. are each just a two-line
  `#pragma once` + `#include "Multiverse.h"` -- at a glance this looks
  like "Process Manager isn't implemented here," but the real
  declarations (including `LaunchApplication`, trap `0xA9F2`, with a
  `#pragma parameter` register mapping) live inside the ~19,000-line
  `Multiverse.h` all per-topic headers funnel into. Confirmed by
  grepping the master header directly rather than trusting a topic
  header's apparent emptiness.

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

- `~/opt/mac/fixture/boot_template.img` -- a bootable HFS volume,
  prepared **once** (see "The launcher stub" below) and never mutated
  in place afterward. Every CI run works on its own fresh
  `cp` of this template into a scratch directory, exactly like every
  other backend's fixture. It is derived from the user's own personal,
  already-existing Mac setup (`~/Apple/Mac/basilisk-40.img`, itself
  built previously from real System 7.5.3 install media) -- no fresh OS
  install happened this session.
- `~/opt/mac/fixture/rom/{PERFORMA.ROM,maciici.rom}` -- two real ROM
  dumps, read-only. `PERFORMA.ROM` (Gestalt model 14, 68040-class,
  `modelid 14`/`cpu 4`) and `maciici.rom` (Mac IIci, 68030-class,
  `modelid 5`/`cpu 3`) become the CI matrix's two images -- honestly
  narrower than Atari's 6-image or Amiga's 3-image matrices, since only
  two real ROM dumps exist locally.
- `~/opt/mac/fixture/shared/` -- a plain host directory mounted via
  BasiliskII's `--extfs`, appearing in the guest as a `Unix` volume.
  This is where the app actually under test lives (`CI_TARGET.APPL`,
  copied in fresh by `host/tools/mac_ci_lib.sh` before every boot) and
  where its PASS/FAIL log (`mac_smoke.out`) is read back from -- a plain
  host file copy, never HFS surgery, per run.
- A prefs file, written fresh before every boot (never relying on
  `~/.basilisk_ii_prefs`) -- **passing `--config` pointed at a
  nonexistent file silently falls back to hardcoded defaults and
  ignores every other CLI flag**, a real gotcha found early. See
  `host/tools/mac_ci_lib.sh` for the exact content generated per run.

### Headless driving technique (Xvfb + xdotool + ImageMagick)

Same overall shape as `ci_atari.sh`/`ci_amiga.sh`'s own Hatari/amiberry
harnesses, with Mac-specific gotchas:

- Boot: `Xvfb :N -screen 0 1024x768x24`, then
  `env HOME=$SCRATCH DISPLAY=:N SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy BasiliskII --config $SCRATCH/prefs`.
- Screenshot: `import -window root -display :N file.png` (ImageMagick).
  **`xwd` is not installed** on this machine -- don't assume it.
- **The fixture is always booted from a freshly copied, never-cleanly-
  shut-down image**, so classic Mac OS's "This computer may not have
  been shut down properly" dialog appears on every single boot. It
  explicitly instructs "Press the Return key on the keyboard to
  continue" -- `xdotool key Return` is the reliable dismissal.
  Coordinate-based clicking was tried first and is markedly less
  reliable here: a blind `xdotool click 1` at the (stale) last-known
  cursor position missed the dialog's actual OK button entirely and
  stalled the whole boot indefinitely in one early test -- when a
  dialog tells you which key to press, prefer the key.
- `Cmd+W` maps to `Alt+W` in this BasiliskII build (useful if any
  interactive Finder-window automation is ever needed again, though the
  current CI flow needs none -- see below).
- **State does not persist across separate tool-call invocations**, even
  under `setsid`/`nohup`/`disown` -- the emulator appears to stall (guest
  clock stops advancing, `xdotool` input has no effect) once the
  originating shell invocation ends. All automation must happen within
  one continuous script run, launch to final check/kill.
- **Xvfb display-number collisions are a real, occasionally-hit failure
  mode**, same risk `atari_ci_lib.sh`'s own comments already document:
  a stale lock file from an earlier, already-dead Xvfb process can fool
  the "is it up yet" readiness poll, and a *live* stray Xvfb from
  unrelated concurrent work can collide outright ("Cannot connect to X
  server" from BasiliskII). Hit once during this milestone's own
  testing, caused by an unusually large number of stray Xvfb processes
  left running from extensive manual debugging earlier in the same
  session -- not a defect specific to the Mac scripts, the identical
  display-number formula is shared with `atari_ci_lib.sh`/
  `amiga_ci_lib.sh` and carries the same accepted, low-probability risk
  there too.

### A note on the user's personal disk image

One early test pointed `--config` at a nonexistent path (see the gotcha
above), which may have caused BasiliskII to fall back to the user's real
`~/.basilisk_ii_prefs` -- which points at their personal
`~/Apple/Mac/basilisk-40.img`. That file's mtime was later found changed
during this session. This was disclosed to the user directly; they
confirmed it was not a concern. Every test since has operated exclusively
on a disposable copy, never on the original again.

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
from-scratch tool that injects something directly into
`System Folder:Startup Items` via targeted HFS surgery, so it
auto-launches at boot with zero interaction. Finder double-click
automation proved unreliable after extensive iteration (hash-based "did
it open" detection had real false-positive/false-negative modes, and
multi-level Finder navigation compounded the unreliability). Option 3
was chosen -- but what gets injected, and how often, changed shape once
Phase 0 was actually working end to end (see below).

## A from-scratch HFS injector

`host/tools/mac_hfs/` is a purpose-built (not general-purpose) HFS
(Mac OS Standard) reader/writer, built to insert a new file record under
`System Folder:Startup Items` via a correct bottom-up B+tree
cascading-split insertion into the volume's Catalog B-tree, plus Volume
Bitmap allocation for the file's data/resource forks. Reference: *Inside
Macintosh: Files*; every byte offset below was empirically verified
against the real fixture, not assumed from the spec alone. It is used
exactly **once** per fixture template (see "The launcher stub" below),
not per CI run.

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
created; depth increased 4 -> 5. Verified self-consistent by an
independent read-back parse (leaf record count correctly incrementing,
the new file correctly present under the right parent with the right
fileID and type) and reproduced identically across every fresh
`boot_template.img` rebuild since.

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

**Confirmed via a real screenshot**: Mac OS auto-launched an injected app
from `Startup Items` with zero interactive clicking, and the app's code
genuinely executed -- reaching a real Toolbox-level "unimplemented trap"
bomb dialog on an early CONSOLE-library test (see the toolchain gotcha
above), not a hang or corruption. This is the hardest part of Phase 0's
risk, and it is solved.

## The launcher stub: LaunchApplication across volumes

The first working end-to-end pipeline injected the *whole app under
test* into Startup Items directly, meaning every CI run re-ran the full
cascading B-tree insertion (with different resource-fork sizes and
allocation-block extents each time the app changed). It worked, but that
per-run structural surgery is real, avoidable risk -- exactly the
instability concern that prompted a simpler design: inject something
**once**, and have every subsequent run touch only plain files on the
ExtFS share.

The first attempt at "something" was a Finder alias -- a real alias file
(`smoketest.APPL alias`, type `adrp`, a 517-byte `alis` resource
resolving to `Unix:smoketest.APPL`) already existed in `shared/` from
earlier interactive Finder-automation attempts. Injecting *that* file's
bytes into Startup Items seemed like a shortcut around hand-encoding the
Alias Manager's binary record format. It didn't work: booting produced
"The document 'RetroWP CI Launcher' could not be opened, because the
application program that created it could not be found" -- Finder
didn't recognize the file as a real alias at all. The `adrp` type (real
Finder aliases normally keep the *original* file's type/creator, with
only the `kIsAlias` Finder-info bit set) strongly suggests this
particular alias was left in a broken, incompletely-created state by
whichever earlier automation attempt produced it -- a genuinely useful
negative result, found by testing rather than assuming the leftover file
was valid.

The design that actually works: `host/tools/mac/launcher.c`, a tiny,
**permanent** Toolbox app that calls `FSMakeFSSpec` + `LaunchApplication`
to hand off execution to `Unix:CI_TARGET.APPL` -- a real Process Manager
launch across volumes, not an alias at all. This is injected into
Startup Items exactly once (producing `boot_template.img`); it never
needs to change again unless the launch logic itself does. Every CI run
after that is just plain host `cp`: `host/tools/mac_ci_lib.sh` copies the
freshly built app onto `shared/` as `CI_TARGET.APPL` (three plain files:
data fork, `.rsrc/CI_TARGET.APPL`, `.finf/CI_TARGET.APPL` -- Retro68's
own CMake build already emits exactly this BasiliskII-compatible sidecar
layout, no repackaging needed), boots the pre-baked template fresh, and
reads back `mac_smoke.out`.

This also happened to resolve what looked like a separate, deeper bug:
earlier testing (with the whole-app-in-Startup-Items design) saw `FSWrite`/
`fopen` content intermittently fail to persist -- files created but
0 bytes, one ambiguous single "PASS" that didn't reproduce, and a
confusing run where both a pass-path and fail-path marker file appeared
at once. None of that has recurred even once since switching to this
design (multiple clean reruns across both ROM images, plus every run of
the committed `ci_mac.sh`). The leading explanation: that earlier
flakiness was never a real ExtFS/Toolbox content-persistence bug at all
-- it was a symptom of the unreliable *Finder double-click automation*
those tests still depended on to launch the app (misclicks landing on
the wrong on-screen target, or occasionally double-invoking the app),
which this design eliminates completely. The only interactive step left
in the entire boot sequence is one `xdotool key Return` for the
"not shut down properly" dialog -- there is no more Finder-level clicking
anywhere in the loop.

## CI infrastructure

- `host/tools/mac/launcher.c` -- the permanent Startup Items stub.
- `host/tools/mac/smoke.c` -- the Phase 0 smoke test itself: proves the
  pipeline (Retro68 -> BasiliskII boot -> Startup Items ->
  `LaunchApplication` hand-off -> ExtFS write), not yet any real
  `platform.h` seam (there is no `platform/mac/` yet). Phases 1-6 grow
  this the same way `atari_smoke.c`/`amiga_smoke.c` grew: one line per
  real `wp_mac_*_selfcheck()`.
- `host/tools/mac/CMakeLists.txt` -- builds both via Retro68's
  `add_application()`.
- `host/tools/mac_ci_lib.sh` -- `run_mac_smoke_once(rom, offset, modelid,
  cpu)`, mirroring `amiga_ci_lib.sh`'s shape: fresh `boot_template.img`
  copy, fresh `CI_TARGET.APPL` copy onto `shared/`, boot, dismiss the
  shutdown-check dialog, poll for `mac_smoke.out` (or either launcher
  failure marker), clean up every file it touched on `shared/` (including
  the `.finf/` sidecars ExtFS auto-creates for plain files, or they'd
  accumulate across runs).
- `host/tools/ci_mac.sh` -- single-ROM (`PERFORMA.ROM`) smoke gate,
  mirroring `ci_atari.sh`/`ci_amiga.sh`'s role. Builds via CMake directly
  (no `mac-engine-check`/`mac-smoke` Makefile target yet -- that's
  Phase 1's, once `platform/mac/` exists to check). **Verified passing
  repeatedly**, including one real transient failure (an Xvfb display
  collision, see above) that a clean rerun resolved -- consistent with
  the same accepted flakiness class the other two backends' matrices
  already carry, not a new defect.
- `ci_mac_matrix.sh` (both ROMs, looped) is deliberately not added yet --
  matching Phase 6's own scope in `docs/milestones.md`, the same phase
  `ci_amiga_matrix.sh` arrived in for Amiga. Both ROMs were verified
  directly during Phase 0 closeout instead (three independent clean
  boots: two against `PERFORMA.ROM`, one against `maciici.rom`).

## Status

**Proven**: the Retro68 + BasiliskII + headless-Xvfb pipeline builds and
runs real m68k classic Mac code, no Docker/podman required; a
from-scratch HFS catalog B-tree writer can correctly and safely inject a
file into `Startup Items` (three real bugs found and fixed along the
way, one of which was a 100%-reproducible boot crash); classic Mac
`Startup Items` plus a tiny permanent `LaunchApplication`-based stub
gives fully hands-free, non-flaky CI automation with **zero** Finder
interaction and **zero** per-run disk-image surgery; `host/tools/ci_mac.sh`
passes repeatedly against both ROM images.

**Next**: Phase 1 -- the real `platform/mac/` Toolbox shell (event loop,
menus/dialogs), which is what finally earns the top-level Makefile's own
`mac-engine-check`/`mac-app` targets and lets `mac_smoke.c` grow real
`wp_mac_*_selfcheck()` coverage instead of the placeholder pipeline probe
it is today.
