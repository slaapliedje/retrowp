#!/bin/sh
# Amiga CI entrypoint (M9 Phase 5): cross-compile the Amiga app shell +
# its amiberry smoke test, boot it under amiberry against the shared
# WB3.1+MUI fixture's own Kickstart 3.1 ROM, and treat AMISMOKE's own
# host-visible PASS/FAIL log as the pass/fail signal (amiga_ci_lib.sh's
# own file comment explains why a file, not the emulator's exit code,
# the way ci_atari.sh's NF_STDERR/NF_EXIT trap does it). Exits non-zero
# on any build or test failure. See docs/amiga-platform.md for the
# fixture layout and every gotcha this recipe works around.
set -e

cd "$(dirname "$0")/../.."
. host/tools/amiga_ci_lib.sh

KICKSTART_ROM="$AMIGA_FIXTURE/kick31.rom"

make amiga-engine-check
make amiga-smoke

# NOT `run_amiga_smoke_once ...; STATUS=$?` -- under `set -e`, a bare
# statement (not an if/while condition) that returns non-zero kills the
# script immediately, before STATUS is ever assigned -- same reasoning
# ci_atari.sh's own comment gives for the identical shape.
if run_amiga_smoke_once "$KICKSTART_ROM" 0; then
    STATUS=0
else
    STATUS=$AMIGA_CI_STATUS
fi
LOG=$AMIGA_CI_LOG

echo "--- AMISMOKE log ---"
cat "$LOG"
echo "--- status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "AMIGA CI: FAILED"
    rm -rf "$(dirname "$LOG")"
    exit 1
fi

rm -rf "$(dirname "$LOG")"
echo "AMIGA CI: PASSED"
