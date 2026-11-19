#!/bin/sh
# The worked example's gate (M16, T16.6): the character sheet
# (host/tests/fixtures/CHARSHT.RTF) opened on each machine, the document
# it holds checked, a screenshot of it kept, and on the Atari printed.
#
#   - The check that matters is RetroWP's own: in CI mode each backend's
#     alive marker carries a line saying what the document holds
#     (platform/common/cisummary.h), and it must be the line the host
#     test expects (host/tests/test_charsheet.c) -- every paragraph, both
#     drawings, every table row, the frame.
#   - A screenshot of each is kept in build/charsheet/ (NAME.png), and
#     must not be one flat colour.
#   - The Atari prints it too, in PostScript (platform/common/psdev.h),
#     on PRN: (Hatari's --printer): one page, the sheet's words on it.
#     With Ghostscript about, the page is kept as build/charsheet/
#     st_printer.png beside the screenshot. GDOS printing is checked by
#     hand: SpeedoGDOS and NVDI are not this repository's to carry.
#   - The Mac opens it the Finder's way: in CI mode RetroWP sends itself
#     an "open documents" Apple Event for the file the trigger names
#     (platform/mac/app_shell.c), through the same handler a document
#     dropped on it takes.
#
# Prints "CHARSHEET CI: N passed, M failed"; exits non-zero on a failure.
set -u

cd "$(dirname "$0")/../.."
. host/tools/display_lib.sh
. host/tools/atari_ci_lib.sh
. host/tools/amiga_ci_lib.sh
. host/tools/mac_ci_lib.sh
MAC_RETRO68_TOOLCHAIN="${MAC_RETRO68_TOOLCHAIN:-$HOME/opt/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake}"

EXPECT="${CHARSHEET_EXPECT:-doc paras=87 pictures=0 drawings=2 rows=16 frames=1}"   # overridable, to prove the gate can fail
SHEET="${CHARSHEET_FILE:-host/tests/fixtures/CHARSHT.RTF}"   # another file: its screenshots, for looking at
CFG_EXTRA="${CHARSHEET_CFG:-}"   # config lines added, e.g. 'view=page\r\n': its screenshots, for looking at
OUTDIR=build/charsheet
mkdir -p "$OUTDIR"
PASS=0
FAIL=0
FAILED=""

ok()   { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "FAIL: $1 -- $2"; FAIL=$((FAIL + 1)); FAILED="$FAILED $1"; }

# A screenshot that is not one flat colour (a blank or crashed screen).
not_flat() {
    _sd="$(magick "$1" -colorspace gray -format '%[fx:standard_deviation]' info: 2>/dev/null || echo 0)"
    awk -v s="$_sd" 'BEGIN { exit !(s > 0.05) }'
}

# Waits up to $2 seconds for file $1 to hold the summary line.
wait_summary() {
    _end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$_end" ]; do
        if [ -f "$1" ] && grep -q "^doc paras=" "$1" 2>/dev/null; then return 0; fi
        sleep 0.5
    done
    return 1
}

# The summary line in $1, once it is the expected one or $2 more seconds
# have passed: a backend may write its marker before the document is in
# (the Mac's comes before the Finder's Apple Event is handled).
settled_summary() {
    _end=$(( $(date +%s) + $2 ))
    while :; do
        _line="$(grep '^doc paras=' "$1" 2>/dev/null | tail -1)"
        [ "$_line" = "$EXPECT" ] && break
        [ "$(date +%s)" -ge "$_end" ] && break
        sleep 0.5
    done
    echo "$_line"
}

start_xvfb() {   # $1 display
    Xvfb "$1" -screen 0 1280x800x24 >/dev/null 2>&1 &
    XVFB_PID=$!
    _w=0
    while [ ! -e "/tmp/.X${1#:}-lock" ] && [ $_w -lt 50 ]; do sleep 0.1; _w=$((_w + 1)); done
}

