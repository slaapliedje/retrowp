# Shared by ci_dos.sh (single, fast gate) and ci_dos_matrix.sh (real
# machine-type coverage) -- sourced, not executed directly. Boots
# bin/dos/SMOKE.EXE under DOSBox with a real host directory mounted as
# C: and reports pass/fail via a plain log-file convention (the same
# shape amiga_smoke.out's PASS/FAIL lines use, host/tools/
# amiga_ci_lib.sh), not an emulator exit-code trap.
#
# DOSBox needs no bespoke CI-automation workaround the way Hatari/
# amiberry/BasiliskII each did: `--exit` makes the DOSBox process
# itself terminate cleanly once its `-c` commands finish (a real,
# documented flag, confirmed directly during Phase 0 bring-up -- not a
# Native-Features-style trap or a fixed sleep-then-kill), and directory
# mounting is a LIVE passthrough (not a copied disk image), so
# SMOKE.EXE's own file write lands on the real host filesystem in real
# time with no shared-folder setup of any kind. `--noprimaryconf
# --nolocalconf` guarantee this never loads or mutates the user's own
# personal DOSBox config (~/.config/dosbox/*.conf, ~/.dosbox,
# ~/dosbox.d -- a real, personal setup found on this machine, off-
# limits for CI the same way Mac's/Amiga's own personal emulator setups
# were). A `timeout` wrapper is still a real safety net in case
# SMOKE.EXE itself hangs -- --exit only helps once the DOS side
# actually returns.
#
# Usage: run_dos_smoke_once <display-offset> [machine]
#   <display-offset> must be a distinct small integer per call WITHIN
#   one script run -- same reason atari_ci_lib.sh's own comment gives.
#   [machine] is one of DOSBox's --machine values (cga/ega/hercules/
#   svga_s3/...); defaults to "svga_s3" (DOSBox's own default).
# Sets on return (0 = pass, matching the function's own return code):
#   DOS_CI_STATUS - 0 on a clean PASS-only smoke log, 1 otherwise
#                    (boot/run timeout, missing log, or any FAIL line)
#   DOS_CI_LOG     - path to SMOKE.EXE's own PASS/FAIL log (or a note
#                     explaining why it's missing, on timeout); the
#                     caller owns cleanup (rm -rf "$(dirname
#                     "$DOS_CI_LOG")") once done reading it.
run_dos_smoke_once() {
    _offset="$1"
    _machine="${2:-svga_s3}"
    _scratch="$(mktemp -d)"

    # PID-derived AND offset-derived -- same collision reasoning
    # atari_ci_lib.sh's own comment gives for its Xvfb display number.
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    Xvfb "$_xvfb_display" -screen 0 800x600x16 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!

    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            DOS_CI_STATUS=1
            DOS_CI_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    cp bin/dos/SMOKE.EXE "$_scratch/SMOKE.EXE"
    rm -f "$_scratch/DOS_CI.OUT"

    env DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        timeout 60 dosbox --noprimaryconf --nolocalconf --exit --machine "$_machine" \
        -c "MOUNT C \"$_scratch\"" -c "C:" -c "SMOKE.EXE" \
        >"$_scratch/dosbox.log" 2>&1
    _dosbox_status=$?

    kill "$_xvfb_pid" 2>/dev/null
    wait "$_xvfb_pid" 2>/dev/null

    if [ "$_dosbox_status" -eq 124 ]; then
        DOS_CI_STATUS=1
        DOS_CI_LOG="$_scratch/dosbox.log"
        echo "(SMOKE.EXE/DOSBox hung -- timeout killed it)" >>"$DOS_CI_LOG"
        return 1
    fi

    DOS_CI_LOG="$_scratch/DOS_CI.OUT"
    if [ ! -f "$DOS_CI_LOG" ]; then
        DOS_CI_LOG="$_scratch/dosbox.log"
        echo "(DOS_CI.OUT never appeared, despite DOSBox exiting cleanly)" >>"$DOS_CI_LOG"
        DOS_CI_STATUS=1
        return 1
    fi

    if [ -s "$DOS_CI_LOG" ] && ! grep -q "FAIL" "$DOS_CI_LOG"; then
        DOS_CI_STATUS=0
        return 0
    fi
    DOS_CI_STATUS=1
    return 1
}

# ---------------------------------------------------------------------
# T169: boot the REAL RETROWP.EXE, not SMOKE.EXE.
#
# SMOKE.EXE links the same engine but never enters text mode, never
# paints a screen and never reads a key, so an app that is completely
# broken for a user still passes the matrix. The identical gap on the
# Amiga (T158) was hiding two real defects (T164).
#
# Two independent signals, neither of which parses a screenshot:
#   1. RETROWP.ALV appears with a COUNT in it -> the app entered text
#      mode, laid out the sample document, painted a full screen and
#      reached its key loop.
#   2. that count advances after real injected keystrokes -> it is still
#      running and still reading the keyboard. DOSBox cannot report a
#      wedged guest at all, so this is the only way to see that.
#
# The mounted C: is a live host-directory passthrough (see the file
# comment above), so both markers are read straight off the host with
# no emulator cooperation and no shared-folder setup.
#
# Unlike the smoke run this cannot use `--exit`: the real app never
# returns, so DOSBox is backgrounded and killed here once the signals
# are read.
#
# Usage: run_dos_app_once <display-offset> [machine]
# Sets: DOS_APP_STATUS (0 = pass), DOS_APP_LOG (caller owns cleanup)
WP_DOS_APP_SETTLE_SECS="${WP_DOS_APP_SETTLE_SECS:-4}"

