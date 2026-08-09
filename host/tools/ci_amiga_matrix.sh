#!/bin/sh
# Real-Kickstart-version coverage on top of ci_amiga.sh's fast single-
# 3.1 gate: builds AMISMOKE once, then boots it under amiberry against
# every ROM in the curated list below, printing a per-version PASS/FAIL
# line and exiting non-zero if any version fails. Mirrors
# ci_atari_matrix.sh's own shape and its founding lesson (single-image
# CI missed two real regressions a multi-version matrix caught) -- see
# docs/amiga-platform.md for this matrix's own scope and findings.
#
# The curated list deliberately stops at Kickstart 2.04, not 1.2/1.3:
# MUI itself requires BOOPSI (Intuition's object-oriented extension),
# which shipped in Kickstart 2.04 -- a Kickstart-1.3 machine cannot run
# MUI at all, full stop, no version of MUI targets it (the M9 plan's
# own "real, honest tension" note). Testing below that floor would not
# be a coverage tradeoff, it would be testing configurations this
# backend's own MUI dependency makes structurally unable to run.
#
# Every ROM below is a real, unmodified Commodore release image (no
# "[b]"/"[h]"/beta/encrypted variants) already present on this machine;
# none carries an A500-labeled release for 2.05 (2.05 shipped with the
# A600), so this deliberately tests a real historical cross-model ROM
# image against this fixture's own --model A500 config -- the same
# kind of any-Kickstart-on-any-model compatibility real reloc-kickstart
# adapters relied on, not assumed safe without having run it.
#
# Kickstart 3.0 is deliberately NOT in this list, the same "excluded
# with a real reason, not silently skipped" treatment
# ci_atari_matrix.sh's own comment gives TOS 1.00/TT/Falcon images.
# Tested here first (both the real (A1200) and (A4000) release ROMs --
# not just one image's own quirk) and found to hit a real 68000
# "Illegal instruction" trap inside the ROM itself partway through
# boot, hanging before ever reaching AMISMOKE, on BOTH images
# identically. Kickstart 3.0 was the first AGA-era ROM, shipped only
# with 68020+ machines (A1200/A4000) -- unlike 3.1, which continued to
# see real-world use on 68000 A500/A600 machines via reloc-kickstart
# upgrades and was evidently kept compatible with that CPU class, 3.0's
# own boot code apparently was not. This is a real, structural
# CPU-generation incompatibility with this fixture's own --model A500
# (68000) floor, not a fixable bug in this project's code or a ROM-
# image-choice mistake -- see docs/amiga-platform.md.
set -e

cd "$(dirname "$0")/../.."
. host/tools/amiga_ci_lib.sh

# label:path -- label is just for the summary output. Paths contain
# spaces (real Aminet/community filename conventions), so this list is
# read line-by-line below via `read`, never word-split.
ROMS="kick-2.04-a500plus:$HOME/Documents/Amiga/Kickstart v2.04 r37.175 (1991)(Commodore)(A500+)[!].rom
kick-2.05-a600hd:$HOME/Documents/Amiga/Kickstart v2.05 r37.300 (1991)(Commodore)(A600HD).rom
kick-3.1-a500:$AMIGA_FIXTURE/kick31.rom"

make amiga-engine-check
make amiga-smoke
make amiga-app

PASS_COUNT=0
FAIL_COUNT=0
FAILED_LABELS=""
OFFSET=0

# A heredoc, NOT a pipe, feeds this loop -- a `... | while read` would
# run the loop body in a subshell, where PASS_COUNT/FAIL_COUNT updates
# below would silently vanish once the pipeline ends (a real, common
# POSIX sh gotcha). A heredoc redirection stays in this shell.
while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    label="${entry%%:*}"
    rom="${entry#*:}"

    if [ ! -f "$rom" ]; then
        echo "SKIP: $label ($rom not found on this machine)"
        continue
    fi

    echo "=== $label ($rom) ==="
    OFFSET=$((OFFSET + 1))
    ENTRY_OK=1
    if run_amiga_smoke_once "$rom" "$OFFSET"; then
        :
    else
        echo "FAIL: $label (AMISMOKE)"
        echo "--- AMISMOKE log ---"
        cat "$AMIGA_CI_LOG"
        ENTRY_OK=0
    fi
    rm -rf "$(dirname "$AMIGA_CI_LOG")"

    # T158: AMISMOKE never opens a MUI window, so on its own it cannot
    # tell a working app from one that dies during its first paint --
    # this matrix passed 3/3 while RETROWP crashed on ~17 of 18 boots.
    # Boot the real app against this same Kickstart.
    #
    # Runs on EVERY Kickstart in the list. It was briefly 3.1-only:
    # the gate's first run found the app never opened a window at all
    # under 2.04/2.05, which turned out to be two real defects it had
    # been blind to (T164 -- asl.library aborting startup before main,
    # and AllocBitMap being V39-only). Both fixed, so the exclusion is
    # gone rather than left to rot.
    OFFSET=$((OFFSET + 1))
    if run_amiga_app_once "$rom" "$OFFSET"; then
        :
    else
        echo "FAIL: $label (RETROWP app gate)"
        echo "--- RETROWP app gate ---"
        cat "$AMIGA_APP_LOG"
        ENTRY_OK=0
    fi
    rm -rf "$(dirname "$AMIGA_APP_LOG")"

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
echo "AMIGA KICKSTART MATRIX: $PASS_COUNT passed, $FAIL_COUNT failed"

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "Failed versions:$FAILED_LABELS"
    exit 1
fi