make atari atari-launch amiga-app dos build/generated/help_retrowp.h >/dev/null || { echo "CHARSHEET CI: build failed"; exit 1; }
cmake -S host/tools/mac -B build/mac -DCMAKE_TOOLCHAIN_FILE="$MAC_RETRO68_TOOLCHAIN" -G "Unix Makefiles" >/dev/null &&
    cmake --build build/mac --target RETROWP_APPL >/dev/null || { echo "CHARSHEET CI: Mac build failed"; exit 1; }

# ---- Atari ST (TOS 1.04, mono) -----------------------------------------
run_st() {
    _dir="$(mktemp -d)"
    mkdir -p "$_dir/c"
    cp bin/atari/RETROWP.PRG bin/atari/LAUNCH.PRG "$_dir/c/"
    cp "$SHEET" "$_dir/c/TEST.RTF"
    printf 'printer = postscript\r\n%b' "$CFG_EXTRA" > "$_dir/c/RETROWP.CFG"
    echo ci > "$_dir/c/RETROWP.CI"
    _d="$(wp_free_display $((300 + $$ % 200)))"
    start_xvfb "$_d"
    HOME="$_dir" DISPLAY="$_d" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy hatari \
        --natfeats on --tos /usr/share/hatari/tos104us.img --machine st --memsize 4 \
        --fast-boot yes $(atari_ci_video_args st) --zoom 2 --statusbar no --borders no \
        --confirm-quit no --sound off --harddrive "$_dir/c" --printer "$_dir/printer.out" \
        --auto "C:\\LAUNCH.PRG" >"$_dir/hatari.log" 2>&1 &
    _hp=$!
    if ! wait_summary "$_dir/c/RETROWP.ALV" 150; then
        bad st "it never said what it opened"
    else
        sleep 4
        _got="$(settled_summary "$_dir/c/RETROWP.ALV" 20)"
        DISPLAY="$_d" import -window root "$OUTDIR/st.png" 2>/dev/null
        # Print: Ctrl+P; the gateway's HELLO times out on an empty line;
        # OK on the print dialog; the printout comes when it comes.
        _w="$(DISPLAY="$_d" xdotool search --sync --class hatari | head -1)"
        DISPLAY="$_d" xdotool windowfocus "$_w"; sleep 1
        DISPLAY="$_d" xdotool keydown Control_L; sleep 0.1
        DISPLAY="$_d" xdotool keydown p; sleep 0.09; DISPLAY="$_d" xdotool keyup p
        sleep 0.1; DISPLAY="$_d" xdotool keyup Control_L
        sleep 25
        DISPLAY="$_d" xdotool keydown Return; sleep 0.09; DISPLAY="$_d" xdotool keyup Return
        _prev=0; _same=0
        for _i in $(seq 1 60); do
            sleep 3
            _s=$(wc -c < "$_dir/printer.out" 2>/dev/null || echo 0)
            if [ "$_s" -gt 0 ] && [ "$_s" = "$_prev" ]; then _same=$((_same + 1)); [ $_same -ge 3 ] && break; else _same=0; fi
            _prev=$_s
        done
        cp "$_dir/printer.out" "$OUTDIR/st_printer.ps" 2>/dev/null
        command -v gs >/dev/null 2>&1 && gs -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=pnggray -r100 \
            -sOutputFile="$OUTDIR/st_printer.png" "$_dir/printer.out" >/dev/null 2>&1
        if [ "$_got" != "$EXPECT" ]; then bad st "opened \"$_got\""
        elif ! not_flat "$OUTDIR/st.png"; then bad st "the screen is blank"
        elif ! grep -q "^%%Pages: 1" "$_dir/printer.out" 2>/dev/null ||
             ! grep -q "(BACKGROUND)" "$_dir/printer.out"; then bad st "the printout lacks the sheet"
        else ok st; fi
    fi
    kill -9 $_hp 2>/dev/null; wait $_hp 2>/dev/null
    kill $XVFB_PID 2>/dev/null; wait $XVFB_PID 2>/dev/null
    rm -rf "$_dir"
}

