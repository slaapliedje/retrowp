#!/bin/sh
# DOS CI entrypoint (M14 T14.6's Phase 0 gate): cross-compile the engine
# portability check + Phase 0 smoke test, run it under DOSBox, and treat
# the smoke test's own log-file PASS/FAIL convention as pass/fail (see
# host/tools/dos_ci_lib.sh for why DOSBox needs no bespoke exit-trap
# workaround the way Hatari/amiberry/BasiliskII each did).
set -e

cd "$(dirname "$0")/../.."
. host/tools/dos_ci_lib.sh

make dos-engine-check
make dos-smoke

# NOT `run_dos_smoke_once ...; STATUS=$?` -- under `set -e`, a bare
# statement (not an if/while condition) that returns non-zero kills the
# script immediately, before STATUS is ever assigned -- same reason
# ci_atari.sh's own comment gives for its identical if-wrapped call.
if run_dos_smoke_once 0; then
    STATUS=0
else
    STATUS=$DOS_CI_STATUS
fi
LOG=$DOS_CI_LOG

echo "--- DOS_CI.OUT ---"
cat "$LOG"
echo "--- status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "DOS CI: FAILED"
    rm -rf "$(dirname "$LOG")"
    exit 1
fi

rm -rf "$(dirname "$LOG")"
echo "DOS CI: PASSED"
