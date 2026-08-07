#!/bin/sh
# Multitasking-AES REDRAW gate: boots RETROWP.PRG and WMREDRAW.PRG side by
# side under ARAnyM running AFROS (FreeMiNT + XaAES + fVDI), has the probe
# repeatedly cover and uncover RetroWP's window, and asserts that the
# uncovered region comes back pixel-identical to how it looked before.
#
# This is the one thing the six-image single-TOS matrix structurally
# cannot test: under plain TOS nothing else can ever put a window over
# RetroWP's, so the wind_update(BEG_UPDATE/END_UPDATE) + graf_mouse(M_OFF/
# M_ON) discipline around drawing has no observable consequence there.
# See host/tools/afros_ci_lib.sh's header for the full rationale, and
# host/tools/atari_wmredraw_probe.c for why the cover/uncover cycle is
# driven from inside the emulated machine rather than by host clicks.
#
# Kept separate from ci_afros.sh because it answers a different question
# (does the app repaint correctly when another process uncovers it) and
# costs a screenshot-comparison pipeline that the selfcheck gate has no
# use for.
#
# Set AFROS_FIXTURE to point at an AFROS install (default ~/src/afros).
# Pass MEM_PROT=YES in the environment to additionally exercise MiNT's
# MMU-backed process isolation.
# Set KEEP_SHOTS=1 to keep the captured PNGs for inspection.
set -e

cd "$(dirname "$0")/../.."
. host/tools/afros_ci_lib.sh

if [ ! -d "$AFROS_FIXTURE" ]; then
    echo "AFROS redraw CI: SKIPPED (no AFROS install at $AFROS_FIXTURE)"
    echo "  Get one from https://github.com/ragnar76/afros, or set AFROS_FIXTURE."
    exit 0
fi
for t in Xvfb import xdotool python3; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "AFROS redraw CI: SKIPPED (missing $t)"
        exit 0
    fi
done
if ! python3 -c "import PIL" >/dev/null 2>&1; then
    echo "AFROS redraw CI: SKIPPED (missing Python Pillow, needed to diff screenshots)"
    exit 0
fi

make atari
make atari-wmredraw-probe

MEMPROT="${MEM_PROT:-NO}"
echo "=== AFROS redraw probe (FreeMiNT + XaAES), MEM_PROT=$MEMPROT ==="

SCRATCH="$(mktemp -d)"
SHOTS="$SCRATCH/shots"
mkdir -p "$SHOTS"

# Work on a copy: MiNT writes into drive_c during boot and the fixture is
# the developer's own AFROS install -- same rule afros_ci_lib.sh states.
cp -a "$AFROS_FIXTURE" "$SCRATCH/afros"
cp bin/atari/RETROWP.PRG "$SCRATCH/afros/drive_c/RETROWP.PRG"
cp bin/atari/WMREDRAW.PRG "$SCRATCH/afros/drive_c/WMREDRAW.PRG"

# fvdi.sys is a BRANCHING SCRIPT, not a flat config: it prints a
# resolution menu, then `waitkey`/`casekey N label` jumps to one of
# several `01r aranym.sys mode ...` lines. Several of those lines are
# uncommented at once because they are alternative branches, not a
# priority list -- editing them does nothing. Headless there is no
# keypress, so `setkey N` IS the selection. Stock AFROS ships `setkey 0`
# (640x480x8); 1 selects 1024x768x32, which is the point of running this
# under ARAnyM at all (the Atari draw path has never run above 4
# bitplanes on the single-TOS matrix). The file has CRLF line endings,
# hence the explicit \r in the pattern.
sed -i 's/^setkey 0\r$/setkey 1\r/' "$SCRATCH/afros/drive_c/fvdi.sys"

# Launch both from XaAES's own run list rather than mint.cnf: mint.cnf
# executes during kernel init, BEFORE the AES exists. RetroWP first so it
# is the one already on screen when the prober starts covering it; the
# prober's own SETTLE_MS absorbs any remaining startup skew.
XAAES_CNF="$SCRATCH/afros/drive_c/mint/1-19-cur/xaaes/xaaes.cnf"
printf '\nrun C:\\RETROWP.PRG\nrun C:\\WMREDRAW.PRG\n' >> "$XAAES_CNF"

sed -i "s/MEM_PROT=[A-Za-z]*/MEM_PROT=$MEMPROT/" "$SCRATCH/afros/config"

# Deliberately taller than the 1024x768 emulated screen: ARAnyM places
# its SDL window at an offset of its own choosing (320,272 observed), so
# on a 1280x1024 root the bottom of the Atari screen -- including
# RetroWP's whole status line -- fell off the captured image entirely and
# could never be compared.
DISPLAY_NUM=":$((97 + $$ % 400))"
Xvfb "$DISPLAY_NUM" -screen 0 1600x1400x24 >"$SCRATCH/xvfb.log" 2>&1 &
XVFB_PID=$!

