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
. host/tools/atari_ci_lib.sh

TOS_IMAGE=/usr/share/hatari/etos512k.img

make atari-engine-check
make atari-smoke

# NOT `run_atari_smoke_once ...; STATUS=$?` -- under `set -e`, a bare
# statement (not an if/while condition) that returns non-zero kills the
# script immediately, before STATUS is ever assigned or anything is
# printed. A real smoke-test FAILURE returning non-zero is an expected,
# handled outcome here, not a script bug -- it must be exempted from
# errexit via an if-condition like this, the same reason the original
# (pre-refactor) version of this script wrapped the raw `hatari` call in
# `set +e` / `set -e` around it.
if run_atari_smoke_once "$TOS_IMAGE" 0; then
    STATUS=0
else
    STATUS=$ATARI_CI_STATUS
fi
LOG=$ATARI_CI_LOG

echo "--- Hatari conout + NF_STDERR log ---"
grep -E "SMOKE|PASS|FAIL|Malloc" "$LOG" || true
echo "--- Hatari exit status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "ATARI CI: FAILED (exit $STATUS)"
    echo "--- full log ---"; cat "$LOG"
    rm -rf "$(dirname "$LOG")"
    exit 1
fi

rm -rf "$(dirname "$LOG")"
echo "ATARI CI: PASSED"
