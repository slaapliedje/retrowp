#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Capture + verdict half of the multitasking-AES redraw gate.

Driven by ci_afros_redraw.sh once ARAnyM is running. Follows the probe's
NF_STDERR marker stream, screenshots the emulated screen at each phase,
and asserts per cover/uncover cycle:

  1. The prober's window really did cover the region it claimed. Without
     this the whole test passes vacuously if the probe silently fails to
     open a window -- "the screen is unchanged" would then be trivially
     true.
  2. Every pixel it covered is byte-identical to how it looked before,
     once the prober's window goes away. That is the whole claim:
     RetroWP serviced a real partial WM_REDRAW and repainted the exposed
     region from its own document model, with no corruption.

WHY THE MARKERS ARE HINTS, NOT TIMING
-------------------------------------
An earlier version ran `tail -F` on the log in shell and screenshotted
directly on each marker line. That looked exact and was not: markers
reached the host 5-8 seconds late, so the "covered" grab consistently
photographed an already-uncovered screen and the "uncovered" grab caught
the NEXT cycle's cover window. (The lag was `tail -F`'s one-second poll
plus each capture serialising behind the previous one, not anything in
the emulator -- reading the log directly the way follow() does below
brought it down to 0.1-0.5s.) The failure was loud rather than silent
only because assertion 1 above exists.

Waiting on a predicate rather than a timestamp is what makes that class
of mistake impossible rather than merely fixed, so each marker starts a
wait for a screen PREDICATE:
  - after "covered", poll until the region actually differs from the
    baseline (the cover is a precondition of the test, not the thing
    under test, so waiting for it weakens nothing);
  - after "restored", poll until the region stops changing between
    consecutive grabs, then compare that settled frame to the baseline.
    Settling is judged frame-to-frame, never against the baseline, so
    this cannot launder a bad repaint into a pass.
