#!/bin/sh
# Real machine-type coverage on top of ci_dos.sh's fast single-profile
# (svga_s3) gate: builds SMOKE.EXE once, then boots it under DOSBox
# against every --machine profile in the list below, printing a
# per-profile PASS/FAIL line and exiting non-zero if any profile fails.
# Mirrors ci_atari_matrix.sh's/ci_amiga_matrix.sh's/ci_mac_matrix.sh's
# own shape and founding lesson (single-image CI missed real
# regressions a multi-image matrix caught) -- see docs/dos-platform.md
# for this matrix's own scope and findings.
#
# Hercules is deliberately NOT in this list, the same "excluded with a
# real reason, not silently skipped" treatment ci_atari_matrix.sh gives
# TOS 1.00/TT/Falcon images and ci_amiga_matrix.sh gives Kickstart 3.0.
# Confirmed directly (not assumed) during this matrix's own bring-up:
# booting RETROWP.EXE under `--machine hercules` produces a solid black
# screen, with DOSBox's own log reporting "Hercules 640x350 monochrome
# text mode 07h" -- NOT the 80x25 16-colour mode 03h this backend
# requests. Real Hercules hardware never supported color text mode 03h
# at all; a real Hercules card only has mode 7 (MDA-compatible mono
# text) and its own bundled graphics mode, and mode 7's own video
# memory lives at segment B000, not the B800 segment draw_dos.c
# hardcodes. This is a genuine, structural CGA/EGA/VGA-vs-MDA/Hercules
# incompatibility -- docs/dos-platform.md's own Architecture-fit section
# already named MDA/Hercules mono-at-B000 as a real, documented, not-
# required-for-v1 alternate, not a surprise found here. cga/ega/svga_s3
# below are the three real machine profiles that share mode 03h's
# color/B800 model this backend actually implements.
set -e

cd "$(dirname "$0")/../.."
. host/tools/dos_ci_lib.sh

# label:machine -- label is just for the summary output; machine is one
# of DOSBox's own --machine values (`dosbox --help`).
MACHINES="cga:cga
ega:ega
svga_s3:svga_s3"

make dos-engine-check
make dos-smoke
make dos

PASS_COUNT=0
FAIL_COUNT=0
FAILED_LABELS=""
OFFSET=0

# A heredoc, NOT a pipe, feeds this loop -- a `... | while read` would
# run the loop body in a subshell, where PASS_COUNT/FAIL_COUNT updates
# below would silently vanish once the pipeline ends (same real POSIX
# sh gotcha ci_atari_matrix.sh's/ci_amiga_matrix.sh's/ci_mac_matrix.sh's
# own comments give).
while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    label="${entry%%:*}"
    machine="${entry#*:}"

    echo "=== $label (--machine $machine) ==="
    OFFSET=$((OFFSET + 1))
    ENTRY_OK=1
    if run_dos_smoke_once "$OFFSET" "$machine"; then
        cat "$DOS_CI_LOG"
    else
        echo "FAIL: $label (SMOKE.EXE)"
        echo "--- DOS_CI.OUT ---"
        cat "$DOS_CI_LOG"
        ENTRY_OK=0
    fi
    rm -rf "$(dirname "$DOS_CI_LOG")"

    # T169: SMOKE.EXE never enters text mode, never paints a screen and
    # never reads a key, so on its own it cannot tell a working app from
    # one that is completely broken for a user. Boot the real
    # RETROWP.EXE against this same machine profile.
    OFFSET=$((OFFSET + 1))
    if run_dos_app_once "$OFFSET" "$machine"; then
        :
    else
        echo "FAIL: $label (RETROWP app gate)"
        echo "--- RETROWP app gate ---"
        cat "$DOS_APP_LOG"
        ENTRY_OK=0
    fi
    rm -rf "$(dirname "$DOS_APP_LOG")"

    if [ "$ENTRY_OK" -eq 1 ]; then
        echo "PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_LABELS="$FAILED_LABELS $label"
    fi
done <<MACHINEEOF
$MACHINES
MACHINEEOF

echo "=================================="
echo "DOS MACHINE MATRIX: $PASS_COUNT passed, $FAIL_COUNT failed"

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "Failed profiles:$FAILED_LABELS"
    exit 1
fi