# ---- Amiga (Kickstart 3.1, Workbench 3.1 + MUI) ------------------------
# A copy of the fixture's Workbench, the copy's start-up naming the sheet;
# the fixture itself is never touched (its MUI volume is mounted read-only).
run_amiga() {
    _rom="$AMIGA_FIXTURE/kick31.rom"
    if [ ! -f "$_rom" ]; then echo "SKIP: amiga ($_rom not found)"; return; fi
    _dir="$(mktemp -d)"
    cp -r "$AMIGA_FIXTURE/wb" "$_dir/wb"
    sed -i 's|^  RUN >NIL: SYS:RETROWP$|  RUN >NIL: SYS:RETROWP SYS:TEST.RTF|' "$_dir/wb/S/Startup-Sequence"
    if ! grep -q "SYS:RETROWP SYS:TEST.RTF" "$_dir/wb/S/Startup-Sequence"; then
        bad amiga "could not name the sheet in the copy's Startup-Sequence"
        rm -rf "$_dir"; return
    fi
    sed -e "s|$AMIGA_FIXTURE/wb|$_dir/wb|g" -e "s|rw,DH1:|ro,DH1:|g" "$AMIGA_FIXTURE/mounts.uae" > "$_dir/mounts.uae"
    cp bin/amiga/RETROWP "$_dir/wb/RETROWP"
    [ -n "$CFG_EXTRA" ] && printf '%b' "$CFG_EXTRA" > "$_dir/wb/RetroWP.cfg"
    cp "$SHEET" "$_dir/wb/TEST.RTF"
    echo ci > "$_dir/wb/RETROWP_CI"
    rm -f "$_dir/wb/retrowp_ci_alive"
    _d="$(wp_free_display $((300 + ($$ + 7) % 200)))"
    start_xvfb "$_d"
    env HOME="$_dir" DISPLAY="$_d" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        amiberry --model A500 --config "$_dir/mounts.uae" -r "$_rom" \
        -c 4 -F 8 -s use_gui=no -s nr_floppies=0 -s floppy0type=-1 -s floppy1type=-1 --log \
        >"$_dir/amiberry.log" 2>&1 &
    _ap=$!
    if ! wait_summary "$_dir/wb/retrowp_ci_alive" 240; then
        bad amiga "it never said what it opened"
    else
        sleep 10
        _got="$(settled_summary "$_dir/wb/retrowp_ci_alive" 20)"
        DISPLAY="$_d" import -window root "$OUTDIR/amiga.png" 2>/dev/null
        _ill="$(grep -c 'Illegal instruction' "$_dir/amiberry.log" 2>/dev/null || echo 0)"
        if [ "$_got" != "$EXPECT" ]; then bad amiga "opened \"$_got\""
        elif [ "$_ill" -gt 1 ]; then bad amiga "the machine crashed ($_ill illegal instructions)"
        elif ! not_flat "$OUTDIR/amiga.png"; then bad amiga "the screen is blank"
        else ok amiga; fi
    fi
    kill -9 $_ap 2>/dev/null; wait $_ap 2>/dev/null
    kill $XVFB_PID 2>/dev/null; wait $XVFB_PID 2>/dev/null
    rm -rf "$_dir"
}

