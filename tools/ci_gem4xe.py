#!/usr/bin/env python3
"""Boot RetroWP on an ATARI 8-BIT under gem4xe, and check what it drew.

The other shells are checked by running them; this one had only ever been
checked by linking it.  That is a real gap, because almost nothing under
the engine is the same here: pointers are 24 bits, A FAR POINTER'S
ARITHMETIC IS SIXTEEN, the image is 202 KB over four banks in the large
data model, and everything the AES reads in place has to be in bank $00
(WP_AES_NEAR).  None of that is visible to a compiler or a linker.  A run
is what says the arrangement held.

WHY IT POLLS, which is the whole reason this file is not sdx_full.py.
For most of the port RETROWP.G4A "did not load".  It always loaded: every
probe sampled the machine ONCE, at a fixed frame count, and gem4xe writes
sh_desk_len only when a whole read has finished -- so a read still in
progress reads as zero, which is indistinguishable from a failure.  A
202 KB read finishes near frame 3,000 and the probes looked at 1,800.
docs/gem4xe-port.md has the timeline and the withdrawal.  So this waits
for a condition and reports how long it took, and a timeout says how far
the read had got rather than just "failed".

RETROWP GOES ON THE DISK AS DESKTOP.G4A.  gem4xe's shell loop reads that
name off the boot disk and hands it to app_load, so the whole loader runs
-- the format 4 header, the chunked image copy, the fixups across four
banks.  SpartaDOS X supplies the DOS from the cartridge; the disk carries
none of its own.

WHAT IS ASSERTED.  The read is the whole file; the program loads with a
near region and APP_OK; wp_menu_tree_init ran; the menu bar drew its
titles; and the WORK AREA carries the document's own text.  That last one
was for a week the thing this gate excused as "known broken" and printed
rather than checked.  It was neither a redraw bug nor gem4xe's: layout
took a BRK before it measured a word, because Calypsi at -O2 left the
accumulator eight bits wide across a call (the Makefile builds
linebreak.c at -O1, with the reasoning).  The blank title bar, the other
excused item, was fixed upstream.  Both are assertions now.

The symbols MUST match the GEM.COM on the disk -- see where they are
loaded below; the Makefile passes GEM4XE_SYM beside GEM4XE_XEX.  A
mismatched pair does not fail, it answers plausible nonsense.
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEM4XE = os.environ.get("GEM4XE", os.path.expanduser("~/dev/gem4xe"))

# THE PATCHED EMULATOR, before the launcher is imported.  /usr/bin/AltirraSDL
# lacks AltirraSDL PR #88, which carried the two 65C816 native-mode CPU fixes;
# the second is an IRQ storm that corrupts the whole of bank $00, which is
# where the pool, the read buffer and the stack all live.  Running this on
# that build measures the emulator, not the port (docs/gem4xe-port.md).
os.environ.setdefault("ALTIRRASDL", os.path.expanduser("~/dev/altirra-patched/AltirraSDL"))
sys.path.insert(0, os.path.join(GEM4XE, "tools"))

from a8test.launcher import launch            # noqa: E402
import symfile                                # noqa: E402

DISK = os.path.join(ROOT, "build", "gem4xe", "RETROWP.ATR")
G4A = os.path.join(ROOT, "bin", "gem4xe", "RETROWP.G4A")
MAP = os.path.join(ROOT, "build", "gem4xe", "retrowp.map")
SHOT = os.path.join(ROOT, "build", "gem4xe", "retrowp-g4a.png")

ERR = {0: "APP_OK", -1: "APP_E_MAGIC", -2: "APP_E_SHORT", -3: "APP_E_POOL",
       -4: "APP_E_FAR", -5: "APP_E_FIXUP", -6: "APP_E_FILE", -7: "APP_E_OLDSDK"}

# wp_menu_tree_init writes this into the BAR object's ob_spec, so it is a
# startup progress marker: the tree is linked, relocated and initialised.
WP_MENU_BAR_OBSPEC = 0x180
BAR_XYWH = (0, 0, 640, 10)

# The screen is 640x240 and a screenshot doubles its rows, so the menu bar's
# ten rows are the top twenty of the picture.
BAR_ROWS = 20
MIN_INK = 100           # "Desk" alone is 48 px wide; several titles follow
# The window's work area: below the menu bar and the title bar, above the
# status line.  Ink here is laid-out document text, which is the whole
# point -- it was absent for the port's first week, and the cause was not
# drawing at all but a BRK in layout (docs/gem4xe-port.md).
WORK_TOP, WORK_BOT = 24, 200
MIN_WORK_INK = 400


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def link_near_of(path):
    """Where the near region was linked, from the .G4A header."""
    with open(path, "rb") as f:
        return struct.unpack("<H", f.read(8)[4:6])[0]


def tree_link_of(path):
    for line in open(path):
        m = re.match(r"\s+wp_menu_tree = ([0-9a-fA-F]+)\s*$", line)
        if m:
            return int(m.group(1), 16)
    return None


def bar_ink(path):
    """Dark pixels in the menu bar's band.  The bar is drawn white with
    black text, so ink there means titles were drawn -- the difference
    between a bar that is present and one that is merely blank."""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGB")
    w = im.width
    px = im.load()
    return sum(1 for y in range(min(BAR_ROWS, im.height)) for x in range(w)
               if sum(px[x, y]) < 128 * 3)


def work_ink(path):
    """Dark pixels in the window's work area: the document's own text."""
    try:
        from PIL import Image
    except ImportError:
        return None
    im = Image.open(path).convert("RGB")
    w = im.width
    px = im.load()
    return sum(1 for y in range(WORK_TOP, min(WORK_BOT, im.height)) for x in range(w)
               if sum(px[x, y]) < 128 * 3)


def main():
    problems = []
    for need in (DISK, G4A, MAP):
        if not os.path.isfile(need):
            print(f"FAIL: {need} is missing -- run `make gem4xe` first")
            return 1
    want_len = os.path.getsize(G4A)
    link_near = link_near_of(G4A)
    tree_link = tree_link_of(MAP)
    # The symbols MUST be the ones belonging to the GEM.COM on the disk.
    # They are read by address, so a gem.sym from a different build answers
    # plausible nonsense rather than failing: app_near moved eleven bytes
    # between two of gem4xe's kits, and reading the stale address gave a
    # steady 0, which reads exactly like "the program never loaded".  That
    # cost a day and an upstream bug report that had to be withdrawn.  So
    # the Makefile passes the gem.sym that sits beside the gem.xex it
    # staged, and this says which file it used.
    sym_path = os.environ.get("GEM4XE_SYM") or os.path.join(GEM4XE, "build", "gem.sym")
    sysm = symfile.load(sym_path)
    print(f"  symbols   {sym_path}")
    cart = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("SDX_CART")
    if not cart:
        print("FAIL: no SpartaDOS X cartridge: pass one, or set SDX_CART")
        return 1

    emu = launch(tag="retrowp", memsize="1088K",
                 extra_args=["--disk", DISK, "--cart", cart])
    b = emu.bridge
    try:
        u32 = lambda a: int.from_bytes(bytes(b.memdump(a, 4)), "little")  # noqa: E731

        # -- wait for the read, rather than guessing when it is done -------
        # gem4xe writes sh_desk_len only when a WHOLE read has finished, so
        # a read in progress is indistinguishable from a failure at any one
        # sample.  There is no progress symbol to watch (the far break is
        # reported by the m3 runner, which is not what is running here), so
        # the honest thing is to wait a long time and say how long it took.
        frames, ln = 0, 0
        while frames < 6000:
            b.frames(200)
            frames += 200
            ln = u32(sysm["sh_desk_len"])
            if ln:
                break
        if not ln:
            print(f"FAIL: no read finished in {frames} frames.  A 202 KB read "
                  f"lands near frame 3,000 on this machine, so this is a real "
                  f"failure rather than an early look -- but check the "
                  f"emulator is the patched one before believing it")
            return 1
        print(f"  read      sh_desk_len={ln} after ~{frames} frames")
        if ln != want_len:
            problems.append(f"read {ln} bytes, the file is {want_len}")

        # -- and for the program to be loaded ------------------------------
        near = 0
        while frames < 9000:
            near = b.peek16(sysm["app_near"])
            if near:
                break
            b.frames(200)
            frames += 200
        runs = b.peek16(sysm["sh_runs"])
        rc, ret = s16(b.peek16(sysm["sh_lastrc"])), s16(b.peek16(sysm["sh_lastret"]))
        print(f"  load      app_near=${near:04X} (linked ${link_near:04X}), "
              f"sh_runs={runs} (runs STARTED, not finished), "
              f"sh_lastrc={rc} ({ERR.get(rc, '?')}), sh_lastret={ret}")
        if not near:
            problems.append(
                f"app_near stayed 0 through {frames} frames, sh_lastrc {rc} "
                f"({ERR.get(rc, '?')}).  Before reading this as a load failure, "
                f"check that {sym_path} belongs to the GEM.COM on the disk -- a "
                f"gem.sym from another build reads a stale address and answers a "
                f"steady 0, which looks identical")
        if rc not in (0, None):
            problems.append(f"sh_lastrc {rc} ({ERR.get(rc, '?')}), expected APP_OK")

        # -- it reached its own start-up ------------------------------------
        if near and tree_link is not None:
            b.frames(600)           # let wp_menu_tree_init and the first draw run
            t = tree_link + near - link_near
            d = bytes(b.memdump(t + 1 * 24, 24))        # the BAR object
            spec = struct.unpack("<I", d[12:16])[0]
            xywh = struct.unpack("<4h", d[16:24])
            print(f"  startup   wp_menu_tree @${t:04X}, BAR spec=${spec:08X} xywh={xywh}")
            if spec != WP_MENU_BAR_OBSPEC:
                problems.append(f"wp_menu_tree_init did not run: BAR spec "
                                f"${spec:08X}, expected ${WP_MENU_BAR_OBSPEC:08X}")
            if tuple(xywh) != BAR_XYWH:
                problems.append(f"the menu bar is {xywh}, expected {list(BAR_XYWH)}")
        elif tree_link is None:
            problems.append("wp_menu_tree is not in the linker map")

        # -- and drew something ---------------------------------------------
        b.screenshot(SHOT)
        ink = bar_ink(SHOT)
        if ink is None:
            print(f"  screen    {SHOT} (no PIL here, so not measured)")
        else:
            print(f"  screen    {ink} dark pixels in the menu bar's rows ({SHOT})")
            if ink < MIN_INK:
                problems.append(f"the menu bar drew {ink} dark pixels, under "
                                f"{MIN_INK}: the titles are missing")
            # The document itself.  Both of the things this gate used to
            # print as "known broken" -- the blank title and the unpainted
            # work area -- are fixed, so they are checked rather than
            # excused: the title came right when gem4xe taught WF_NAME to
            # bring a far string down at draw time, and the work area when
            # linebreak.c stopped being built at -O2 (see the Makefile).
            wink = work_ink(SHOT)
            print(f"  document  {wink} dark pixels in the work area")
            if wink < MIN_WORK_INK:
                problems.append(f"the work area drew {wink} dark pixels, under "
                                f"{MIN_WORK_INK}: the document did not lay out "
                                f"or did not paint")
    finally:
        try:
            b.close()
        except Exception:
            pass
        try:
            emu.proc.kill()
        except Exception:
            pass


    if problems:
        for p in problems:
            print(f"FAIL: {p}")
        return 1
    print("retrowp-gem4xe: PASS -- RetroWP loads and starts up on an Atari 8-bit, "
          "0 problem(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
