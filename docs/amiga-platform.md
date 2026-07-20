# Amiga Platform Backend (M9)

`platform/amiga` is RetroWP's second real `platform.h` backend and the
first test of the portability thesis M8.5 set up for: can a new GUI
backend land without touching `platform/atari/` or `platform/common/`?
This document covers the toolchain, the amiberry test-fixture recipe, and
the honest gotchas hit standing both up -- written incrementally per phase,
starting here with Phase 0 (toolchain + emulator harness bring-up).

## Toolchain

- Compiler: Bebbo's `m68k-amigaos-gcc` 6.5.0b (github.com/bebbo/amiga-gcc),
  built from source into `~/opt/amiga`. **The GitHub repo has since been
  taken down/moved** -- clone the live mirror instead:
  `https://codeberg.org/bebbo/amiga-gcc`. Per its own `default-repos` file,
  the core components (gcc, binutils, libnix, newlib-cygwin, sfdc, fd2sfd,
  amiga-netinclude) resolve to `franke.ms` (Bebbo's own git) and gnu.org,
  not GitHub -- only peripheral tools are GitHub-hosted, so the mirror move
  doesn't otherwise change the build.
- Build: `make gcc PREFIX=~/opt/amiga -j8`, then `make libgcc libnix
  PREFIX=~/opt/amiga` (the first `make gcc` builds the compiler but not
  libgcc/libnix -- the first link fails with `cannot find -lgcc` until this
  runs), then `make ndk PREFIX=~/opt/amiga` (NDK 3.2, headers auto-added to
  the compiler's include path -- `<exec/types.h>` etc. resolve with no
  `-I`), then `make sdk=mui PREFIX=~/opt/amiga` for the MUI SDK (see below).
- **GCC 6.5 shift miscompiler**: Bebbo's own `-fbbb=+` ("optimize shift
  instructions") default pass can narrow a `(long)short * 1024`-shaped
  shift to HImode across an intervening call, silently dropping the
  carry-out. Build with `-fbbb=abcefilmnprsz0` (default minus `h`) always --
  this is baked into the `AMIGA_CPU` Makefile variable from the start, not
  discovered after a bug.
- **`fd2sfd` patch conflict**: `patches/fd2sfd/fd2inline.c.diff` fails with
  "Reversed (or previously applied) patch detected!" because franke's
  source is already patched. Fix: delete
  `patches/fd2sfd/fd2inline.c.diff` and the resulting
  `projects/fd2sfd/fd2inline.c.rej`, then re-run `make gcc` (resumes
  cleanly since GCC/binutils are already built).
- **GCC 6.5 ICE building `libsocket`**: a deterministic internal compiler
  error (segfault) compiling `libnix/sources/socket/socket/__initsock.c`'s
  `_sock_select`, reproducible at both `-O1` and `-O2`. RetroWP doesn't
  need libnix's socket shim anyway (the offload transport goes through
  `serial.device`/`bsdsocket.library` directly, per the M9 plan). Fix:
  edit `projects/libnix/Makefile.gcc6`'s `LIBS_TO_BUILD ?=` default (line
  ~41) to drop `libsocket`, then `rm -rf
  build-Linux-m68k-amigaos/libnix` and rebuild. Passing
  `LIBS_TO_BUILD=...` to the outer `make libnix` invocation does **not**
  work -- it isn't threaded through to the inner `Makefile.gcc6` sub-make,
  which only receives `CFLAGS` explicitly; the Makefile default must be
  edited directly.
- **MUI SDK**: not present anywhere on this machine as dev headers (only
  end-user runtime install packages). `sdk/mui.sdk` inside the amiga-gcc
  checkout is a manifest file (not a directory) naming a live download URL
  (`github.com/amiga-mui/muidev` releases, MUI 5.0-20210831-os3.lha) and
  `make sdk=mui PREFIX=~/opt/amiga` is a first-class target that fetches
  and installs it automatically -- no separate manual sourcing needed.
- Runtime: `-noixemul` + libnix (crt0 + a standalone libc), not depending
  on the target machine having `ixemul.library` -- matches the Atari
  backend's own "no GDOS required" minimal-dependency ethos.
- `AMIGA_*` Makefile variables mirror `ATARI_*` exactly (`AMIGACC`,
  `AMIGA_STRICT_CFLAGS`/`AMIGA_RELAXED_CFLAGS`, `AMIGA_INCLUDES`,
  `AMIGA_BUILD`/`AMIGA_BIN`, per-subsystem `_OBJ` lists, an
  `amiga-engine-check` link-free portability gate).

## amiberry test fixture

Emulator: **amiberry** (native package, `/usr/bin/amiberry`; a flatpak
build also exists but the native binary was used here since it behaves
better under a bare Xvfb display with no window manager -- the flatpak is
documented, in the unrelated OpenUA project on this machine, as always
opening on the real desktop display instead).

Fixture layout (all **outside the repo**, like the toolchain itself --
copyrighted Kickstart ROM + Workbench, not committed):