# ---- Mac (BasiliskII, Performa ROM, System 7.5.3) ----------------------
# The fixture's Startup Items launcher starts whatever is on the share as
# CI_TARGET.APPL (host/tools/mac_ci_lib.sh); the trigger file names the
# sheet, which RetroWP opens through its own Apple Event handler.
run_mac() {
    _rom="$MAC_FIXTURE/rom/PERFORMA.ROM"
    if [ ! -f "$_rom" ] || [ ! -x "$MAC_BASILISK_BIN" ]; then echo "SKIP: mac (no ROM or BasiliskII here)"; return; fi
    _dir="$(mktemp -d)"
    _sh="$_dir/shared"
    mkdir -p "$_sh/.finf" "$_sh/.rsrc"
    cp "$MAC_FIXTURE/boot_template.img" "$_dir/boot.img"
    cp build/mac/RETROWP.APPL "$_sh/CI_TARGET.APPL"
    cp build/mac/.rsrc/RETROWP.APPL "$_sh/.rsrc/CI_TARGET.APPL"
    cp build/mac/.finf/RETROWP.APPL "$_sh/.finf/CI_TARGET.APPL"
    cp "$SHEET" "$_sh/TEST.RTF"
    [ -n "$CFG_EXTRA" ] && printf '%b' "$CFG_EXTRA" > "$_sh/RetroWP.cfg"
    printf 'open Unix:TEST.RTF\n' > "$_sh/RETROWP.CI"
    printf '%s\n' "disk $_dir/boot.img" "extfs $_sh" "screen win/512/384" "rom $_rom" \
        "bootdrive 0" "bootdriver 0" "ramsize 33554432" "modelid 14" "cpu 4" "fpu true" \
        "nocdrom true" "seriala /dev/retrowp-no-serial" "serialb /dev/retrowp-no-serial" \
        "nosound true" "nogui true" "jit false" "keyboardtype 5" "idlewait true" > "$_dir/prefs"
    _d="$(wp_free_display $((300 + ($$ + 21) % 200)))"
    start_xvfb "$_d"
    env HOME="$_dir" DISPLAY="$_d" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        "$MAC_BASILISK_BIN" --config "$_dir/prefs" >"$_dir/basilisk.log" 2>&1 &
    _bp=$!
    # The "not shut down properly" dialog, dismissed as it asks: Return.
    sleep 8; DISPLAY="$_d" xdotool key Return 2>/dev/null; sleep 1; DISPLAY="$_d" xdotool key Return 2>/dev/null
    if ! wait_summary "$_sh/RETROWP.ALV" 150; then
        bad mac "it never said what it opened"
    else
        sleep 4
        _got="$(settled_summary "$_sh/RETROWP.ALV" 20)"
        DISPLAY="$_d" import -window root "$OUTDIR/mac.png" 2>/dev/null
        if [ "$_got" != "$EXPECT" ]; then bad mac "opened \"$_got\""
        elif ! not_flat "$OUTDIR/mac.png"; then bad mac "the screen is blank"
        else ok mac; fi
    fi
    kill -9 $_bp 2>/dev/null; wait $_bp 2>/dev/null
    kill $XVFB_PID 2>/dev/null; wait $XVFB_PID 2>/dev/null
    rm -rf "$_dir"
}

# ---- DOS (DOSBox, S3 SVGA) ---------------------------------------------
run_dos() {
    _dir="$(mktemp -d)"
    mkdir -p "$_dir/c"
    cp bin/dos/RETROWP.EXE "$_dir/c/"
    cp "$SHEET" "$_dir/c/TEST.RTF"
    echo ci > "$_dir/c/RETROWP.CI"
    _d="$(wp_free_display $((300 + ($$ + 13) % 200)))"
    start_xvfb "$_d"
    HOME="$_dir" DISPLAY="$_d" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        dosbox --noprimaryconf --nolocalconf --machine svga_s3 \
        -c "MOUNT C \"$_dir/c\"" -c "C:" -c "RETROWP.EXE TEST.RTF" >"$_dir/dosbox.log" 2>&1 &
    _dp=$!
    if ! wait_summary "$_dir/c/RETROWP.ALV" 90; then
        bad dos "it never said what it opened"
    else
        sleep 3
        _got="$(settled_summary "$_dir/c/RETROWP.ALV" 20)"
        DISPLAY="$_d" import -window root "$OUTDIR/dos.png" 2>/dev/null
        if [ "$_got" != "$EXPECT" ]; then bad dos "opened \"$_got\""
        elif ! not_flat "$OUTDIR/dos.png"; then bad dos "the screen is blank"
        else ok dos; fi
    fi
    kill -9 $_dp 2>/dev/null; wait $_dp 2>/dev/null
    kill $XVFB_PID 2>/dev/null; wait $XVFB_PID 2>/dev/null
    rm -rf "$_dir"
}

# CHARSHEET_ONLY=st|amiga|dos|mac runs one machine (while working on it).
only() { [ -z "${CHARSHEET_ONLY:-}" ] || [ "$CHARSHEET_ONLY" = "$1" ]; }
only st && run_st
only amiga && run_amiga
only dos && run_dos
only mac && run_mac

echo "=================================="
echo "CHARSHEET CI: $PASS passed, $FAIL failed"
if [ "$FAIL" -ne 0 ]; then
    echo "Failed:$FAILED"
    exit 1
fi