"""
import os
import subprocess
import sys
import time
from PIL import Image, ImageChops

POLL_S = 0.25
PHASE_TIMEOUT_S = 40.0
SETTLE_FRAMES = 2


def grab(display, path):
    subprocess.run(["import", "-window", "root", path],
                   env={**os.environ, "DISPLAY": display},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   check=False)
    return Image.open(path).convert("RGB") if os.path.exists(path) else None


def ndiff(a, b, box):
    """Exact count of pixels differing in any channel.

    Built from per-band masks rather than a luminance conversion: a
    one-level difference in a single channel rounds to zero luminance,
    which would quietly under-report exactly the subtle corruption this
    gate exists to catch."""
    if a is None or b is None:
        return -1
    d = ImageChops.difference(a.crop(box), b.crop(box))
    mask = None
    for band in d.split():
        m = band.point(lambda v: 255 if v else 0)
        mask = m if mask is None else ImageChops.lighter(mask, m)
    return mask.histogram()[255] if mask is not None else 0


def wait_differs(display, path, box, baseline, threshold):
    """Poll until the region differs from baseline by at least threshold."""
    deadline = time.time() + PHASE_TIMEOUT_S
    best = -1
    while time.time() < deadline:
        im = grab(display, path)
        n = ndiff(im, baseline, box)
        best = max(best, n)
        if n >= threshold:
            return n
        time.sleep(POLL_S)
    return best


def wait_settled(display, path, box):
    """Poll until consecutive grabs are identical in the region."""
    deadline = time.time() + PHASE_TIMEOUT_S
    prev = None
    stable = 0
    tmp = path + ".tmp.png"
    while time.time() < deadline:
        im = grab(display, path if prev is None else tmp)
        if prev is not None and im is not None and ndiff(im, prev, box) == 0:
            stable += 1
            if stable >= SETTLE_FRAMES:
                im.save(path)
                if os.path.exists(tmp):
                    os.remove(tmp)
                return True
        else:
            stable = 0
        prev = im
        time.sleep(POLL_S)
    if prev is not None:
        prev.save(path)
    return False


def follow(path, stop_after):
    """Yields lines appended to path until a stop marker or EOF-timeout."""
    deadline = time.time() + stop_after
    pos = 0
    while time.time() < deadline:
        try:
            with open(path, "rb") as f:
                f.seek(pos)
                chunk = f.read()
                pos = f.tell()
        except OSError:
            chunk = b""
        if chunk:
            for raw in chunk.decode("latin-1").replace("\r", "\n").split("\n"):
                if raw.strip():
                    yield raw.strip()
        else:
            time.sleep(0.1)


def main(display, log, shots, box, total_timeout):
    baseline = None
    area = (box[2] - box[0]) * (box[3] - box[1])
    if area <= 0:
        print("FAIL: cover rectangle %s is empty" % (box,))
        return 1
    print("comparing region %s (%d px)" % (box, area))

    cycle = 0
    results = []
    for line in follow(log, total_timeout):
        if "WMREDRAW: FAIL" in line:
            print("FAIL: probe reported %s" % line)
            return 1
        if "WMREDRAW: settled" in line:
            baseline = grab(display, os.path.join(shots, "baseline.png"))
            if baseline is None:
                print("FAIL: could not capture a baseline screenshot")
                return 1
        elif "WMREDRAW: covered" in line and baseline is not None:
            cycle += 1
            p = os.path.join(shots, "covered_%d.png" % cycle)
            t0 = time.time()
            n = wait_differs(display, p, box, baseline, area // 2)
            # Reported because it IS the marker lag, and a lag creeping up
            # toward PHASE_TIMEOUT_S is the thing that would quietly turn
            # this gate flaky.
            print("  cycle %d: cover visible %.1fs after its marker"
                  % (cycle, time.time() - t0))
            results.append({"cycle": cycle, "covered": n})
        elif "WMREDRAW: restored" in line and results:
            p = os.path.join(shots, "restored_%d.png" % cycle)
            settled = wait_settled(display, p, box)
            im = Image.open(p).convert("RGB") if os.path.exists(p) else None
            results[-1]["settled"] = settled
            results[-1]["restored"] = ndiff(im, baseline, box)
            if im is not None and results[-1]["restored"] > 0:
                ImageChops.difference(baseline.crop(box), im.crop(box)).point(
                    lambda v: min(255, v * 8)).save(
                        os.path.join(shots, "diff_%d.png" % cycle))
        elif "WMREDRAW: done" in line:
            break

    if not results:
        print("FAIL: no complete cover/uncover cycle was captured")
        return 1

    failures = 0
    for r in results:
        n = r["cycle"]
        if r["covered"] < area // 2:
            print("FAIL cycle %d: prober window never covered the region it "
                  "claimed (best %d/%d px changed)" % (n, r["covered"], area))
            failures += 1
        elif r.get("restored") is None:
            print("FAIL cycle %d: never saw the region uncovered again" % n)
            failures += 1
        elif not r["settled"]:
            print("FAIL cycle %d: region never stopped changing after the "
                  "cover was removed" % n)
            failures += 1
        elif r["restored"] == 0:
            print("PASS cycle %d: %d px covered, all %d px restored identical"
                  % (n, r["covered"], area))
        else:
            print("FAIL cycle %d: %d px covered, %d px did NOT come back"
                  % (n, r["covered"], r["restored"]))
            failures += 1

    print("%d/%d cycles passed" % (len(results) - failures, len(results)))
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 9:
        print(__doc__)
        print("usage: afros_redraw_check.py <display> <stderr-log> <shots-dir>"
              " <x> <y> <w> <h> <timeout-s>")
        sys.exit(2)
    disp, logp, shotsd = sys.argv[1:4]
    x, y, w, h = (int(v) for v in sys.argv[4:8])
    sys.exit(main(disp, logp, shotsd, (x, y, x + w, y + h), float(sys.argv[8])))
