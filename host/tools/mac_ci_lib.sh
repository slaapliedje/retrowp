# Shared by ci_mac.sh (single, fast smoke gate). Boots bin/mac/mac_smoke.APPL
# under BasiliskII against the M12 fixture template, with one ROM image
# swapped in per call -- mirrors amiga_ci_lib.sh's role for ci_amiga.sh.
#
# BasiliskII has neither Hatari's NF_STDERR/NF_EXIT trap nor amiberry's
# host-directory-mountable boot volume (see docs/mac-platform.md for the
# full story). This works around both without touching the boot image
# per run: the fixture template's boot volume already has a tiny,
# permanent launcher app injected into System Folder:Startup Items
# (host/tools/mac/launcher.c, placed there once via
# host/tools/mac_hfs/inject_startup_item.py -- see docs/mac-platform.md's
# "one-time fixture setup" section). It auto-launches at every boot with
# zero Finder interaction, and its only job is to LaunchApplication
# whatever this script has just copied onto the ExtFS "Unix:" share as
# CI_TARGET.APPL -- a plain host file copy, never disk-image surgery.
# The launched app's own mac_smoke.out is the host-visible PASS/FAIL
# signal this polls for, the same sentinel-log-file role amiga_smoke.out
# plays for Amiga.
#
# Usage: run_mac_smoke_once <rom-path> <display-offset> <modelid> <cpu>
#   <display-offset> must be a distinct small integer per call WITHIN
#   one script run -- same reasoning atari_ci_lib.sh/amiga_ci_lib.sh give
#   for their own Xvfb display number.
#   <modelid>/<cpu> are BasiliskII prefs values matched to the ROM (14/4
#   for PERFORMA.ROM, 5/3 for maciici.rom -- see docs/mac-platform.md).
# Sets on return (0 = pass, matching the function's own return code):
#   MAC_CI_STATUS - 0 on a clean PASS-only smoke log, 1 otherwise
#                    (boot timeout, launcher failure, or any FAIL line)
#   MAC_CI_LOG     - path to mac_smoke.out's own content (or a note
#                     explaining why it's missing, on timeout/launcher
#                     failure); the caller owns cleanup
#                     (rm -rf "$(dirname "$MAC_CI_LOG")") once done reading.
MAC_FIXTURE="${MAC_FIXTURE:-$HOME/opt/mac/fixture}"
MAC_BASILISK_BIN="${MAC_BASILISK_BIN:-$HOME/macemu-mon/BasiliskII/src/Unix/BasiliskII}"

run_mac_smoke_once() {
    _rom="$1"
    _offset="$2"
    _modelid="$3"
    _cpu="$4"
    _shared="$MAC_FIXTURE/shared"
    _scratch="$(mktemp -d)"
    _boot_img="$_scratch/boot.img"

    # PID-derived AND offset-derived -- same collision reasoning
    # atari_ci_lib.sh's own comment gives for its Xvfb display number.
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    cp "$MAC_FIXTURE/boot_template.img" "$_boot_img"
    rm -f "$_shared/mac_smoke.out" "$_shared/launcher_fsspec_fail.txt" "$_shared/launcher_launch_fail.txt"
    cp bin/mac/mac_smoke.APPL "$_shared/CI_TARGET.APPL"
    cp bin/mac/.rsrc/mac_smoke.APPL "$_shared/.rsrc/CI_TARGET.APPL"
    cp bin/mac/.finf/mac_smoke.APPL "$_shared/.finf/CI_TARGET.APPL"

    cat > "$_scratch/prefs" <<EOF
disk $_boot_img
extfs $_shared
screen win/512/384
rom $_rom
bootdrive 0
bootdriver 0
ramsize 33554432
modelid $_modelid
cpu $_cpu
fpu true
nocdrom true
nosound true
nogui true
jit false
keyboardtype 5
idlewait true
EOF

    Xvfb "$_xvfb_display" -screen 0 1024x768x24 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!

    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            MAC_CI_STATUS=1
            MAC_CI_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    env HOME="$_scratch" DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        "$MAC_BASILISK_BIN" --config "$_scratch/prefs" \
        >"$_scratch/basilisk.log" 2>&1 &
    _basilisk_pid=$!

    # The fixture is always copied fresh and never cleanly shut down, so
    # the pre-Finder "this computer may not have been shut down
    # properly" dialog appears on every single boot. It explicitly
    # instructs "Press the Return key on the keyboard to continue" --
    # use exactly that, rather than clicking a button at a guessed
    # coordinate (an earlier attempt at coordinate-blind clicking missed
    # the button entirely and stalled the whole boot indefinitely).
    sleep 8
    DISPLAY="$_xvfb_display" xdotool key Return 2>/dev/null
    sleep 1
    DISPLAY="$_xvfb_display" xdotool key Return 2>/dev/null

    _done_wait=0
    while [ ! -f "$_shared/mac_smoke.out" ] && \
          [ ! -f "$_shared/launcher_fsspec_fail.txt" ] && \
          [ ! -f "$_shared/launcher_launch_fail.txt" ]; do
        _done_wait=$((_done_wait + 1))
        if [ "$_done_wait" -gt 300 ]; then
            kill -9 "$_basilisk_pid" 2>/dev/null
            wait "$_basilisk_pid" 2>/dev/null
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            MAC_CI_STATUS=1
            MAC_CI_LOG="$_scratch/basilisk.log"
            echo "(no marker file ever appeared -- boot, launcher, or the smoke app itself hung)" >>"$MAC_CI_LOG"
            return 1
        fi
        sleep 0.1
    done

    kill -9 "$_basilisk_pid" 2>/dev/null
    wait "$_basilisk_pid" 2>/dev/null
    kill "$_xvfb_pid" 2>/dev/null
    wait "$_xvfb_pid" 2>/dev/null

    MAC_CI_LOG="$_scratch/smoke.log"
    if [ -f "$_shared/mac_smoke.out" ]; then
        cp "$_shared/mac_smoke.out" "$MAC_CI_LOG"
    elif [ -f "$_shared/launcher_fsspec_fail.txt" ]; then
        echo "FAIL: launcher could not resolve Unix:CI_TARGET.APPL" >"$MAC_CI_LOG"
    else
        echo "FAIL: launcher's own LaunchApplication call failed" >"$MAC_CI_LOG"
    fi

    # BasiliskII's ExtFS auto-creates a .finf/<name> sidecar for every
    # plain file it sees, including these -- clean those up too, or
    # they'd accumulate in shared/.finf/ forever across CI runs.
    rm -f "$_shared/mac_smoke.out" "$_shared/.finf/mac_smoke.out" \
          "$_shared/CI_TARGET.APPL" "$_shared/.rsrc/CI_TARGET.APPL" "$_shared/.finf/CI_TARGET.APPL" \
          "$_shared/launcher_fsspec_fail.txt" "$_shared/.finf/launcher_fsspec_fail.txt" \
          "$_shared/launcher_launch_fail.txt" "$_shared/.finf/launcher_launch_fail.txt"

    if [ -s "$MAC_CI_LOG" ] && ! grep -q "FAIL" "$MAC_CI_LOG"; then
        MAC_CI_STATUS=0
        return 0
    fi
    MAC_CI_STATUS=1
    return 1
}
