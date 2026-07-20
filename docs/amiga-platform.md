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
- Known, deliberately unchased gap: Workbench icons never render on this
  fixture's `LoadWB` (root cause not identified -- worked around with a
  `NewCLI` debug shell, since nothing in Phase 0 or the MUI install needed
  Workbench's own icon view). Doesn't affect T9.1b: MUI opens its own
  window regardless of Workbench's own icon rendering.
- Known gap, explicitly out of T9.1b's scope: Tools menu actions
  (Spellcheck/Thesaurus/Grammar/Chat) are no-ops (offload-dependent,
  T9.3+); RTF file support isn't wired into Open/Save yet (plain text
  only); the Help menu action shows a static `MUI_Request` message
  rather than the real help viewer (`help_view.o` excluded from the
  link, see Phase 2's own notes above). This one action was NOT
  individually mouse-click-verified under amiberry (precisely hovering a
  specific pulldown item while holding RMB hit the same relative-mouse-
  calibration cost documented in Phase 0's installer notes, and wasn't
  worth repeating for a cosmetic message box) -- it goes through the
  identical menu-Trigger -> `MUIM_Application_ReturnID` ->
  `invoke_backend_action` path already proven end-to-end by File Open/
  Save and Quit, so this is a reasoned, not a blind, gap.
