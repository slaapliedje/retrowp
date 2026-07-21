#!/bin/sh
# Mac CI entrypoint (M12 Phase 0): cross-compile the Mac smoke test with
# Retro68/CMake, boot it under BasiliskII against the fixture's own
# PERFORMA.ROM, and treat mac_smoke.out's own host-visible PASS/FAIL
# content as the pass/fail signal (mac_ci_lib.sh's own file comment
# explains why a file, not the emulator's exit code, the way ci_atari.sh's
# NF_STDERR/NF_EXIT trap does it). Exits non-zero on any build or test
# failure. See docs/mac-platform.md for the fixture layout and every
# gotcha this recipe works around.
#
# No mac-engine-check/mac-smoke Makefile target exists yet (Phase 1's
# platform/mac/ is what earns that, per docs/milestones.md) -- Phase 0
# only needs Retro68's own CMake build, invoked directly here.
set -e

cd "$(dirname "$0")/../.."
. host/tools/mac_ci_lib.sh

MAC_RETRO68_TOOLCHAIN="${MAC_RETRO68_TOOLCHAIN:-$HOME/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}"
ROM="$MAC_FIXTURE/rom/PERFORMA.ROM"

cmake -S host/tools/mac -B build/mac -DCMAKE_TOOLCHAIN_FILE="$MAC_RETRO68_TOOLCHAIN" -G "Unix Makefiles" >/dev/null
cmake --build build/mac >/dev/null

mkdir -p bin/mac/.rsrc bin/mac/.finf
cp build/mac/mac_smoke.APPL bin/mac/mac_smoke.APPL
cp build/mac/.rsrc/mac_smoke.APPL bin/mac/.rsrc/mac_smoke.APPL
cp build/mac/.finf/mac_smoke.APPL bin/mac/.finf/mac_smoke.APPL

# NOT `run_mac_smoke_once ...; STATUS=$?` -- under `set -e`, a bare
# statement (not an if/while condition) that returns non-zero kills the
# script immediately, before STATUS is ever assigned -- same reasoning
# ci_atari.sh's/ci_amiga.sh's own comments give for the identical shape.
if run_mac_smoke_once "$ROM" 0 14 4; then
    STATUS=0
else
    STATUS=$MAC_CI_STATUS
fi
LOG=$MAC_CI_LOG

echo "--- mac_smoke log ---"
cat "$LOG"
echo "--- status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "MAC CI: FAILED"
    rm -rf "$(dirname "$LOG")"
    exit 1
fi

rm -rf "$(dirname "$LOG")"
echo "MAC CI: PASSED"
