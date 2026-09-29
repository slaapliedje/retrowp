#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the release archives, each carrying its licence.
#
# v0.15.0's zips were assembled by hand and went out with no statement of
# terms of any kind -- a binary in somebody's hands telling them nothing
# about what they may do with it.  That is the failure this script
# exists to make impossible: COPYING and NOTICE.txt are not optional
# steps somebody remembers, they are how an archive is built.
#
#   tools/package_release.sh [target ...]     (default: every target)
#
# Targets whose binary is not present are SKIPPED with a line saying so,
# rather than silently producing a short archive -- not every toolchain
# is installed on every machine (docs/atari-platform.md and CLAUDE.md
# both note the Mac one is currently broken here).
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/release"
cd "$ROOT"

test -f COPYING || { echo "no COPYING at the repo root -- refusing to package"; exit 1; }
test -f dist/NOTICE.txt || { echo "no dist/NOTICE.txt -- refusing to package"; exit 1; }

# target:binary[:extra ...]
TARGETS='atari:bin/atari/RETROWP.PRG
amiga:bin/amiga/RETROWP
mac:bin/mac/RETROWP.dsk:bin/mac/RETROWP.macbinary.bin
dos:bin/dos/RETROWP.EXE
gem4xe:bin/gem4xe/RETROWP.G4A'

want="${*:-atari amiga mac dos gem4xe}"
mkdir -p "$OUT"
made=0

for t in $want; do
    line=$(echo "$TARGETS" | grep "^$t:") || { echo "  $t: no such target"; continue; }
    files=$(echo "$line" | cut -d: -f2-)

    missing=""
    for f in $(echo "$files" | tr ':' ' '); do
        [ -f "$f" ] || missing="$missing $f"
    done
    if [ -n "$missing" ]; then
        echo "  $t: SKIPPED, not built --$missing"
        continue
    fi

    stage="$OUT/stage-$t"
    rm -rf "$stage"; mkdir -p "$stage"
    for f in $(echo "$files" | tr ':' ' '); do cp "$f" "$stage/"; done
    cp COPYING "$stage/COPYING"
    cp dist/NOTICE.txt "$stage/NOTICE.txt"
    if [ -f "dist/$t/README.txt" ]; then
        cp "dist/$t/README.txt" "$stage/README.txt"
    else
        echo "  $t: warning, no dist/$t/README.txt"
    fi

    zip="$OUT/retrowp-$t.zip"
    rm -f "$zip"
    (cd "$stage" && zip -q -r "$zip" .)
    rm -rf "$stage"
    echo "  $t: $(basename "$zip") -- $(unzip -l "$zip" | tail -1 | awk '{print $2}') files, $(wc -c < "$zip") bytes"
    made=$((made + 1))
done

echo "$made archive(s) in $OUT"
