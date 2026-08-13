#!/bin/sh
# Real two-ROM coverage on top of ci_mac.sh's fast single-ROM
# (PERFORMA.ROM) gate: builds mac_smoke once, then boots it under
# BasiliskII against every ROM in the fixture, printing a per-ROM
# PASS/FAIL line and exiting non-zero if any ROM fails. Mirrors
# ci_atari_matrix.sh's/ci_amiga_matrix.sh's own shape.
#
# Only two ROMs exist in this project's own fixture (docs/mac-platform.md):
# PERFORMA.ROM (Gestalt model 14, 68040-class Performa, System 7.5.3) and
# maciici.rom (Mac IIci, 68030-class, older/simpler machine) -- honestly
# narrower than Atari's 6-image or Amiga's 3-image matrices, since only
# two real ROM dumps exist locally, not a scope choice.
set -e

cd "$(dirname "$0")/../.."
. host/tools/mac_ci_lib.sh

MAC_RETRO68_TOOLCHAIN="${MAC_RETRO68_TOOLCHAIN:-$HOME/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}"

# label:rom:modelid:cpu
ROMS="performa:$MAC_FIXTURE/rom/PERFORMA.ROM:14:4
maciici:$MAC_FIXTURE/rom/maciici.rom:5:3"

make mac-engine-check >/dev/null
make build/generated/help_retrowp.h >/dev/null

cmake -S host/tools/mac -B build/mac -DCMAKE_TOOLCHAIN_FILE="$MAC_RETRO68_TOOLCHAIN" -G "Unix Makefiles" >/dev/null
cmake --build build/mac >/dev/null
# Explicit, even though the default target already covers it: T169's app
# gate boots build/mac/.rsrc/RETROWP.APPL, and the RETROWP_APPL target is
# the one that runs Rez. Building bare RETROWP leaves a stale resource
# fork and the gate would silently boot a previous build -- the real trap
# documented in docs/mac-platform.md.
cmake --build build/mac --target RETROWP_APPL >/dev/null

mkdir -p bin/mac/.rsrc bin/mac/.finf
cp build/mac/mac_smoke.APPL bin/mac/mac_smoke.APPL
cp build/mac/.rsrc/mac_smoke.APPL bin/mac/.rsrc/mac_smoke.APPL
cp build/mac/.finf/mac_smoke.APPL bin/mac/.finf/mac_smoke.APPL

PASS_COUNT=0
FAIL_COUNT=0
FAILED_LABELS=""
OFFSET=0

# A heredoc, NOT a pipe, feeds this loop -- a `... | while read` would
# run the loop body in a subshell, where PASS_COUNT/FAIL_COUNT updates
# below would silently vanish once the pipeline ends (same real POSIX
# sh gotcha ci_atari_matrix.sh's/ci_amiga_matrix.sh's own comments
# give).
while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    label="${entry%%:*}"
    rest="${entry#*:}"
    rom="${rest%%:*}"
    rest="${rest#*:}"
    modelid="${rest%%:*}"
    cpu="${rest#*:}"

    if [ ! -f "$rom" ]; then
        echo "SKIP: $label ($rom not found on this machine)"
        continue
    fi

    echo "=== $label ($rom, modelid=$modelid cpu=$cpu) ==="
    OFFSET=$((OFFSET + 1))
    ENTRY_OK=1
    if run_mac_smoke_once "$rom" "$OFFSET" "$modelid" "$cpu"; then
        cat "$MAC_CI_LOG"
    else
        echo "FAIL: $label (mac_smoke)"
        echo "--- mac_smoke log ---"
        cat "$MAC_CI_LOG"
        ENTRY_OK=0
    fi
    rm -rf "$(dirname "$MAC_CI_LOG")"

    # T169: mac_smoke never draws a menu bar, never opens a window and
    # never runs the paint loop, so on its own it cannot tell a working
    # app from one that is completely broken for a user. Boot the real
    # RETROWP.APPL against this same ROM.
    OFFSET=$((OFFSET + 1))
    if run_mac_app_once "$rom" "$OFFSET" "$modelid" "$cpu"; then
        :
    else
        echo "FAIL: $label (RETROWP app gate)"
        echo "--- RETROWP app gate ---"
        cat "$MAC_APP_LOG"
        ENTRY_OK=0
    fi
    rm -rf "$(dirname "$MAC_APP_LOG")"

    if [ "$ENTRY_OK" -eq 1 ]; then
        echo "PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_LABELS="$FAILED_LABELS $label"
    fi
done <<ROMEOF
$ROMS
ROMEOF

echo "=================================="
echo "MAC ROM MATRIX: $PASS_COUNT passed, $FAIL_COUNT failed"

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "Failed ROMs:$FAILED_LABELS"
    exit 1
fi