- `~/opt/amiga/fixture/kick31.rom` -- Kickstart v3.1 r40.63
  (A500/A600/A2000), a known-good dump.
- `~/opt/amiga/fixture/wb/` -- a real Workbench 3.1 install (copied from
  an existing local FS-UAE hard-drive directory, MagicWB + Roadshow
  already present), not a bare mount. amiberry's own docs (and this same
  lesson learned independently on the unrelated OpenUA project) are
  explicit that a bare mount isn't enough for a real boot -- `ENV:`,
  assigns, `icon.library` etc. need a real WB tree.
- `~/opt/amiga/fixture/muisrc/` -- the extracted `mui38usr.lha` payload
  (MUI 3.8), mounted as a second, non-bootable volume for staging.
- `~/opt/amiga/fixture/mounts.uae` -- the two directory-harddrive mounts,
  in the format amiberry's own GUI generates:
  ```
  filesystem2=rw,DH0:System:~/opt/amiga/fixture/wb,0
  uaehf0=dir,rw,DH0:System:~/opt/amiga/fixture/wb,0
  filesystem2=rw,DH1:Install:~/opt/amiga/fixture/muisrc,-10
  uaehf1=dir,rw,DH1:Install:~/opt/amiga/fixture/muisrc,-10
  ```
- Boot command (headless-ish, same Xvfb shape `ci_atari.sh` uses for
  Hatari -- most flags are `amiberry --help`-documented CLI options, not
  hand-guessed config keys):
  ```sh
  Xvfb :97 -screen 0 1024x768x24 &
  HOME=<scratch> DISPLAY=:97 SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    amiberry --model A500 --config ~/opt/amiga/fixture/mounts.uae \
    -r ~/opt/amiga/fixture/kick31.rom -c 4 -F 8 \
    -s use_gui=no -s nr_floppies=0 -s floppy0type=-1 -s floppy1type=-1 \
    --log
  ```
  **`-c 8` (4 MB chip) + `-F 8` (8 MB fast) errors** with "You can't use
  fastmem and more than 2MB chip at the same time" on a stock A500 board
  model -- use `-c 4` (2 MB chip) if fastmem is also wanted. `-G` alone
  did **not** reliably skip the launcher GUI when combined with
  `--config` in testing; `-s use_gui=no` did.

### Boot hang, and how it was actually diagnosed

The first boot attempts appeared to hang forever on a bare gray "Workbench
Screen" with no icons, no CLI window, ~60% host CPU busy. Screenshot-only
debugging was a dead end -- CPU-busy-but-no-visible-progress is equally
consistent with "genuinely stuck" and "quietly still working," and this
WB's `S:Startup-Sequence` has real candidates for both (an
`AddBuffers DF0:` against a disabled floppy; a Roadshow
`AddNetInterface` against an unconfigured network device; a `CD0`
DOSDriver naming `scsi.device` with no SCSI controller emulated).