# The marker's payload is "<events> <painted_fragments>". $2 selects
# which field; empty output means "no usable marker yet".
_dos_ci_field() {
    _dacf="$(find "$1" -maxdepth 1 -iname 'RETROWP.ALV' 2>/dev/null | head -1)"
    [ -n "$_dacf" ] || { echo ""; return; }
    awk -v n="$2" 'NF >= 2 { print $n; found = 1; exit } END { if (!found) print "" }' "$_dacf"
}

_dos_ci_alive_count()   { _dos_ci_field "$1" 1; }
_dos_ci_painted_count() { _dos_ci_field "$1" 2; }

run_dos_app_once() {
    _offset="$1"
    _machine="${2:-svga_s3}"
    _scratch="$(mktemp -d)"
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    Xvfb "$_xvfb_display" -screen 0 800x600x16 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!
    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null
            DOS_APP_STATUS=1
            DOS_APP_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    cp bin/dos/RETROWP.EXE "$_scratch/RETROWP.EXE"
    echo "ci" > "$_scratch/RETROWP.CI"

    env DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        dosbox --noprimaryconf --nolocalconf --machine "$_machine" \
        -c "MOUNT C \"$_scratch\"" -c "C:" -c "RETROWP.EXE" \
        >"$_scratch/dosbox.log" 2>&1 &
    _dosbox_pid=$!

    # Polls for the marker's CONTENT, never its mere existence: `find`
    # sees the file the instant fopen(...,"wb") creates it, before
    # fwrite/fclose have put the digits in. An existence poll wins that
    # race often enough to read an empty file and wrongly conclude the
    # app never started -- a real false failure hit while bringing the
    # Atari version of this gate up. Wall-clock bounded, not iteration-
    # bounded: each pass spawns find/head/tr, so an iteration is not a
    # fixed unit of time.
    _deadline=$(( $(date +%s) + 90 ))
    while [ -z "$(_dos_ci_alive_count "$_scratch")" ]; do
        [ "$(date +%s)" -ge "$_deadline" ] && break
        sleep 0.1
    done

    _count_before="$(_dos_ci_alive_count "$_scratch")"
    _alive=0
    _responsive=0
    _rendered=0

    if [ -n "$_count_before" ]; then
        _alive=1

        _wid="$(DISPLAY="$_xvfb_display" xdotool search --name -i 'dosbox' 2>/dev/null | head -1)"
        [ -n "$_wid" ] && DISPLAY="$_xvfb_display" xdotool windowfocus "$_wid" 2>/dev/null
        sleep 1

        # Motion keys only -- this gate must not mutate the document.
        for _k in Right Right Left Escape; do
            if [ -n "$_wid" ]; then
                DISPLAY="$_xvfb_display" xdotool key --window "$_wid" "$_k" 2>/dev/null
            else
                DISPLAY="$_xvfb_display" xdotool key "$_k" 2>/dev/null
            fi
            sleep 0.3
        done

        sleep "$WP_DOS_APP_SETTLE_SECS"
        # Read AFTER the settle, not before: on the Atari the first
        # document paint happens in response to the first redraw EVENT,
        # i.e. inside the event loop, so the marker written just before
        # that loop legitimately still reads 0 painted fragments. Only a
        # post-keystroke read means "has this app ever painted text".
        _count_after="$(_dos_ci_alive_count "$_scratch")"
        _painted="$(_dos_ci_painted_count "$_scratch")"
        [ -n "$_painted" ] && [ "$_painted" -gt 0 ] 2>/dev/null && _rendered=1
        if [ -n "$_count_after" ] && [ "$_count_after" -gt "$_count_before" ] 2>/dev/null; then
            _responsive=1
        fi
    fi

    DISPLAY="$_xvfb_display" import -window root "$_scratch/screen.png" 2>/dev/null || true

    kill -9 "$_dosbox_pid" 2>/dev/null; wait "$_dosbox_pid" 2>/dev/null
    kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null

    DOS_APP_LOG="$_scratch/app.log"
    {
        echo "startup sentinel: $([ "$_alive" -eq 1 ] && echo present || echo MISSING)"
        echo "event count before keys: ${_count_before:-<none>}"
        echo "event count after keys:  ${_count_after:-<none>}"
        echo "still responsive: $([ "$_responsive" -eq 1 ] && echo yes || echo NO)"
        echo "painted text fragments: ${_painted:-<none>}"
        echo "rendered document: $([ "$_rendered" -eq 1 ] && echo yes || echo 'NO -- app came up but painted an EMPTY page')"
        echo "screenshot: $_scratch/screen.png"
        echo "--- dosbox log ---"
        cat "$_scratch/dosbox.log" 2>/dev/null
    } > "$DOS_APP_LOG"

    if [ "$_alive" -eq 1 ] && [ "$_responsive" -eq 1 ] && [ "$_rendered" -eq 1 ]; then
        DOS_APP_STATUS=0
        return 0
    fi
    DOS_APP_STATUS=1
    return 1
}
