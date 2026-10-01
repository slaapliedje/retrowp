#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# iigs_engine_check.sh -- compile the portable engine, offload/transport and
# services with ORCA/C (Apple IIGS), under ORCA's full lint.
#
#   tools/iigs_engine_check.sh IIX RETROPLAT FILE.c...
#
# IIX is Golden Gate's `iix` (here, the Wine wrapper gem4xe keeps in
# ref/iix; its sibling iix-clean must sit beside it).  Absent iix is a
# SKIP, not a failure, like every other cross-toolchain gate.
#
# Why flat staging: ORCA/C takes no -I on the command line.  A quoted
# #include searches the CURRENT directory -- not the including file's own
# directory -- and ORCA's Libraries:ORCACDefs: after it, which has its own
# types.h, memory.h, font.h, event.h and window.h.  So every source and
# header is copied into one directory and the compiler is run from inside
# it; run from anywhere else, "#include" cannot find the file it names.
# Flattening would also let two same-named files silently overwrite each
# other, so a collision stops the check.
#
# Why a control first: every result here is "0 errors found", which is
# also what a check that never really compiled would look like.  The
# control fixture must be REPORTED, or nothing below is believed.
set -e

IIX=$1; RETROPLAT=$2; shift 2

if [ ! -x "$IIX" ]; then
    echo "iigs-engine-check: SKIP -- no Golden Gate iix at $IIX (set IIX=)"
    exit 0
fi
IIX=$(cd "$(dirname "$IIX")" && pwd)/$(basename "$IIX")   # absolute: compile() runs from $STAGE
IIX_CLEAN="$(dirname "$IIX")/iix-clean"

rm -rf build/iigs; mkdir -p build/iigs/stage build/iigs/log
OUT=$(cd build/iigs && pwd)
STAGE=$OUT/stage

# Every header the sources can reach, plus the sources themselves.
HDRS="engine/include/*.h engine/src/*.h $RETROPLAT/include/*.h
      offload/transport/*.h services/spell/*.h services/grammar/*.h"
for f in $HDRS "$@"; do
    b=$(basename "$f")
    if [ -e "$STAGE/$b" ]; then
        echo "iigs-engine-check: FAIL -- two files named $b; flat staging would lose one"
        exit 1
    fi
    cp "$f" "$STAGE/$b"
done

# Golden Gate runs under Wine: reach the stage through Z:, Wine's root.
WSTAGE="Z:$(printf '%s' "$STAGE" | tr '/' '\\')"

"$IIX_CLEAN"

# compile NAME: lint-wrap NAME.c, compile, print the result line.
compile() {
    printf '#pragma lint -1\n#include "%s.c"\n' "$1" > "$STAGE/lint_$1.c"
    (cd "$STAGE" && "$IIX" compile -I "$WSTAGE\\lint_$1.c" keep="$WSTAGE\\$1") > "$OUT/log/$1.log" 2>&1 || true
    grep -o '[0-9][0-9]* errors\{0,1\} found' "$OUT/log/$1.log" | tail -1
}

# The control: one lint error (a call to an undeclared function) that
# ORCA must report.  Its absence means the pipeline cannot see errors.
printf 'int iigs_control(void) { return never_declared(3); }\n' > "$STAGE/iigs_control.c"
r=$(compile iigs_control)
case $r in
    "1 error found") echo "  control   ORCA/C reports the lint fixture, so it can speak" ;;
    *) echo "iigs-engine-check: FAIL -- control fixture gave '${r:-no result line}', not 1 error"
       echo "  (a silent pipeline would print 0 errors for everything below)"
       exit 1 ;;
esac
rm -f "$STAGE"/iigs_control* "$STAGE"/lint_iigs_control*

fail=0; n=0
for f in "$@"; do
    b=$(basename "$f" .c); n=$((n + 1))
    r=$(compile "$b")
    if [ "$r" != "0 errors found" ] || [ ! -s "$STAGE/$b.a" ]; then
        echo "  FAIL      $f: ${r:-no result line} (log: build/iigs/log/$b.log)"
        grep -B2 '\^' "$OUT/log/$b.log" | head -20
        fail=1
    fi
done

[ $fail -eq 0 ] || { echo "iigs-engine-check: FAIL"; exit 1; }
echo "iigs-engine-check: $n files compile clean under ORCA/C with #pragma lint -1"
