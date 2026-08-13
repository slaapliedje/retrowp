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

# ---------------------------------------------------------------------
# T169: boot the REAL RETROWP.APPL, not mac_smoke.APPL.
#
# mac_smoke.APPL links the same engine but never draws a menu bar, never
# opens a window and never runs the paint loop, so an app that is
# completely broken for a user still passes the matrix. The identical
# gap on the Amiga (T158) was hiding two real defects (T164).
#
# THREE INDEPENDENT SIGNALS, none of which parses a screenshot:
#   1. Unix:RETROWP.ALV appears -> the app drew its menu bar, opened its
#      window and reached its WaitNextEvent loop.
#   2. its event COUNT advances after real injected keystrokes -> still
#      scheduling and still reading the keyboard. BasiliskII reports a
#      wedged guest no better than DOSBox does, so this is the only way
#      to see that.
#   3. its PAINTED-FRAGMENT count is nonzero -> the paint loop actually
#      put document text on screen. Signals 1 and 2 both pass for an app
#      that comes up, paints its chrome and shows an empty page -- the
#      DOS backend was found doing exactly that (T171) while this gate
#      was being built.
#
# Reuses the fixture's existing permanent Startup-Items launcher exactly
# as run_mac_smoke_once does: whatever is copied to the ExtFS share as
# CI_TARGET.APPL is auto-launched with zero Finder interaction. The only
# difference is WHICH app is copied there.
#
# MUST copy from the RETROWP_APPL build target, never the bare RETROWP
# one: `cmake --build --target RETROWP` skips Rez, so the .rsrc fork
# (where all the code lives) is left stale and the gate would silently
# boot a previous build -- a real trap that cost four misleading runs
# during T157, see docs/mac-platform.md.
#
# Usage: run_mac_app_once <rom-path> <display-offset> <modelid> <cpu>
# Sets: MAC_APP_STATUS (0 = pass), MAC_APP_LOG (caller owns cleanup)
WP_MAC_APP_SETTLE_SECS="${WP_MAC_APP_SETTLE_SECS:-6}"

# The marker's payload is "<events> <painted_fragments>". $2 selects
# which field; empty output means "no usable marker yet".
_mac_ci_field() {
    _macf="$(find "$1" -maxdepth 1 -iname 'RETROWP.ALV' 2>/dev/null | head -1)"
    [ -n "$_macf" ] || { echo ""; return; }
    awk -v n="$2" 'NF >= 2 { print $n; found = 1; exit } END { if (!found) print "" }' "$_macf"
}

_mac_ci_alive_count()   { _mac_ci_field "$1" 1; }
_mac_ci_painted_count() { _mac_ci_field "$1" 2; }

_mac_ci_cleanup_share() {
    rm -f "$1/RETROWP.ALV" "$1/.finf/RETROWP.ALV" \
          "$1/RETROWP.CI" "$1/.finf/RETROWP.CI" \
          "$1/CI_TARGET.APPL" "$1/.rsrc/CI_TARGET.APPL" "$1/.finf/CI_TARGET.APPL" \
          "$1/launcher_fsspec_fail.txt" "$1/.finf/launcher_fsspec_fail.txt" \
          "$1/launcher_launch_fail.txt" "$1/.finf/launcher_launch_fail.txt"
}