**What actually worked**: since the mounted WB is a plain host directory,
not an ADF/HDF, its scripts are directly host-editable. Inserting a
`C:Echo "T<n> <label>" >SYS:trace<n>` checkpoint after every major
`Startup-Sequence` line (each to a **uniquely named** file, since real
AmigaDOS Shell append-redirection support isn't a safe assumption) turns
"is it hung, and where" into a plain `ls`/`cat` on the host after any
boot attempt, hang or not -- no more screenshot guessing. This is how it
was found that the script actually completes end-to-end in well under the
first 25s wait (all `trace00`..`trace17` markers appear, including past
`C:LoadWB`) -- the "hang" was a red herring; LoadWB was opening the
Workbench screen but drawing no icons on it, for an unrelated reason never
fully root-caused (not worth chasing further -- see next paragraph).

The fix that actually mattered: inserting `C:NewCLI
CON:10/50/620/300/Debug-Shell` right before `C:LoadWB` gives a real,
interactive, visible CLI regardless of whatever LoadWB's icon rendering is
or isn't doing -- sufficient for everything Phase 0 and the MUI install
below needed, so the icon question was left honestly unresolved rather
than chased further. (Also disabled as cheap insurance, though neither
turned out to be the actual hang: the `AddBuffers DF0: 15` line, since no
floppy is emulated; and User-Startup's Roadshow `Network-Startup` block,
since `DEVS:NetInterfaces/` is empty in this fixture.)

### Driving amiberry with xdotool (headless, no window manager)

The unrelated OpenUA project's `run-amiga-port` driver skill documents a
similar harness for a *different*, DE-attached amiberry setup; a few
things are different -- and one is a real correctness trap -- for a bare
Xvfb display with no window manager:

- `xdotool windowactivate` **fails** ("windowmanager claims not to
  support _NET_ACTIVE_WINDOW") -- there's no WM to answer the EWMH
  query. Use `xdotool windowfocus` instead (sets input focus directly via
  `XSetInputFocus`, no WM needed).
- **The host→guest mouse mapping is not 1:1, and not even a fixed
  ratio.** `xdotool mousemove <absolute x> <y>` does not reliably place
  the emulated Amiga cursor at that logical position -- large jumps land
  short (observed as low as ~25-40% of the requested delta on the Y axis
  alone, with X much closer to 1:1), consistent with the emulated mouse
  being **relative/delta-driven** once captured, put through some
  amiberry-side acceleration/scaling curve, not literally "move the host
  pointer here, the guest pointer follows." **Do not compute one large
  absolute jump from a stale screenshot.** Instead: screenshot, move a
  *small* relative step (`xdotool mousemove_relative -- dx dy`, magnitude
  under ~50-60px), screenshot again to see where the cursor actually
  landed, and repeat in small corrective steps until it's on target --
  the same visual-feedback-loop discipline this project already uses for
  Hatari/GEM mouse injection (see the `Hatari mouse injection technique`
  memory). A single click also appears to durably shift the *next*
  window/dialog's on-screen position in some cases (cause not fully
  pinned down) -- re-fetch geometry and re-screenshot after every click,
  never trust a stale offset.
- Classic AmigaDOS system requesters (the Installer's own error dialogs)
  did not respond to `Return` or `Space` in testing -- they appear to be
  genuinely mouse-only. Don't rely on a keyboard dismiss path for these.
- `xdotool type` into a live `CON:`/Shell window works fine for normal
  command entry (unlike the OpenUA project's own one-key-per-invocation
  workaround, which was specific to a custom low-level game input
  handler, not a normal Intuition console) -- but a **busy console
  actively producing output (e.g. mid-`TYPE` of a large file) silently
  swallows a following command** until it finishes. Confirm the prompt is
  idle (screenshot, or send Ctrl-C to flush/cancel) before typing the
  next command.

### Installing MUI: a real packaging gap in `mui38usr.lha`, found by reading the script

`Install-MUI` (the bundled Installer433 script) has a special-cased
`P_PREPARE` path for its root-level `MUI`/`PSI` items that unconditionally
runs `t:unlzx -m x muipack.lzx ... t:mui.inst/` before copying from the
extracted staging dir. **Neither the locally-archived copy of
`mui38usr.lha` nor a fresh download from Aminet
(`aminet.net/util/libs/mui38usr.lha`) actually contains a `muipack.lzx` or
`unlzx` anywhere in the archive** -- confirmed by `lha l` against both
files, not assumed. Running the installer as intended fails with `COPYLIB:
Can't open file "t:mui.inst/MUI"` (`DOS Error Type: Unable to locate a
file or drawer`) every time, regardless of installer prompts or working
directory.

This only matters for the *root* `MUI`/`PSI` program copy -- the actually
load-bearing files, `MUI/Libs/*.library` and `*.class` (`muimaster.library`
at 166 KB, plus `muigfx`/`muihelp`/`muilocale`/`muipop`/etc.), are **plain,
already-uncompressed files** in the archive, copied by a different,
unaffected code path when the installer runs at all. Given the broken path
only gates the non-essential root items, the fix taken was to skip the
Installer script's automated run entirely and reproduce the load-bearing
part by hand:

```
cp -a MUI/Libs/. SYS:MUI/Libs/       ; muimaster.library + friends
```
plus one line added to `S:User-Startup`:
```
Assign >NIL: LIBS: SYS:MUI/Libs ADD
```

**Verified, not just copied and assumed**: a tiny C89 program
(`OpenLibrary("muimaster.library", 0)`, written to a file so the result is
host-readable regardless of what the screen shows) was cross-compiled with
this same toolchain and run under amiberry against the fixture. Output:
`MUIMASTER_OK`. This is the fixture's "hello world" proof for Phase 0's AC
*and* independent confirmation that MUI itself is genuinely loadable here,
ahead of Phase 2's real GUI shell work.

## Status

- ✅ Toolchain: `m68k-amigaos-gcc 6.5.0b` + binutils + NDK + MUI SDK, fully
  built and verified (`m68k-amigaos-gcc --version`; a trivial program
  compiles to a real `AmigaOS loadseg()ble executable/binary`).
- ✅ `AMIGA_*` Makefile scaffolding, mirroring `ATARI_*`.
- ✅ Bootable amiberry fixture: real Workbench 3.1 + MUI (libs/classes)
  installed and confirmed loadable via a real cross-compiled test binary.
  Phase 0's stated AC ("a hello world hunk executable boots under amiberry
  to a visible result") is met and exceeded (MUI itself verified loadable,
  not just a boot).
- Known, deliberately unchased gap: Workbench icons never render on this
  fixture's `LoadWB` (root cause not identified -- worked around with a
  `NewCLI` debug shell, since nothing in Phase 0 or the MUI install needed
  Workbench's own icon view). Revisit if a later phase's verification
  genuinely needs Workbench icons specifically (Phase 2's MUI shell opens
  its own screen/window and does not depend on this).
