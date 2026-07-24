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
