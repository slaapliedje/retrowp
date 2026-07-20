#!/bin/sh
# Real-TOS-version coverage on top of ci_atari.sh's fast single-EmuTOS
# gate: builds SMOKE.PRG once, then boots it under Hatari against every
# image in the curated list below, printing a per-image PASS/FAIL line
# and exiting non-zero if any image fails. See docs/atari-platform.md
# for the matrix's scope and why each excluded image is excluded.
#
# The curated list deliberately does NOT include every .img under
# /usr/share/hatari/:
#   - TOS 1.00 (tos100us.img) does not support GEMDOS hard-drive
#     emulation at all, which this whole harness's approach
#     (--harddrive + auto-launching SMOKE.PRG from C:) depends on --
#     there is no working recipe for booting it here, not a coverage
#     tradeoff.
#   - TT (tos306us.img) and Falcon (tos404.img, TOSv4.92.img) ROMs are
#     installed on this machine but are for hardware CLAUDE.md doesn't
#     list as a project target (ST/Amiga/Mac/DOS only) -- skipped, not
#     silently ignored.
set -e

cd "$(dirname "$0")/../.."
. host/tools/atari_ci_lib.sh

# label:path:machine -- label is just for the summary output; machine is
# one of Hatari's --machine values, empirically required (TOS 1.62 is
# STE-only, TOS 2.06 is the MegaSTE ROM -- Hatari's own ERROR output
# named the right family when run under plain "st", see
# docs/atari-platform.md).
IMAGES="
emutos-512k:/usr/share/hatari/etos512k.img:st
emutos-256us:/usr/share/hatari/etos256us.img:st
emutos-512us:/usr/share/hatari/etos512us.img:st
tos-1.04:/usr/share/hatari/tos104us.img:st
tos-1.62-ste:/usr/share/hatari/tos162us.img:ste
tos-2.06-megaste:/usr/share/hatari/tos206us.img:megaste
"

make atari-engine-check
make atari-smoke

PASS_COUNT=0
FAIL_COUNT=0
FAILED_LABELS=""
OFFSET=0

for entry in $IMAGES; do
    [ -z "$entry" ] && continue
    label="${entry%%:*}"
    rest="${entry#*:}"
    image="${rest%%:*}"
    machine="${rest#*:}"

    if [ ! -f "$image" ]; then
        echo "SKIP: $label ($image not found on this machine)"
        continue
    fi

    echo "=== $label ($image, --machine $machine) ==="
    OFFSET=$((OFFSET + 1))
    if run_atari_smoke_once "$image" "$OFFSET" "$machine"; then
        echo "PASS: $label"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "FAIL: $label (Hatari exit $ATARI_CI_STATUS)"
        echo "--- full log ---"
        cat "$ATARI_CI_LOG"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        FAILED_LABELS="$FAILED_LABELS $label"
    fi
    rm -rf "$(dirname "$ATARI_CI_LOG")"
done

echo "=================================="
echo "ATARI TOS MATRIX: $PASS_COUNT passed, $FAIL_COUNT failed"

if [ "$FAIL_COUNT" -ne 0 ]; then
    echo "Failed images:$FAILED_LABELS"
    exit 1
fi
