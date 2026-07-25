#!/bin/sh
# Multitasking-AES gate: boots SMOKE.PRG under ARAnyM running AFROS
# (FreeMiNT + XaAES + fVDI) and exits non-zero if any selfcheck fails.
# See host/tools/afros_ci_lib.sh for why this exists alongside the
# six-image single-TOS matrix and what it covers that that one cannot.
#
# Deliberately NOT part of ci_atari_matrix.sh: this is a different
# question (does the app behave under a real multitasking AES), and it
# boots a whole operating system rather than a single .PRG.
#
# Set AFROS_FIXTURE to point at an AFROS install (default ~/src/afros).
# Pass MEM_PROT=YES in the environment to additionally exercise MiNT's
# MMU-backed process isolation.
set -e

cd "$(dirname "$0")/../.."
. host/tools/afros_ci_lib.sh

if [ ! -d "$AFROS_FIXTURE" ]; then
    echo "AFROS CI: SKIPPED (no AFROS install at $AFROS_FIXTURE)"
    echo "  Get one from https://github.com/ragnar76/afros, or set AFROS_FIXTURE."
    exit 0
fi

make atari-engine-check
make atari-smoke

MEMPROT="${MEM_PROT:-NO}"
echo "=== AFROS (FreeMiNT + XaAES), MEM_PROT=$MEMPROT ==="

# NOT `run_afros_smoke_once ...; STATUS=$?` -- under `set -e`, a bare
# statement (not an if/while condition) that returns non-zero kills the
# script before STATUS is ever assigned. Same reasoning ci_atari.sh's
# and ci_mac.sh's own comments give for the identical shape.
if run_afros_smoke_once 0 "$MEMPROT"; then
    STATUS=0
else
    STATUS=$AFROS_CI_STATUS
fi
LOG=$AFROS_CI_LOG

echo "--- SMOKE.PRG log (via NF_STDERR) ---"
grep -E "^(PASS|FAIL|ATARI SMOKE)" "$LOG" || cat "$LOG"
echo "--- status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "AFROS CI: FAILED (exit $STATUS)"
    rm -rf "$(dirname "$LOG")"
    exit 1
fi

rm -rf "$(dirname "$LOG")"
echo "AFROS CI: PASSED"
