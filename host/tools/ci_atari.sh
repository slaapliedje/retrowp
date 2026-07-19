#!/bin/sh
# Atari CI entrypoint (M4 T4.1): cross-compile the AES app shell + its
# Hatari smoke test, run the smoke test under Hatari with EmuTOS at a
# genuine 1MB ST config, and treat the emulator process's own exit code
# as pass/fail (via the NF_STDERR/NF_EXIT Native Features trap in
# host/tools/atari_smoke.c). Exits non-zero on any build or test
# failure. See docs/atari-platform.md for how this recipe was derived
# and the specific gotchas it works around (the --auto path must be the
# FULL in-emulator path, not a bare filename; a real X display -- Xvfb
# is fine -- is required even though nothing needs to be seen; the
# monitor/zoom/tos-res flags matter for the video subsystem to init at
# all in a headless container).
set -e

cd "$(dirname "$0")/../.."

TOS_IMAGE=/usr/share/hatari/etos512k.img
SCRATCH="$(mktemp -d)"
XVFB_DISPLAY=":97"
trap 'kill "$XVFB_PID" 2>/dev/null; rm -rf "$SCRATCH"' EXIT

make atari-engine-check
make atari-smoke

Xvfb "$XVFB_DISPLAY" -screen 0 1280x800x24 >"$SCRATCH/xvfb.log" 2>&1 &
XVFB_PID=$!
sleep 1

mkdir -p "$SCRATCH/c_drive"
cp bin/atari/SMOKE.PRG "$SCRATCH/c_drive/SMOKE.PRG"

set +e
HOME="$SCRATCH" DISPLAY="$XVFB_DISPLAY" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    timeout 25 hatari \
    --natfeats on \
    --tos "$TOS_IMAGE" \
    --machine st \
    --memsize 1 \
    --monitor rgb --tos-res low --zoom 4 --statusbar no --borders no \
    --confirm-quit no \
    --sound off \
    --conout 2 \
    --harddrive "$SCRATCH/c_drive" \
    --auto SMOKE.PRG \
    >"$SCRATCH/stdout.log" 2>"$SCRATCH/stderr.log"
STATUS=$?
set -e

echo "--- Hatari conout + NF_STDERR log ---"
cat "$SCRATCH/stdout.log" "$SCRATCH/stderr.log" | grep -E "SMOKE|PASS|FAIL|Malloc" || true
echo "--- Hatari exit status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "ATARI CI: FAILED (exit $STATUS)"
    echo "--- full stdout ---"; cat "$SCRATCH/stdout.log"
    echo "--- full stderr ---"; cat "$SCRATCH/stderr.log"
    exit 1
fi

echo "ATARI CI: PASSED"