run_mac_app_once() {
    _rom="$1"
    _offset="$2"
    _modelid="$3"
    _cpu="$4"
    _shared="$MAC_FIXTURE/shared"
    _scratch="$(mktemp -d)"
    _boot_img="$_scratch/boot.img"
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    cp "$MAC_FIXTURE/boot_template.img" "$_boot_img"
    _mac_ci_cleanup_share "$_shared"
    cp build/mac/RETROWP.APPL       "$_shared/CI_TARGET.APPL"
    cp build/mac/.rsrc/RETROWP.APPL "$_shared/.rsrc/CI_TARGET.APPL"
    cp build/mac/.finf/RETROWP.APPL "$_shared/.finf/CI_TARGET.APPL"
    echo "ci" > "$_shared/RETROWP.CI"

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
            kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null
            _mac_ci_cleanup_share "$_shared"
            MAC_APP_STATUS=1
            MAC_APP_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    env HOME="$_scratch" DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        "$MAC_BASILISK_BIN" --config "$_scratch/prefs" \
        >"$_scratch/basilisk.log" 2>&1 &
    _basilisk_pid=$!

    # Same "this computer may not have been shut down properly" dialog
    # run_mac_smoke_once documents -- dismissed exactly the way the
    # dialog itself instructs, with Return, never a guessed click.
    sleep 8
    DISPLAY="$_xvfb_display" xdotool key Return 2>/dev/null
    sleep 1
    DISPLAY="$_xvfb_display" xdotool key Return 2>/dev/null

    # Polls the marker's CONTENT, never its mere existence -- see
    # host/tools/atari_ci_lib.sh for the create-before-write race this
    # avoids. Wall-clock bounded, not iteration bounded.
    _deadline=$(( $(date +%s) + 120 ))
    while [ -z "$(_mac_ci_alive_count "$_shared")" ]; do
        if [ -f "$_shared/launcher_fsspec_fail.txt" ] || \
           [ -f "$_shared/launcher_launch_fail.txt" ]; then break; fi
        [ "$(date +%s)" -ge "$_deadline" ] && break
        sleep 0.1
    done

    _count_before="$(_mac_ci_alive_count "$_shared")"
    _alive=0
    _responsive=0
    _rendered=0

    if [ -n "$_count_before" ]; then
        _alive=1

        # The window name is "Basilisk II", with a space -- an
        # xdotool search for "BasiliskII" matches nothing (a real
        # dead-end hit during T157). Without an explicit focus the
        # injected keys reach no window at all under bare Xvfb and the
        # app looks wedged when it is merely deaf.
        _wid="$(DISPLAY="$_xvfb_display" xdotool search --name -i 'basilisk' 2>/dev/null | head -1)"
        [ -n "$_wid" ] && DISPLAY="$_xvfb_display" xdotool windowfocus "$_wid" 2>/dev/null

        # X focus alone is not enough: BasiliskII only starts delivering
        # keys to the guest after a real click inside its window. The
        # click lands on the RetroWP window's TITLE BAR, not its content
        # area -- a content click would move the document caret, and
        # this gate must leave the document exactly as it found it.
        # BasiliskII tracks the ABSOLUTE host pointer (unlike ARAnyM and
        # unlike amiberry -- see T150 and the project memory), so a plain
        # mousemove to a computed coordinate is reliable here. The
        # emulated screen is pinned to 512x384 at the root's top-left by
        # this prefs file, and the window is opened at a fixed Toolbox
        # rect (40,60)-(520,340) by app_shell_run, so the title bar sits
        # at a stable spot -- nothing here depends on where a window
        # manager chose to put anything.
        DISPLAY="$_xvfb_display" xdotool mousemove 280 50 2>/dev/null
        sleep 0.3
        DISPLAY="$_xvfb_display" xdotool click 1 2>/dev/null
        sleep 1

        # Motion keys only -- this gate must not mutate the document.
        for _k in Right Right Left Escape; do
            DISPLAY="$_xvfb_display" xdotool key "$_k" 2>/dev/null
            sleep 0.4
        done
        sleep "$WP_MAC_APP_SETTLE_SECS"
        # Read AFTER the settle: the first document paint happens in
        # response to an update EVENT, i.e. inside the event loop, so a
        # marker written just before that loop legitimately still reads
        # 0 painted fragments.
        _count_after="$(_mac_ci_alive_count "$_shared")"
        _painted="$(_mac_ci_painted_count "$_shared")"
        [ -n "$_count_after" ] && [ "$_count_after" -gt "$_count_before" ] 2>/dev/null && _responsive=1
        [ -n "$_painted" ] && [ "$_painted" -gt 0 ] 2>/dev/null && _rendered=1
    fi

    DISPLAY="$_xvfb_display" import -window root "$_scratch/screen.png" 2>/dev/null || true

    kill -9 "$_basilisk_pid" 2>/dev/null; wait "$_basilisk_pid" 2>/dev/null
    kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null

    MAC_APP_LOG="$_scratch/app.log"
    {
        echo "window-open sentinel: $([ "$_alive" -eq 1 ] && echo present || echo MISSING)"
        if [ -f "$_shared/launcher_fsspec_fail.txt" ]; then
            echo "  (launcher could not resolve Unix:CI_TARGET.APPL)"
        elif [ -f "$_shared/launcher_launch_fail.txt" ]; then
            echo "  (launcher's own LaunchApplication call failed)"
        fi
        echo "event count before keys: ${_count_before:-<none>}"
        echo "event count after keys:  ${_count_after:-<none>}"
        echo "still responsive: $([ "$_responsive" -eq 1 ] && echo yes || echo NO)"
        echo "painted text fragments: ${_painted:-<none>}"
        echo "rendered document: $([ "$_rendered" -eq 1 ] && echo yes || echo 'NO -- app came up but painted an EMPTY page')"
        echo "screenshot: $_scratch/screen.png"
    } > "$MAC_APP_LOG"

    _mac_ci_cleanup_share "$_shared"

    if [ "$_alive" -eq 1 ] && [ "$_responsive" -eq 1 ] && [ "$_rendered" -eq 1 ]; then
        MAC_APP_STATUS=0
        return 0
    fi
    MAC_APP_STATUS=1
    return 1
}