XVFB_LOCK="/tmp/.X${DISPLAY_NUM#:}-lock"
WAITED=0
while [ ! -e "$XVFB_LOCK" ]; do
    WAITED=$((WAITED + 1))
    if [ "$WAITED" -gt 50 ]; then
        echo "AFROS redraw CI: FAILED (Xvfb never came up)"
        kill "$XVFB_PID" 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
done

STDERR_LOG="$SCRATCH/stderr.log"
: > "$STDERR_LOG"

set +e
( cd "$SCRATCH/afros" && \
  env DISPLAY="$DISPLAY_NUM" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    timeout 300 aranym -c config -N ) \
    >"$SCRATCH/stdout.log" 2>"$STDERR_LOG" &
ARANYM_PID=$!
set -e

# ARAnyM opens its SDL window wherever it likes on the bare Xvfb root
# (observed at 320,272, not at the origin and not centred), so
# emulated-screen coordinates have to be translated by that window's real
# position before anything host-side can compare a region. Measuring the
# offset rather than assuming it is the whole point: assuming it is
# exactly how an earlier hand-aimed attempt at this test clicked empty
# desktop and proved nothing.
#
# Resolved LATE, not at startup. fVDI switches the screen mode partway
# through boot and SDL replaces its X window when that happens, so an ID
# captured early is stale by the time it matters -- every later request
# against it fails with BadWindow and the offset silently reads back as
# 0,0. Any candidate whose geometry query fails or whose size is smaller
# than a plausible Atari screen is skipped for the same reason.
aranym_origin() {
    _found=""
    for _w in $(DISPLAY="$DISPLAY_NUM" xdotool search --name ARAnyM 2>/dev/null); do
        _geom="$(DISPLAY="$DISPLAY_NUM" xdotool getwindowgeometry --shell "$_w" 2>/dev/null)" || continue
        [ -n "$_geom" ] || continue
        ( eval "$_geom"
          [ "${WIDTH:-0}" -ge 320 ] && [ "${HEIGHT:-0}" -ge 200 ] || exit 1
          echo "$X $Y $WIDTH $HEIGHT" ) && _found=1 && break
    done
    [ -n "$_found" ]
}

# The probe announces every phase transition over NF_STDERR (the same
# Native Features channel atari_smoke.c uses). Wait for the very first
# one -- it carries the cover rectangle -- before resolving the ARAnyM
# window, since by then fVDI has finished switching the screen mode.
WAITED=0
while ! grep -aq "WMREDRAW: rect " "$STDERR_LOG" 2>/dev/null; do
    WAITED=$((WAITED + 1))
    if [ "$WAITED" -gt 2400 ]; then
        echo "AFROS redraw CI: FAILED (probe never started -- no rect marker)"
        kill "$ARANYM_PID" "$XVFB_PID" 2>/dev/null || true
        exit 1
    fi
    sleep 0.1
done

RECT_RAW="$(grep -a "WMREDRAW: rect " "$STDERR_LOG" | head -1 \
    | sed -n 's/.*WMREDRAW: rect \([0-9]*\) \([0-9]*\) \([0-9]*\) \([0-9]*\).*/\1 \2 \3 \4/p')"
ORIGIN="$(aranym_origin || true)"

STATUS=0
if [ -z "$RECT_RAW" ]; then
    echo "AFROS redraw CI: FAILED (could not parse the probe's cover rectangle)"
    STATUS=1
elif [ -z "$ORIGIN" ]; then
    echo "AFROS redraw CI: FAILED (could not locate the ARAnyM window to"
    echo "  translate emulated coordinates -- comparing at a guessed offset"
    echo "  would silently compare the wrong pixels, so this refuses instead)"
    STATUS=1
else
    set -- $ORIGIN
    ORIGIN_X=$1; ORIGIN_Y=$2
    echo "ARAnyM window at ${ORIGIN_X},${ORIGIN_Y} ($3x$4)"
    set -- $RECT_RAW
    set +e
    python3 host/tools/afros_redraw_check.py "$DISPLAY_NUM" "$STDERR_LOG" "$SHOTS" \
        "$(($1 + ORIGIN_X))" "$(($2 + ORIGIN_Y))" "$3" "$4" 240
    STATUS=$?
    set -e
fi

kill "$ARANYM_PID" 2>/dev/null || true
wait "$ARANYM_PID" 2>/dev/null || true
kill "$XVFB_PID" 2>/dev/null || true
wait "$XVFB_PID" 2>/dev/null || true

echo "--- probe markers ---"
grep -a "WMREDRAW:" "$STDERR_LOG" || echo "(none -- the probe never ran)"

if [ "${KEEP_SHOTS:-0}" = "1" ]; then
    echo "screenshots kept in $SHOTS"
else
    rm -rf "$SCRATCH"
fi

if [ "$STATUS" -ne 0 ]; then
    echo "AFROS redraw CI: FAILED"
    exit 1
fi
echo "AFROS redraw CI: PASSED"
