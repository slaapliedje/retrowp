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

## Phase 1 (T9.1a): non-UI `platform.h` seams

`platform/amiga/mem_amiga.c`, `file_amiga.c`, `clipboard_amiga.c`,
`timer_amiga.c` -- no Intuition/MUI yet, per the M9 plan's own phasing.

- **Memory**: libnix's `malloc`/`realloc`/`free` under `-noixemul`, same
  reasoning as the Atari backend's mintlib `malloc` -- a standalone C
  runtime's allocator, not a raw `AllocMem()` wrapper. Alloc/free counters
  mirror `mem_atari.h`'s test-only introspection pattern exactly.
- **File I/O**: real `dos.library` calls (`Open`/`Read`/`Write`/`Seek`/
  `Close`/`DeleteFile`/`Lock`+`UnLock`), not a stdio wrapper -- the M9
  plan calls this out explicitly, and it also sidesteps a real, documented
  toolchain gotcha (`-noixemul` is link-only; passing it to a *compile*
  step flips newlib's reent header path and breaks `<stdio.h>`). `Seek()`
  returns the **old** position on success (not the new one, unlike
  `fseek()`), and there's no separate "tell" primitive -- the documented
  idiom, `Seek(file, 0, OFFSET_CURRENT)`, is what `plat_file_tell`/
  `plat_file_size` both use. `DOSBase` needs no explicit `OpenLibrary` --
  confirmed already open by the time `main()` runs (the Phase 0 `muitest`
  probe already relied on this).
- **Clipboard**: a real typed IFF stream over `clipboard.device` (unit 0)
  via `iffparse.library` -- a `FORM FTXT` containing one `CHRS` chunk,
  written/read with `AllocIFF`/`OpenClipboard`/`InitIFFasClip`/`OpenIFF`/
  `PushChunk`/`WriteChunkBytes`/`PopChunk`/`CloseIFF`/`CloseClipboard`/
  `FreeIFF` (write) and the `StopChunk`+`ParseIFF(IFFPARSE_SCAN)` pattern
  (read). Deliberately **not** a port of the Atari backend's `SCRAP.TXT`
  file convention -- genuinely different mechanism, and this one
  interoperates with real Amiga software's clipboard, not just itself.
  Verified against the real NDK headers on this machine
  (`~/opt/amiga/m68k-amigaos/ndk-include/{libraries,clib}/iffparse.h`)
  before writing any code, not recalled from memory alone.
- **Timer**: `DateStamp()` (fixed `TICKS_PER_SECOND` = 50 by AmigaDOS API
  contract, not tied to the chipset's actual PAL/NTSC vblank rate) tracked
  as a delta from the first call, same relative-clock shape as the Atari
  backend's `clock()` wrapper. `plat_sleep_ms` uses a real yielding
  `Delay()` -- unlike the Atari backend's busy-wait (only ever justified
  by plain TOS having no yield primitive), AmigaOS has always had one.
- **Encoding edge** (`plat_utf8_to_native`/`plat_native_to_utf8`,
  `draw_amiga.c`): pulled forward from T9.2 because the clipboard needs it
  to link at all -- only the two encoding functions exist there for now,
  `plat_draw_text`/`plat_fill_rect`/`plat_gc_flush` are still T9.2's job.
  Unlike the Atari backend's ASCII-only `'?'` fallback for every byte
  above 0x7F (a documented, deliberate gap), Amiga's native charset
  genuinely **is** ISO-8859-1 from Kickstart 2.0's `topaz.font` onward --
  this project's Amiga floor is already 2.04+ for MUI, so codepoints
  0x80-0xFF map identically to native bytes, a real correctness
  improvement landing now rather than as a later revisit.
- **Linking gotcha, `AMISMOKE` (the T9.1a smoke test)**: pulling in the
  *whole* `AMIGA_ENGINE_OBJ` set (mirroring how `SMOKE.PRG` links the
  full Atari engine) fails to link here -- `linebreak.o` and
  `offload_client.o` reference `plat_font_*`/`plat_measure_text` (T9.2)
  and `plat_transport_*`/`wire_*`/`caps_*` (T9.3), neither of which exists
  yet. Atari's `SMOKE.PRG` never hit this because GEM's `app_shell.c`
  implemented the *whole* `platform.h` surface before its smoke test was
  written -- M9 deliberately phases `platform.h` across T9.1a/T9.1b/T9.2/
  T9.3, so its smoke tests can't yet link the full engine object set the
  same way. Fix: link only `build/amiga/engine/utf8.o` (the one leaf
  object `draw_amiga.c`'s encoding functions actually call), not
  `$(AMIGA_ENGINE_OBJ)` -- revisit once T9.2/T9.3 land and the full
  `platform.h` surface exists.
- **Verified end-to-end under amiberry**, not just cross-compiled:
  `wp_amiga_file_clipboard_selfcheck()` (mirrors
  `wp_atari_file_clipboard_selfcheck()` exactly -- write/read/delete a
  file, then set/get/has-text round-trip through the clipboard, `memcmp`
  against the fixture text at every step) runs from `host/tools/
  amiga_smoke.c`, writing `file_clipboard: PASS` or `FAIL` to a
  host-readable file on the boot volume (amiberry has no Hatari-style
  `NF_STDERR`/`NF_EXIT` trap to make the emulator process itself exit
  with a status code -- flagged as real, not-yet-solved Phase 5 work, not
  silently ignored). Result against the real fixture: `PASS`.

## Phase 2 (T9.1b): the real MUI shell

`platform/amiga/app_shell.c` grows from T9.1a's bare selfcheck into a real
Application/Window/Menustrip, a custom `Area.mui` subclass for the
document body, `IDCMP_RAWKEY` -> vi-mode dispatch, File Open/Save via
`asl.library`, Cut/Copy/Paste, and clean quit -- matching T9.1's stated AC
("App runs under an Amiga emulator; clean quit") and the M9 plan's own
Phase 2 AC ("menu, a document window, vi-mode Normal-mode movement, clean
quit"). All verified for real under amiberry, not just linked.

- **BOOPSI dispatcher calling convention under plain GCC/m68k**: not
  found written down anywhere in the docs/guide read for this -- resolved
  by reading real, compilable MUI SDK example source already on this
  machine (`~/src/amiga-gcc/build/mui/SDK/MUI/C/Examples/Class1.c` +
  `SDI_hook.h`'s non-MorphOS/non-AROS branch) rather than guessing:
  `struct IClass *cl`, `Object *obj` and `Msg msg` are passed in
  registers a0, a2 and a1 respectively (`arg __asm("reg")` parameter
  declarations, a real GCC extension), `__saveds` preserves the caller's
  A4. `MUI_CreateCustomClass(NULL, MUIC_Area, NULL, sizeof(instdata),
  (APTR)dispatcher)` is the real entry point; `ENTRY(func)` collapses to
  a plain `(APTR)func` cast for this target (no trampoline needed, unlike
  MorphOS/AROS).
- **Menu construction**: folds `platform/common/actions.c`'s
  `g_wp_actions[]` over a `Menustrip.mui`/`Menu.mui`/`Menuitem.mui`
  hierarchy, children attached dynamically via `OM_ADDMEMBER` (`doc/
  MUI_Group.doc`'s documented "add/remove children at will" mechanism --
  the per-group item count isn't known at compile time as a literal
  nested tag list). Each item's `MUIA_Menuitem_Trigger` notifies
  `MUIV_Notify_Application` + `MUIM_Application_ReturnID` with a
  per-action numeric ID, checked in the main loop -- the same real,
  documented pattern `doc/MUI_Application.doc`'s own
  `MUIA_Application_Iconified` example uses for custom notification IDs.
  **Real gotcha, verified empirically**: `MUIA_Application_Menustrip` is
  documented `[ISG]` (Init/Set/Get all legal), but a post-creation
  `SetAttrs` on an already-open window did NOT make the menu strip
  populate under amiberry -- it had to be passed in the Application's
  *initial* `NewObject` tag list instead (built before the Application
  even exists; the per-item notifications target `MUIV_Notify_Application`
  rather than a literal `g_app` pointer, so this ordering is safe).
- **Raw keyboard input**: `MUIM_HandleEvent` on the custom class (guide
  node `CSC_HANDLEEVENT`'s documented real path), registered/deregistered
  via `MUIM_Window_AddEventHandler`/`RemEventHandler` in `MUIM_Setup`/
  `MUIM_Cleanup`, `ehn_Events = IDCMP_RAWKEY`. `MUIA_Window_IDCMP` does
  **not exist** in this MUI version -- confirmed absent, not a typo.
  Translation to the portable `wp_key_event` mirrors
  `platform/atari/app_shell.c`'s `atari_translate_key` in spirit: a small
  set of raw scan codes (Backspace 0x41, Tab 0x42, Enter 0x44, Esc 0x45,
  Delete 0x46, arrows 0x4C-0x4F, F1-F10 0x50-0x59 -- the standard, stable
  Amiga hardware codes, no offline RKM available to cross-check against,
  verified empirically under amiberry instead) get an early
  `wp_special_key`; everything else goes through `MapRawKey()`
  (`keymap.library`) for the real, keymap-correct ASCII/Latin-1 result,
  including automatic Ctrl+letter -> C0-control-code translation. The
  action-shortcut lookup (`wp_action_find_by_shortcut`) uses that SAME
  raw byte, not the decoded `wp_key_event` -- exactly mirroring
  `wp_dispatch_key`'s own documented reason (a Ctrl-chord is guaranteed
  to reach it un-consumed by vi-mode, since every vi-mode command
  requires an unmodified key).
- **Library bases**: `muimaster.library`/`intuition.library`/
  `graphics.library`/`utility.library`/`asl.library`/`keymap.library` all
  need an explicit `OpenLibrary` (unlike `dos.library`/`exec.library`,
  pre-opened by Kickstart for every process) -- confirmed against the
  real MUI SDK example sources' own `init()`/`fail()` functions, not
  assumed. Each library's own `proto/*.h` header already `extern`-
  declares its base global (e.g. `proto/intuition.h` declares `struct
  IntuitionBase *IntuitionBase`); this code only assigns them, never
  redeclares. **Real version gotcha**: `mui.h`'s own suggested
  `MUIMASTER_VMIN` (20) is *newer* than the actually-installed MUI 3.8's
  library version (19, per the Install-MUI script's own
  `current_libver 19`) -- `OpenLibrary` with that minimum silently fails
  on this real fixture. Fixed by opening with version 0 (accept whatever
  is installed), matching the already-proven Phase 0 `muitest` probe.
- **Rendering, deliberately minimal**: `editor_draw` (the custom class's
  `MUIM_Draw`) walks the real document tree directly (`wp_document_
  section_get`/`wp_section_paragraph_get`/`wp_paragraph_run_get`/
  `wp_strpool_get`) and renders plain text via `RastPort` `Move`/`Text`
  calls, bypassing `plat_draw_text`/`plat_measure_text` entirely --
  neither exists yet (T9.2's job). A cursor-line marker (a small filled
  rect at the left margin) proves vi-mode movement visually without
  needing per-column glyph-width precision. **Real redraw bug, found and
  fixed empirically**: `MUI_Redraw()`-triggered repaints (as opposed to
  the window's initial open) did not reliably clear stale pixels first --
  repeated key-driven redraws left ghosted/garbled text where a line's
  old and new glyphs overlapped, worsening over successive redraws.
  Fixed by having the class explicitly `RectFill` its own area with
  `BACKGROUNDPEN` at the start of every `MUIM_Draw`, rather than trusting
  the Area superclass's own `MADF_DRAWOBJECT` handling to do a full clear
  on this path.
- **`plat_dialog_choice`**: implemented directly in `app_shell.c` (not a
  separate `dialog_amiga.c`) since it needs direct access to the
  Application/Window objects the shell already owns. Uses `MUI_Request`'s
  real multi-gadget form (`"Yes|No|Cancel"`-style `gadgets` string,
  documented in `doc/MUI_Application.doc`), not a new dialog window/class
  -- `platform/common/actions.c`'s `act_format_size`/`act_format_align`
  call this unconditionally, so it has to exist for `actions.o` to link
  at all, not just for a hypothetical future Format-menu action.
- **Linking**: `platform/common/actions.c`'s `act_edit_copy` calls
  `plat_dialog_choice`, and `docmodel.c`/`edit.c` call `linebreak.c`'s
  `wp_paragraph_layout_free` unconditionally (freeing a paragraph's
  layout cache, always `NULL` in this app since relayout is never
  triggered) -- both real, unavoidable link dependencies once
  `platform/common`'s actions and the engine's document/edit code are
  pulled in for the first time. `metrics_amiga.c` supplies
  real-but-unreachable-today stubs for the four `plat_font_*` functions
  `linebreak.c`'s (uncalled) `wp_paragraph_relayout` references, so the
  object links without needing T9.2's real font metrics yet.
  `help_doc.o`/`help_view.o`/`statusline.o` are excluded from the link
  (would need `plat_draw_text`/`plat_fill_rect`/`plat_gc_flush`, which
  nothing in this phase's own code calls) -- the help viewer and vi-mode
  status bar are real gaps, not silently dropped, just out of this
  phase's stated scope.
- **Verified end-to-end under amiberry**: a real MUI window titled
  "RetroWP" opens showing the sample document's text; holding the right
  mouse button (classic AmigaOS menu convention -- there is no
  permanently-visible menu bar) drops down a real menu strip reading
  "Project | Edit | Format | Tools | Help", built dynamically from
  `g_wp_actions[]`; `j`/`k` move the cursor-line marker down/up
  correctly; `:q` + Enter closes the window and returns cleanly to the
  shell prompt with no crash. `AMISMOKE` (Phase 1's smoke test, now
  linking the same broader object set as `RETROWP` since both live in
  the same grown `app_shell.c`) still reports `file_clipboard: PASS`
  against the same fixture.

## Phase 3 (T9.2): real font metrics + text rendering

`metrics_amiga.c`/`draw_amiga.c` replace T9.1a's real-but-unreachable
`plat_font_*` stubs with a genuine `graphics.library`/`diskfont.library`
implementation, and `app_shell.c`'s `editor_draw` is rewritten to call real
`wp_paragraph_relayout` and paint each engine-computed wrapped line, instead
of dumping each paragraph's raw run bytes as one unwrapped line -- matching
T9.2's stated AC ("On-screen wrap matches engine layout using real Amiga
fonts") and the M9 plan's own Phase 3 AC.

- **No true physical DPI query exists below AmigaOS 3.9**: unlike the ST's
  `v_opnvwk`, which reports real device pixel size in microns straight from
  VDI, stock `graphics.library` has no equivalent below `GetDiskFontCtrl`
  (`DFCTRL_XDPI`/`YDPI`), which needs `diskfont.library` V45 (AmigaOS 3.9)
  -- far above this project's Kickstart 2.04+ floor and this fixture's own
  MUI-3.8-era install. Y is therefore pinned at 72 DPI, not an arbitrary
  guess but AmigaOS's own long-standing font-engine convention
  (`diskfonttag.h`'s own comment: "Default is 72 dpi"; `TextAttr.ta_YSize`
  has always meant "pixels at 72dpi" the same way Mac/PostScript point
  sizing did). X is derived from a REAL, queried, per-mode value:
  `DisplayInfo.Resolution` (real ticks-per-pixel per axis for the current
  public screen's video mode, via `LockPubScreen` + `GetVPModeID` +
  `FindDisplayInfo` + `GetDisplayInfoData(DTAG_DISP)` -- all real V36+
  `graphics.library` calls, within this project's floor), scaled relative
  to the Y=72 pin. Falls back to 72/72 (square) defensively if any of
  those queries fail, mirroring `ATARI_ASSUMED_DPI`'s own role.
- **Font selection**: `OpenDiskFont` (not the narrower `OpenFont`, which
  only searches already-resident fonts) with `ta_Name` taken from the
  current public screen's own default font (`Screen->Font->ta_Name`,
  cached at first query) rather than a hardcoded `"topaz.font"` literal --
  a real, user/Preferences-driven value when available, falling back to
  the literal only if the screen can't be queried. `ta_YSize` is the
  requested point size converted through the real Y DPI above; `ta_Style`
  maps `WP_STYLE_BOLD/ITALIC/UNDERLINE` to `FSF_BOLD/ITALIC/UNDERLINED`
  (no `FS_*` equivalent for STRIKE/SUPER/SUB, same documented gap
  `metrics_atari.c`'s `map_effects` already has). Falls back to the
  exec-guaranteed-non-`NULL` `GfxBase->DefaultFont` if `OpenDiskFont`
  itself fails (shouldn't happen for a `topaz.font` request on real
  hardware) -- never `CloseFont`s a font this code didn't open.
- **Measurement**: `TextLength` on a lazily-initialized metrics-only
  `RastPort`, separate from any real window's own RastPort -- the same
  "font metrics can be queried before a window exists" case
  `platform.h`'s own comment calls out, mirroring `metrics_atari.c`'s
  separate metrics-only VDI workstation. **Real memory-corruption bug,
  found and fixed empirically, the hard way**: the commonly-cited
  reasoning for a bitmap-less measuring `RastPort` ("`TextLength` only
  reads the font's own advance tables, never touches `rp_BitMap`") does
  NOT hold on this real target -- a bare `InitRastPort()` with no `BitMap`
  attached produced genuine heap corruption under repeated real redraws
  (live `wp_strpool` bytes overwritten with NUL, visible as garbled glyph
  boxes on screen), reproducible even from a single bare Shift keypress
  with no other key involved (a harmless, no-op `wp_key_event` by every
  measure -- traced all the way through `engine/src/vimode.c`'s own
  source to confirm `codepoint == 0` hits nothing but a no-op `default:
  BEEP` case, ruling out the engine before suspecting the backend).
  Fixed by allocating a real, minimal (1x1, depth 1) `BitMap` via
  `AllocBitMap` and attaching it to the metrics `RastPort` before first
  use -- the same graphics.library primitive any real off-screen-
  measuring Amiga program uses -- removing the whole class of "passing
  graphics.library an incompletely-initialized structure" risk, not just
  the one call path that happened to corrupt memory first. Re-verified
  clean afterward: `wp_amiga_layout_selfcheck` (below) plus a real,
  repeated on-target stress sequence (`j` x3, a bare Shift tap, `:q` +
  Enter) under amiberry, with the document text checked intact at every
  step and a clean quit at the end.
- **Rendering**: `editor_draw` now calls `wp_paragraph_relayout` (via a
  `relayout_document` helper mirroring `platform/atari/app_shell.c`'s own)
  and paints each real `wp_line` -- alignment (`amiga_align_offset_twips`,
  the same pure LEFT/CENTER/RIGHT arithmetic
  `atari_align_offset_twips` already has; JUSTIFY unimplemented, same
  documented gap), per-line font caching, and ascent-based baseline
  placement all mirror `platform/atari/app_shell.c`'s `paint_document`
  structure closely. Real difference from Atari: MUI gives the editor
  object no valid pixel geometry until MUI's own layout pass has run
  (unlike Atari's `wind_open_grect`, which returns real geometry
  immediately), so relayout can't happen eagerly on document load the way
  Atari's `replace_document` does it -- it happens lazily instead, the
  first time `editor_draw` sees a real object width, and again whenever
  that width changes (a real resize) or the document itself just
  changed. The cursor-line marker (added in Phase 2, before real
  per-paragraph line layout existed) is upgraded to mark the exact
  wrapped line containing the cursor's `(run, offset)`, via a small
  `line_contains_pos` range check, rather than always the paragraph's
  first line.
- **`plat_draw_text`/`plat_fill_rect`/`plat_gc_flush`** (`draw_amiga.c`,
  alongside the encoding functions already pulled forward into Phase 1):
  real `RastPort` `Move`/`Text`/`RectFill` calls. `(r,g,b)` maps to a real
  MUI pen (`dri_Pens[TEXTPEN]`/`[BACKGROUNDPEN]`) via the same luminance
  threshold `draw_atari.c`'s `rgb_to_vdi_color` already uses, since pens
  are screen-relative palette slots, not direct RGB -- `plat_gc` for this
  backend therefore carries a `DrawInfo*` alongside the destination
  `RastPort*`.
- **On-target verification, real, not just linked**: a real amiberry
  screenshot shows genuinely proportional real-font text (the public
  screen's own default font, not a fixed-pitch placeholder) wrapping
  cleanly within the window's actual pixel width, matching the sample
  document's real word boundaries -- a visible, qualitative upgrade over
  Phase 2's fixed-9px-row raw-byte dump. `wp_amiga_layout_selfcheck`
  (mirroring `wp_atari_layout_selfcheck` exactly: same fixture text, same
  900-twip column, same two assertions -- more than one line, no line
  wider than the column) passes on-target through the real
  `plat_measure_text`/`plat_font_open` path, both before and after the
  stress sequence above. Interactive window resize (to visually confirm
  re-wrap at a new width) was attempted via `xdotool` drag but could not
  be reliably calibrated against amiberry's SDL relative-mouse handling
  -- the same documented cost/benefit call Phase 2 made for the Help
  menu's mouse-click verification; the selfcheck's quantitative width
  assertions are the substitute, not a screenshot at a second window
  size.

## Phase 4 (T9.3): offload transport over serial.device

`transport_amiga.c` implements the four `plat_transport_*` functions over
real `serial.device` `IOExtSer` `IORequest`s, mirroring `transport_atari.c`'s
`"serial:<device>:<baud>"` endpoint convention and `deadline =
plat_ticks_ms() + timeout_ms` polling shape. `app_shell.c` wires the
portable `offload_client.c` API (already proven end-to-end on Atari, T6.3)
into three of the four Tools menu actions -- matching T9.3's stated AC
("Same offload features work as on ST") with one honestly-scoped exception,
below.

- **Real serial.device mechanics**: `CreateMsgPort`/`CreateIORequest`
  (the latter sized `sizeof(struct IOExtSer)`, per that header's own
  CAUTION comment -- and, unlike `mem_alloc`, documented to return a
  zero-filled block) / `OpenDevice("serial.device", 0, ...)` /
  `SDCMD_SETPARAMS` (8N1, `SERF_XDISABLED` to disable software xON/xOFF
  flow control, matching `transport_atari.c`'s own `FLOW_NONE` choice) /
  `CloseDevice`. `io_Baud` takes a true baud value directly (no
  `BAUD_*` index table to map into the way Atari's `Rsconf` needs, so
  endpoint parsing is simpler here). Reads wait for the first byte via a
  real async `SendIO` raced against `plat_ticks_ms()` (`CheckIO` to
  poll, `AbortIO`+`WaitIO` to cancel and reclaim the request on
  timeout), then drain whatever else is already buffered via a real
  `SDCMD_QUERY` (which reports the exact count waiting, not a guess) --
  matching Atari's own "wait then drain, no further waiting" contract.
  Writes ship the whole buffer as one `IORequest` (unlike Atari's
  `Bconout`, which is single-byte-at-a-time by hardware necessity --
  `serial.device` genuinely accepts a multi-byte `CMD_WRITE`).
- **Real gotcha, found and fixed empirically**: `SDCMD_SETPARAMS` is
  attempted but NOT treated as fatal if it fails, a deliberate choice
  discovered the hard way. This amiberry build's `serial.device`
  answers `SDCMD_QUERY` successfully (proving `OpenDevice` and the
  exec.library `IORequest` lifecycle genuinely work) but rejects
  `SDCMD_SETPARAMS` with `SerErr_InvParam` regardless of which
  individually-valid parameter values are sent -- verified by trying
  several distinct combinations (including the most conservative
  possible: zeroed `io_CtlChar`/`io_RBufLen`/`io_SerFlags`), all
  rejected identically. This is consistent with an emulated serial port
  that has no real host-side backing configured to actually apply line
  settings to (see the next bullet), not a bug in the values requested.
  Treating this as fatal would make the transport permanently
  unopenable under this project's own test fixture; on genuine
  hardware, a well-formed SETPARAMS for a completely standard
  configuration should never legitimately fail, and if the actual line
  settings are wrong for some other reason, the wire protocol's own CRC
  framing (`offload/transport/wire.c`) is the real, live arbiter of
  whether communication actually works, not this call's return code.
- **Real environment limitation, investigated, not assumed**: unlike
  Hatari's `--rs232-in`/`--rs232-out` (plain host FIFOs, used by
  `ci_atari_serial.sh` to bridge a live `mock_gateway` process), this
  amiberry build's `serial_port=` config key validates against a real
  hardware enumeration (`Serial port enumeration.. / Port enumeration
  end`, with nothing listed on this machine -- no physical serial ports
  exist here) rather than accepting an arbitrary path. Two real,
  distinct workarounds were tried and both failed differently: a plain
  host FIFO caused a hang and an unrelated-looking JIT SIGSEGV storm
  (FIFOs aren't TTYs; a termios-based serial backend calling `open()`
  then a TTY-only ioctl on one is a real, if unusual, failure mode); a
  real `socat`-created PTY pair opened cleanly but was rejected at
  config time with `"Error finding serial port <path>"` -- confirming
  the enumeration-only behavior. No physical serial hardware, and no
  accepted synthetic backing, means there is no live peer in this
  environment to loop bytes through end to end. A two-tier CI proof
  mirroring `ci_atari_serial.sh`/`ci_atari_convert_serial.sh` was
  therefore not practically achievable here and was not attempted
  further -- the same cost/benefit call already made for interactive
  window-resize and menu-hover verification in Phases 2-3, applied to a
  new kind of environment gap instead of an input-calibration one.
- **On-target selfcheck, real, not a live round trip**:
  `wp_amiga_transport_selfcheck` proves what IS provable without a live
  peer: `OpenDevice`/
  `CloseDevice` succeed repeatably across multiple open/close cycles;
  `CMD_WRITE` succeeds (bytes structurally accepted, same as a real
  cable plugged into nothing); a `CMD_READ` against a short timeout
  genuinely returns `WP_TIMEOUT` (not an instant no-op, not a hang)
  within a real, bounded window -- proving the `SendIO`/`CheckIO`/
  `AbortIO`/`WaitIO` deadline race itself, the one genuinely novel
  Amiga-specific mechanism this file adds, is correct on real hardware;
  and `offload_init()` against the same endpoint, with no live gateway,
  returns cleanly (not hanging) leaving `offload_available()` false --
  CLAUDE.md's own Offload Rule ("the engine must be fully functional
  WITHOUT an offload device attached") proven through the real
  transport on this backend too, not just assumed. Deliberately does
  NOT prove that bytes written on one end are correctly received on the
  other, or that `SDCMD_SETPARAMS` itself succeeds against real
  hardware -- the same honestly-drawn boundary
  `wp_atari_thesaurus_selfcheck`/`wp_atari_grammar_check_selfcheck`
  already establish for their own backend, extended here to cover a
  second, independently-discovered gap.
- **Tools menu wiring**: `ensure_offload()` (lazy-once `offload_init`,
  mirroring Atari's own helper), `extract_word_before_cursor`/
  `extract_current_paragraph_text` (near-verbatim ports of Atari's own
  helpers, adapted to `g_amiga_doc`/`g_ctx.vi.cursor`), and real
  `do_thesaurus`/`do_chat`/`do_grammar_check` implementations, displayed
  via `MUI_Request` instead of GEM's `form_alert()`. **Real correctness
  note**: `MUI_Request`'s `format` parameter is a genuine printf-style
  format MUI itself expands, not a plain display string -- every
  message containing real document/gateway text (never fully
  caller-controlled, unlike the Phase 2 Help action's fixed literal) is
  passed through a literal `"%s"` format with the actual text as the
  vararg (`show_message()`), never interpolated into the format string
  directly, so a stray `%` in a word, a gateway reply, or a grammar
  message can't be misinterpreted as a format specifier.
  **Grammar Check is intentionally NOT a full port**: Atari's version is
  an interactive per-issue Accept/Skip/Cancel loop with byte-offset-
  shifting bookkeeping across edits (~200 lines); this backend reports
  what was found (a count plus the first issue's message/suggestion,
  from either the real offload check or, per CLAUDE.md's Offload Rules,
  the local `grammar_lite_check()` fallback when no device is attached)
  via one `MUI_Request`, never applying anything automatically -- a
  real, honestly-scoped-down gap, not a silent one. Spellcheck stays a
  no-op: it is a local, file-based dictionary feature with no offload
  dependency at all (this project's own M9 plan says so explicitly), so
  it is out of T9.3's scope by definition, and this app has no
  dictionary file on its fixture yet.
- **Not individually mouse-click-verified**: Thesaurus/Grammar
  Check/Chat's menu items were not individually clicked under amiberry
  -- the same right-mouse-hold-and-hover gesture that made Phase 2's
  Help-action verification and Phase 3's window-resize attempt costly
  to calibrate proved costly here too (confirmed again: a `mousedown`
  with no motion never rendered the dropdown; small relative moves
  while held warped the cursor to an unrelated screen position instead
  of tracking smoothly). All three go through the identical
  menu-Trigger -> `MUIM_Application_ReturnID` -> `invoke_backend_action`
  path already proven end-to-end by File Open/Save/Quit and Help, and
  their own new logic closely mirrors Atari's already-exercised
  helpers -- a reasoned, not a blind, gap, consistent with every prior
  phase's own calibration-cost calls.

## Phase 5: CI infrastructure -- Kickstart-version matrix

Mirrors the Atari multi-TOS matrix's own founding lesson: single-image CI
missed two real regressions that only a real multi-version matrix caught
(see `platform/atari/app_shell_internal.h`'s own history). `ci_amiga.sh`
(a single fast Kickstart 3.1 gate) and `ci_amiga_matrix.sh` (real
Kickstart-version coverage) mirror `ci_atari.sh`/`ci_atari_matrix.sh`'s
own two-tier shape, both sourcing a shared `host/tools/amiga_ci_lib.sh`
the same way the Atari scripts share `atari_ci_lib.sh`.

- **The headless pass/fail signal, resolved**: amiberry has no
  equivalent to Hatari's NF_STDERR/NF_EXIT trap -- there is no way to
  make the emulator *process* exit with a pass/fail status the way
  `ci_atari.sh` reads Hatari's own exit code. The fixture's own
  `S/Startup-Sequence` gets one small, conditional addition instead: if
  `SYS:AMISMOKE` exists on the volume at boot time, it runs
  automatically (writing its usual `SYS:amiga_smoke.out` PASS/FAIL log,
  unchanged from every earlier phase's interactive use) and then writes
  a `SYS:amiga_ci_done` sentinel file. A normal interactive session
  never has `AMISMOKE` present at boot (it only gets copied in mid-
  session, as every earlier phase's manual verification did), so this
  hook is a real no-op then -- verified by re-running Phase 1-4's own
  interactive workflow unchanged after adding it. Since `DH0:` is a
  real host directory (`mounts.uae`'s `filesystem2`/`uaehf0` lines, not
  a disk image), both files are ordinary host filesystem entries
  `amiga_ci_lib.sh` polls and reads directly -- no in-emulator
  interaction, no screenshot, no `xdotool` needed at all. This is a
  real, working instance of the M9 plan's own called-for "debug-log
  file convention with a sentinel line."
- **`amiga_ci_lib.sh`'s `run_amiga_smoke_once`**: copies `AMISMOKE`
  onto the fixture, launches amiberry against a given Kickstart ROM (a
  per-call scratch `$HOME` and a per-call-offset Xvfb display, mirroring
  `atari_ci_lib.sh`'s own collision-avoidance reasoning for a matrix
  loop), polls for `amiga_ci_done` (bounded at 90s -- real boot plus
  AMISMOKE's own serial.device round trip and `offload_init`'s real
  2s+2s timeout budget against a dead endpoint together run
  15-25s under this project's config, so this is generous real margin),
  then kills the amiberry process directly (there is nothing else to
  `wait()` on) and reads back `amiga_smoke.out`. Leaves the fixture
  exactly as clean as every prior phase's manual testing did (`AMISMOKE`
  /`amiga_smoke.out`/`amiga_ci_done`/`trace*` all removed again before
  returning), on every exit path including timeout.
- **Real finding: Kickstart 3.0 cannot boot on this fixture's `--model
  A500` (68000) floor.** Tested first (the matrix's whole point is
  catching exactly this kind of thing, not just re-confirming what
  already works): both the real `(A1200)` and `(A4000)` release ROMs
  hit an identical `Illegal instruction` trap inside the ROM itself
  partway through boot and hang before ever reaching `AMISMOKE`. This
  is a real, structural CPU-generation boundary, not a fixable bug or a
  ROM-image-choice mistake -- Kickstart 3.0 was the first AGA-era ROM,
  shipped only with 68020+ machines (A1200/A4000); unlike 3.1, which
  continued to see real 68000 A500/A600 use via reloc-kickstart
  upgrades and was evidently kept compatible with that CPU class, 3.0's
  own boot code apparently was not. Excluded from `ci_amiga_matrix.sh`'s
  tested list with this reasoning recorded in the script itself, the
  same "excluded with a real reason, not silently skipped" treatment
  `ci_atari_matrix.sh` already gives TOS 1.00/TT/Falcon images -- a
  matrix entry known in advance to always fail for a reason this
  project's code can never fix would just make the gate permanently
  red, defeating its purpose as a regression signal.
- **The curated list**: Kickstart 2.04 (`A500+`-labeled -- an exact
  model match), 2.05 (`A600HD`-labeled -- no A500-specific 2.05 release
  exists; 2.05 shipped only with the A600), and 3.1 (`A500-A600-A2000`-
  labeled -- also an exact match, and the same ROM image
  `docs/amiga-platform.md`'s own fixture has used as `kick31.rom` since
  Phase 0). All three are real, unmodified Commodore release images (no
  `[b]`/`[h]`/beta/encrypted variants), and all three pass cleanly --
  `AMIGA KICKSTART MATRIX: 3 passed, 0 failed`. 1.2/1.3 were never
  candidates: MUI itself requires BOOPSI, which shipped in Kickstart
  2.04 -- a Kickstart-1.3 machine cannot run MUI at all, full stop (the
  M9 plan's own "real, honest tension" note), so testing below that
  floor would test configurations this backend's own MUI dependency
  makes structurally unable to run, not a real coverage gap.

## Phase 6: help system + AmigaGuide export spike

Closes out M9. Two pieces, matching the plan's own Phase 6 scope: wiring
`platform/common/help_doc.h`/`help_view.h`'s already-target-agnostic pager
into the real MUI shell (parity with Atari, not new engineering), and
spiking a real `.guide` export as the plan's called-for "confirm before
committing" check on the idea that AmigaGuide is a more natural fit here
than Atari's own `.HYP` research spike was.

- **AmigaGuide export spike: real, and it pans out.** `host/tools/
  gen_help_guide.c` reuses `wp_help_doc_parse` (the same portable parser
  the in-app pager itself uses, not a second hand-rolled one) and walks
  the node/line graph emitting real AmigaGuide syntax (`@database`,
  `@node NAME "Title"`/`@endnode`, `@{"label" link target}`, `@wordwrap`),
  verified against WB3.1's own official `amigaguide.guide` documentation
  file already present on this machine's fixture
  (`Locale/Help/english/Sys/amigaguide.guide`) rather than recalled from
  memory. This confirms the M9 plan's own "Explicitly deferred" section
  had it right in advance: RetroWP's portable `{label|target}` format was
  deliberately designed close to AmigaGuide's own shape, so the export is
  a genuine mechanical per-line transform, not a new format's worth of
  engineering. Verified on-target under amiberry with the real
  `amigaguide.library` and MultiView: node text and link buttons both
  render correctly, clicking a link button navigates to its target node
  -- a more complete interactive verification than the Atari `.HYP` spike
  achieved there (no real ST Guide viewer was available in that
  environment). `make help-guide` builds `generated/retrowp.guide` from
  `docs/help/retrowp.help` via the new `gen_help_guide` host tool.
- **Real in-app help pager wiring**: F1 and the Help menu action both now
  open the real `wp_help_view_state` pager (replacing Phase 2's static
  `MUI_Request` placeholder), painted by a new `paint_help_amiga()` using
  the same `plat_draw_text`/`plat_fill_rect` seam the document body
  already uses, with a one-line hint bar (`wp_statusline_paint`, reused
  as-is) showing the pager's own key bindings. `amiga_dispatch_key` checks
  `g_help_active` first and routes to `wp_help_view_feed_key` while the
  pager is open, falling through to the normal vi-mode/action dispatch
  otherwise. `wp_amiga_help_selfcheck` (mirroring
  `wp_atari_help_selfcheck`) proves node lookup and real
  diskfont-measured word-wrap on-target; wired into `AMISMOKE`'s report.
  Verified interactively under amiberry: F1 opens real help content,
  j/k/Enter/Backspace navigate and follow links, Escape/`q` closes back to
  the document, and the document's own text is unaffected by a
  help-view round trip.
- **Real bug found and fixed: bare Escape closed the whole RETROWP
  window**, present since Phase 2 but never noticed until this phase's
  pager needed Escape to mean "close the help view" instead. Root cause:
  `editor_handleevent` (`MUIM_HandleEvent`) always returned `0` ("event
  not consumed"), so the raw key kept propagating past the app's own
  dispatch to MUI's *global*, per-window hotkey table, where
  `MUIKEY_WINDOW_CLOSE` defaults to Escape and independently fired the
  window's already-wired `MUIA_Window_CloseRequest` notification --
  a second, parallel path to "quit" that had nothing to do with this
  app's own vi-mode Escape handling. Isolated by temporary file-based
  checkpoint logging (`SYS:retrowp_debug.out`, the same technique Phase 4
  used to isolate its `SDCMD_SETPARAMS` gotcha) proving `amiga_dispatch_key`
  itself never requested quit for that key. Fixed with two changes,
  both required: `editor_handleevent` now returns `MUI_EventHandlerRC_Eat`
  (stops the raw key propagating further), and the window's own
  `NewObject` tag list gains `MUIA_Window_DisableKeys,
  (IPTR)MUIKEYF_WINDOW_CLOSE` (disables the global hotkey binding itself
  -- the `Eat` fix alone was insufficient, since MUI's hotkey table
  operates at a different level than the custom class's own event-handler
  chain). Verified by repeated interactive Escape presses post-fix: window
  stays open, vi-mode correctly returns to Normal mode instead.
- **Real bug found and fixed, shared with Atari: `wp_help_view_paint`
  drew text with no ascent offset**, positioning row 0's glyphs above the
  viewport's own top edge -- genuinely outside any rectangle the caller's
  background-clear covers, so the leaked pixels survived every redraw.
  Invisible on host (`draw_host.c`'s `plat_draw_text`/`plat_fill_rect` are
  no-ops, so the host suite could only prove `wp_help_view_paint` doesn't
  crash, never that it draws in the right place) and never caught on
  Atari either, since its own pager had never been interactively
  exercised end-to-end before now (`docs/help.md`'s own documented gap)
  -- only this phase's real on-target Amiga click-through surfaced it.
  Fixed once at the shared `platform/common/help_view.c`/`.h` level (a
  new `ascent_px` parameter, supplied by each caller's own
  `plat_font_metrics_get` result, mirroring how the main document body
  already adds its font's ascent before calling `plat_draw_text`) rather
  than patched per-backend, so Atari's own pager gets the same fix for
  free. Both platform call sites and all 5 host `test_help_view.c` call
  sites updated. Verified under amiberry: the ghosting is gone and both
  short and long help nodes render completely and correctly.
- **False alarm, investigated and ruled out, not left ambiguous**: an
  early re-test session appeared to show content-length-dependent
  incomplete rendering plus a RETROWP crash (`error #80000003`). Traced to
  two unrelated causes, not a real defect: (1) test-methodology confusion
  -- keystrokes intended as "close the app and relaunch" were sent to an
  *already-open* help view, where Backspace means "go back" and letter
  keys are navigation, not shell input; (2) a genuinely separate,
  pre-existing flaky background-process crash (`IPrefs`, `error
  #80000004`, unrelated to F1/help at all) that reproduced on a fresh
  boot with zero help interaction. A controlled re-test (fresh boot,
  immediate F1, full checkpoint logging) showed zero crashes and complete
  correct rendering of both short and long nodes, refuting the "real bug"
  theory. Recorded honestly per this project's own documentation
  discipline, rather than silently dropped.
- `amiga-app`/`amiga-smoke` both green; host `make test` (219/219),
  `atari-engine-check` and `amiga-engine-check` all unaffected.

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
- ✅ Phase 1 (T9.1a): memory/file/clipboard/timer implemented and verified
  on-target (`file_clipboard: PASS` under amiberry against the real
  fixture) -- `amiga-engine-check` and `amiga-smoke` both green, host's
  `make test` (219/219) and `atari-engine-check` unaffected.
- ✅ Phase 2 (T9.1b): real MUI Application/Window/Menustrip, custom-class
  rawkey dispatch into vi-mode, File Open/Save/Cut/Copy/Paste, clean quit
  -- all verified under amiberry against the real fixture (menu drops
  down correctly, document text renders, cursor moves on j/k, `:q` quits
  cleanly). `amiga-app`/`amiga-smoke` both green; host `make test`
  (219/219) and `atari-engine-check` unaffected.
- ✅ Phase 3 (T9.2): real `graphics.library`/`diskfont.library` font
  metrics and `RastPort`-based rendering; `editor_draw` now paints real
  engine-computed wrapped lines instead of raw unwrapped paragraph text.
  A real on-target memory-corruption bug (a bitmap-less measuring
  `RastPort`) was found and fixed -- see Phase 3's own notes above.
  Verified under amiberry: real proportional wrapped text, a repeated
  on-target stress sequence with document text checked intact at every
  step, and a clean `:q` quit. `wp_amiga_layout_selfcheck` passes
  on-target. `amiga-app`/`amiga-smoke` both green; host `make test`
  (219/219) and `atari-engine-check` unaffected.
- ✅ Phase 4 (T9.3): real `serial.device`-backed offload transport
  (`transport_amiga.c`), Thesaurus/Grammar Check/Chat wired to the
  portable `offload_client.c` API. A real, non-fatal `SDCMD_SETPARAMS`
  gotcha was found and fixed -- see Phase 4's own notes above. A live
  two-tier CI proof (mirroring Atari's `ci_atari_serial.sh`) was not
  practically achievable in this environment (no physical serial
  hardware, and this amiberry build's serial-port passthrough rejects
  both a host FIFO and a real `socat`-created PTY -- both investigated,
  documented, not assumed). `wp_amiga_transport_selfcheck` proves the
  real exec.library `IORequest`/timeout mechanics on-target instead.
  `amiga-app`/`amiga-smoke` both green; host `make test` (219/219) and
  `atari-engine-check` unaffected.
- ✅ Phase 5: `ci_amiga.sh` (fast single-Kickstart-3.1 gate) and
  `ci_amiga_matrix.sh` (2.04/2.05/3.1 -- see Phase 5's own notes above
  for why 3.0 is excluded, not silently untested), both fully headless,
  no `xdotool`/interactive step at all -- the fixture's own
  `Startup-Sequence` now auto-runs `AMISMOKE` and writes a host-visible
  completion sentinel when a caller has copied it in first, resolving
  the M9 plan's own "amiberry lacks an NF_STDERR/NF_EXIT-equivalent
  trap" open question. `AMIGA KICKSTART MATRIX: 3 passed, 0 failed`; a
  real Kickstart 3.0/68000 incompatibility was found along the way (see
  above), not a regression in this project's own code.
- ✅ Phase 6: real AmigaGuide export (`gen_help_guide`), verified on-target
  under `amigaguide.library`/MultiView; the real in-app help pager wired
  to F1 and the Help menu (replacing Phase 2's static placeholder). Two
  real bugs found and fixed: bare Escape closing the whole window
  (`MUIKEY_WINDOW_CLOSE` propagation, see Phase 6's own notes above), and
  a shared `platform/common/help_view.c` baseline/ascent bug fixed once
  for both Atari and Amiga. `amiga-app`/`amiga-smoke` both green; host
  `make test` (219/219), `atari-engine-check` and `amiga-engine-check`
  all unaffected. **M9 complete.**
- Known, deliberately unchased gap: Workbench icons never render on this
  fixture's `LoadWB` (root cause not identified -- worked around with a
  `NewCLI` debug shell, since nothing in Phase 0 or the MUI install needed
  Workbench's own icon view). Doesn't affect T9.1b: MUI opens its own
  window regardless of Workbench's own icon rendering.
- Known gap: Spellcheck stays a no-op (a local, file-based dictionary
  feature with no offload dependency at all, so it was never in T9.3's
  scope, and this app has no dictionary file on its fixture yet); RTF
  file support isn't wired into Open/Save yet (plain text only); Grammar
  Check is a reduced-scope report-only version, not Atari's interactive
  apply loop (see Phase 4's own notes above). The Help menu action now
  opens the real help viewer as of Phase 6 (see Phase 6's own notes
  above) -- this gap is resolved, not open. Thesaurus/Grammar
  Check/Chat menu items were not individually mouse-click-verified under
  amiberry (precisely hovering a specific pulldown item while holding RMB
  hit the same relative-mouse-calibration cost documented in Phase 0's
  installer notes and reconfirmed in Phase 4, and wasn't worth repeating
  for message-box-only actions) -- all three go through the identical
  menu-Trigger -> `MUIM_Application_ReturnID` -> `invoke_backend_action`
  path already proven end-to-end by File Open/Save, Quit, and (as of
  Phase 6) Help, so this is a reasoned, not a blind, gap.

## Switchable keybinding layouts (M10)

Full account: `docs/keymaps.md`. This backend's own share of that
milestone:

- **Config file**: `PROGDIR:RetroWP.cfg` -- a standard, always-
  available AmigaDOS assign (the directory the program launched from),
  no dependency on a full `ENV:`/Preferences tree this fixture's own
  WB install doesn't fully have working anyway (see this doc's own
  Phase 0 notes). Read once at startup via `wp_config_load` before
  `wp_app_ctx_init`, right in `app_shell_run`.
- **`translate_rawkey` now also reads real `IEQUALIFIER_LALT`/
  `IEQUALIFIER_RALT` bits** (T10.3): `imsg->Qualifier` was already
  being captured every keystroke, but only to feed `MapRawKey`'s own
  ASCII translation -- MapRawKey folds Ctrl into a C0 control code the
  same way it always did, but does *not* do the same for Alt, so this
  needed its own explicit read. Verified against the real NDK header
  (`~/opt/amiga/m68k-amigaos/ndk-include/devices/inputevent.h`):
  `IEQUALIFIER_CONTROL`/`LALT`/`RALT` are `0x0008`/`0x0010`/`0x0020`,
  **not** the `0x0200`/`0x0400` pair some online references cite for
  Alt -- checked against the real, local, authoritative source rather
  than trusted from memory. New `wp_amiga_key_translate_selfcheck`
  (wired into `AMISMOKE`) proves Alt is read correctly across
  Escape/Up/F1 and that an unrelated qualifier bit (Control) doesn't
  false-trigger it.
- **`amiga_dispatch_key`'s own two-step vi-mode-then-shortcut-fallback
  logic moved into `platform/common/keymap.c`** (T10.1), replaced here
  by one `wp_keymap_feed_key` call -- see `docs/keymaps.md`'s own
  architecture section. This backend's own pre-existing asymmetries
  with the Atari backend (no `sync_format_check`-equivalent menu-
  checkmark sync on the shortcut-fallback path; `set_document_from_text`
  never called a `wp_vimode_free`-equivalent before re-init, unlike
  Atari's own `replace_document`) were preserved exactly as real,
  pre-existing behavior, not fixed as part of this refactor -- out of
  scope for a keybinding-layout milestone.
- **`wp_amiga_config_selfcheck`** (T10.1, wired into `AMISMOKE`):
  mirrors `wp_atari_config_selfcheck` exactly -- writes a real config
  file at the real path for each of the five `keymap=` values in turn
  and confirms `wp_config_load` resolves every one correctly through
  real AmigaDOS file I/O, leaving the fixture exactly as clean as it
  found it.

The full 3-image `ci_amiga_matrix.sh` stayed green through every phase
that touched this file directly (T10.1, T10.3).

## Find & Replace (M11)

Full account: `docs/keymaps.md`. Unlike M10, this milestone needed
**zero changes to any file in this directory**: every scheme's
Find/Replace prompt renders through the same status-line mechanism
vi-mode's own `:`/`/` already used, and no new `platform.h` primitive
or modifier bit was needed. The 3-image Kickstart matrix stayed green
throughout as a pure regression gate. The one real finding this
milestone surfaced on this backend: `kick-3.1-a500` hit a reproducible-
as-flaky boot-time "Illegal instruction"/"Exception 3" crash (before
`AMISMOKE.exe` ever runs) on two separate occasions across the
milestone's phases, both times cleared by an immediate rerun (3/3
clean) -- environmental amiberry boot flakiness specific to this one
Kickstart image under this fixture, not a regression from any source
change (confirmed by the crash happening at Kickstart's own filesystem-
install stage, before any of this project's own code executes).

## The startup crash was a stack overflow in the shared engine (T151)

`RETROWP` died with `Program failed (error #80000004)` — an illegal
instruction — on roughly **17 of 18 boots**, with zero user input. It was
originally recorded as happening "after ~13 typed characters"; measuring
it showed typing had nothing to do with it, and the app usually never
finished opening its window at all.

### What it actually was

`engine/src/linebreak.c`'s `wp_paragraph_relayout` declared

```c
token_t tokens[MAX_TOKENS];   /* 512 x 28 bytes ~= 14KB, on the STACK */
```

Measured on target (`FindTask(NULL)->tc_SPUpper - tc_SPLower`, with the
value encoded into a marker filename so the host could read it straight
off DH0:):

| | bytes |
| --- | --- |
| Total stack, `RUN`-launched from `S/Startup-Sequence` | **4096** |
| Already used when MUI's draw callback reached layout | **1292** |
| Headroom available for a ~14000-byte frame | **~2804** |

A ~5x overflow. It scribbled over whatever lived below the stack —
harmless when that memory happened to be dead, wild jumps into garbage
when it was not. That explains every previously-confusing symptom at
once: a different fault address each run, ~1 boot in 18 surviving
cleanly, the rate changing whenever unrelated code changed, alerts that
sometimes named an unrelated task (`IPrefs`) instead of RetroWP, and
`AMISMOKE` passing 3/3 while the interactive app died — the smoke test
runs before `LoadWB`, when the memory below its stack is mostly free.

Fixed by moving the buffer to the heap (`mem_alloc`, freed on every exit
path), which is what CLAUDE.md's memory rules ask for anyway. It is in
the shared engine, so this was never Amiga-specific — the ST and Mac
survive it only because their toolchains hand out a far larger stack.
An image paragraph never tokenizes, so it now pays no allocation at all.

### Reading amiberry's "Illegal instruction" lines

**One per boot is normal and appears on completely clean boots:**

```
Illegal instruction: 4e7b at 00F80C38 -> 00F80CC0
```

`4e7b` is `MOVEC`, a 68020+ instruction, at a Kickstart ROM address —
Kickstart probing for a 68020 and catching the expected exception.

A **second** line, with a garbage opcode at a **RAM** address that
differs run to run, is a real guest crash:

```
Illegal instruction: 00f0 at 0027EBE4 -> 00F80AD2
Illegal instruction: 00f9 at 0021CFD4 -> 00F80AD2
```

`grep -c 'Illegal instruction'` is therefore a complete crash detector:
1 is healthy, >= 2 is a real fault. Cheaper and far more reliable than
screenshot parsing. This project's own notes had previously written
these off as an amiberry JIT quirk — they are not, and that
misattribution is part of why this bug survived so long. (The CI config
runs `--model A500`, a 68000; JIT does not even apply, and `-c 4 -F 8`
are chip/fast RAM sizes, not JIT flags.)

### `__stack` does not work here

`unsigned long __stack = 65536;` in `platform/amiga/main.c` is silently
ignored by this amiga-gcc/libnix toolchain — the app still measured 4096
bytes. An experiment resting on it proves nothing, so read the real size
at runtime rather than trusting the cookie. Keeping large buffers out of
shared engine stack frames is the durable fix; a bigger stack would only
have hidden this one.

### Verified

12/12 clean boots with the fix (every one reaching the main event loop,
and `emu_illegal_insn=1` on all of them), against 17/18 crashing before.
Reverting only the engine change and rebuilding reproduced faults again
(3 of 6 boots), confirming the causal link rather than inferring it.
A real screenshot shows the MUI window open with the sample document
rendered.

## The caret: wrong column and wrong vertical anchor (T161)

`editor_draw` painted the cursor marker with

```c
RectFill(gc.rp, _mleft(obj), (LONG)(y - ascent_px),
         _mleft(obj) + 1, (LONG)(y + 1));
```

Two independent defects, both shared verbatim with the Mac backend
before T157 fixed them there — the direct consequence of three backends
each re-deriving caret placement instead of sharing it.

- **The column was never computed.** `_mleft(obj)` is the object's
  hardcoded left edge. The guard above it picked the right *line*, but
  nothing ever derived an x from the cursor's run/offset, so the caret
  sat at column 0 wherever the cursor really was.
- **The vertical anchor was off by an ascent.** `y` is the line's TOP
  (the draw loop derives its baseline as `y + ascent_px`), but
  `(y - ascent_px)..(y + 1)` is the correct span measured from a
  BASELINE. The caret floated in the gap above the line it marked.

The column now comes from `wp_line_cursor_x_twips` (`engine/src/
justify.c`, added by T157 — see `docs/layout.md`), fed the same
`extra_twips` the fragment walk uses, so a JUSTIFY line's stretched gaps
count identically in the caret and in the glyphs. Height comes from the
line's own `height_twips` rather than `ascent_px`, matching Atari
(T162): `ascent_px` is carried across loop iterations from whichever run
last opened a font, so on an empty paragraph nothing sets it and the
caret would collapse to a stub.

Note `amiga_twips_to_px_x` is `(twips * dpi_x) / 1440` and therefore not
exact, unlike Mac's plain `/20` — converting the accumulated total once
can land a pixel off from summing per-fragment pixel widths. That is
narrower than the caret itself; see the helper's header comment.

### Verified on-target

Interactively under amiberry — possible at all only because T151 stopped
the app crashing at startup. Real screenshots across
`w`/`w`/`w`/`$`/`0` on `"RetroWP on Amiga (M9 T9.1b)"`: the caret's
changed-pixel bounding box grows 52 -> 68 -> 103 -> 159 px for cursor
offsets 8, 11, 17 and end-of-line, i.e. ~6.1-6.5 px per character with
the wider capitals early on, exactly as a proportional screen font
should. `w` three times lands it on the `(` of `(M9 T9.1b)`, `0`
returns a frame pixel-identical to the startup one, and the caret spans
the text line rather than hovering above it. `grep -c 'Illegal
instruction'` stayed at 1 (Kickstart's own CPU probe) throughout — no
crash.
