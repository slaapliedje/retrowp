# Shared by ci_amiga.sh (single, fast Kickstart 3.1 gate) and
# ci_amiga_matrix.sh (real Kickstart-version coverage) -- sourced, not
# executed directly. Boots bin/amiga/AMISMOKE under amiberry against
# the shared WB3.1+MUI fixture (docs/amiga-platform.md), with one
# Kickstart ROM swapped in per call.
#
# amiberry has no equivalent to Hatari's NF_STDERR/NF_EXIT Native
# Features trap (there is no way to make the emulator PROCESS exit with
# a pass/fail status), so this uses a real, host-visible file
# convention instead: the fixture's own S/Startup-Sequence has a small,
# conditional CI hook (only active when SYS:AMISMOKE exists on the
# volume at boot time -- a normal interactive session never has this
# file present, so it's a no-op then) that runs AMISMOKE automatically
# and, once it returns, writes a SYS:amiga_ci_done sentinel file. Since
# DH0: is a real host directory (not a disk image -- see mounts.uae),
# both that sentinel and AMISMOKE's own SYS:amiga_smoke.out PASS/FAIL
# log are just ordinary files this script can poll and read directly,
# no in-emulator interaction needed at all. This satisfies the M9
# plan's own Phase 5 requirement to "establish an equivalent headless
# pass/fail signal (a debug-log file convention with a sentinel line)"
# before the CI phase can be considered done.
#
# Usage: run_amiga_smoke_once <kickstart-rom-path> <display-offset>
#   <display-offset> must be a distinct small integer per call WITHIN
#   one script run (e.g. a loop index) -- same reason
#   atari_ci_lib.sh's run_atari_smoke_once needs one.
# Sets on return (0 = pass, matching the function's own return code):
#   AMIGA_CI_STATUS - 0 on a clean PASS-only smoke log, 1 otherwise
#                      (boot timeout, missing log, or any FAIL line)
#   AMIGA_CI_LOG     - path to AMISMOKE's own PASS/FAIL log (or a note
#                       explaining why it's missing, on timeout); the
#                       caller owns cleanup (rm -rf "$(dirname
#                       "$AMIGA_CI_LOG")") once done reading it.
AMIGA_FIXTURE="${AMIGA_FIXTURE:-$HOME/opt/amiga/fixture}"

run_amiga_smoke_once() {
    _kick_rom="$1"
    _offset="$2"
    _wb="$AMIGA_FIXTURE/wb"
    _scratch="$(mktemp -d)"

    # PID-derived AND offset-derived -- same collision reasoning
    # atari_ci_lib.sh's own comment gives for its Xvfb display number.
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    rm -f "$_wb/amiga_smoke.out" "$_wb/amiga_ci_done"
    find "$_wb" -maxdepth 1 -name "trace*" -delete 2>/dev/null
    cp bin/amiga/AMISMOKE "$_wb/AMISMOKE"

    Xvfb "$_xvfb_display" -screen 0 1024x768x24 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!

    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            rm -f "$_wb/AMISMOKE"
            AMIGA_CI_STATUS=1
            AMIGA_CI_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    env HOME="$_scratch" DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        amiberry --model A500 --config "$AMIGA_FIXTURE/mounts.uae" -r "$_kick_rom" \
        -c 4 -F 8 -s use_gui=no -s nr_floppies=0 -s floppy0type=-1 -s floppy1type=-1 --log \
        >"$_scratch/amiberry.log" 2>&1 &
    _amiberry_pid=$!

    # Real amiberry boot + AMISMOKE (a real serial.device SDCMD_QUERY/
    # SETPARAMS round trip, two ~500ms read-timeout cycles, and
    # offload_init's real 2s+2s connect/handshake budget against a dead
    # endpoint -- see docs/amiga-platform.md's Phase 4 notes) together
    # take on the order of 15-25s under this project's own cycle-exact
    # config; 90s is generous real margin, not a tight bound.
    _done_wait=0
    while [ ! -f "$_wb/amiga_ci_done" ]; do
        _done_wait=$((_done_wait + 1))
        if [ "$_done_wait" -gt 900 ]; then
            kill -9 "$_amiberry_pid" 2>/dev/null
            wait "$_amiberry_pid" 2>/dev/null
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            rm -f "$_wb/AMISMOKE" "$_wb/amiga_smoke.out"
            find "$_wb" -maxdepth 1 -name "trace*" -delete 2>/dev/null
            AMIGA_CI_STATUS=1
            AMIGA_CI_LOG="$_scratch/amiberry.log"
            echo "(amiga_ci_done sentinel never appeared -- boot or AMISMOKE itself hung)" >>"$AMIGA_CI_LOG"
            return 1
        fi
        # amiberry has no exit signal to wait() on -- this loop IS the
        # synchronization primitive, same role atari_ci_lib.sh's own
        # Xvfb-lock poll plays for a different real absence (there,
        # Xvfb's own readiness; here, the guest's own completion).
        sleep 0.1
    done

    kill -9 "$_amiberry_pid" 2>/dev/null
    wait "$_amiberry_pid" 2>/dev/null
    kill "$_xvfb_pid" 2>/dev/null
    wait "$_xvfb_pid" 2>/dev/null

    AMIGA_CI_LOG="$_scratch/smoke.log"
    if [ -f "$_wb/amiga_smoke.out" ]; then
        cp "$_wb/amiga_smoke.out" "$AMIGA_CI_LOG"
    else
        echo "(SYS:amiga_smoke.out never appeared, despite the done sentinel)" >"$AMIGA_CI_LOG"
    fi

    rm -f "$_wb/AMISMOKE" "$_wb/amiga_smoke.out" "$_wb/amiga_ci_done"
    find "$_wb" -maxdepth 1 -name "trace*" -delete 2>/dev/null

    if [ -s "$AMIGA_CI_LOG" ] && ! grep -q "FAIL" "$AMIGA_CI_LOG"; then
        AMIGA_CI_STATUS=0
        return 0
    fi
    AMIGA_CI_STATUS=1
    return 1
}

# Boots the REAL interactive app (bin/amiga/RETROWP) under amiberry and
# checks that it actually comes up and stays up. This exists because
# run_amiga_smoke_once above does NOT prove that: AMISMOKE is a headless
# selfcheck binary that never opens a MUI Application/Window, so the
# matrix passed 3/3 across three Kickstarts while the real app crashed
# on ~17 of 18 boots with zero user input (T151). See
# docs/amiga-platform.md.
#
# Two independent signals, because either one alone is fooled:
#
#   1. SYS:retrowp_ci_alive -- written by the app itself once its window
#      is confirmed open, i.e. after MUI has run its Setup/AskMinMax/
#      Draw methods. Gated in the app on the SYS:RETROWP_CI trigger file
#      copied in below, mirroring the fixture's own "IF EXISTS" hook
#      convention. Absent => startup never completed.
#
#   2. The emulator's own illegal-instruction count. amiberry logs
#      exactly ONE per boot on a completely healthy run --
#      "Illegal instruction: 4e7b at 00F80C38", which is Kickstart
#      probing for a 68020 (4e7b is MOVEC) and catching the expected
#      exception. A SECOND line, garbage opcode at a RAM address, is a
#      real guest crash. So >= 2 fails. This catches a crash that
#      happens AFTER the window opened, which signal 1 cannot.
#
# The app deliberately keeps running after writing its sentinel rather
# than quitting, so the extra settle time below is real crash exposure.
#
# Usage: run_amiga_app_once <kickstart-rom-path> <display-offset>
# Sets: AMIGA_APP_STATUS (0 = pass), AMIGA_APP_LOG (caller owns cleanup)
WP_AMIGA_APP_SETTLE_SECS="${WP_AMIGA_APP_SETTLE_SECS:-8}"

run_amiga_app_once() {
    _kick_rom="$1"
    _offset="$2"
    _wb="$AMIGA_FIXTURE/wb"
    _scratch="$(mktemp -d)"
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    rm -f "$_wb/retrowp_ci_alive"
    find "$_wb" -maxdepth 1 -name "trace*" -delete 2>/dev/null
    cp bin/amiga/RETROWP "$_wb/RETROWP"
    echo "ci" > "$_wb/RETROWP_CI"

    Xvfb "$_xvfb_display" -screen 0 1024x768x24 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!
    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null
            rm -f "$_wb/RETROWP" "$_wb/RETROWP_CI"
            AMIGA_APP_STATUS=1
            AMIGA_APP_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    env HOME="$_scratch" DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        amiberry --model A500 --config "$AMIGA_FIXTURE/mounts.uae" -r "$_kick_rom" \
        -c 4 -F 8 -s use_gui=no -s nr_floppies=0 -s floppy0type=-1 -s floppy1type=-1 --log \
        >"$_scratch/amiberry.log" 2>&1 &
    _amiberry_pid=$!

    # Same 90s budget run_amiga_smoke_once uses, for the same reason: a
    # real Workbench boot plus MUI startup, not a tight bound.
    _alive_wait=0
    while [ ! -f "$_wb/retrowp_ci_alive" ]; do
        _alive_wait=$((_alive_wait + 1))
        if [ "$_alive_wait" -gt 900 ]; then break; fi
        sleep 0.1
    done

    if [ -f "$_wb/retrowp_ci_alive" ]; then
        _alive=1
        # Let it run on a while: a crash after the window opened is
        # exactly the class of bug signal 1 cannot see.
        sleep "$WP_AMIGA_APP_SETTLE_SECS"
    else
        _alive=0
    fi

    DISPLAY="$_xvfb_display" import -window root "$_scratch/screen.png" 2>/dev/null || true

    kill -9 "$_amiberry_pid" 2>/dev/null; wait "$_amiberry_pid" 2>/dev/null
    kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null

    _illegal="$(grep -c 'Illegal instruction' "$_scratch/amiberry.log" 2>/dev/null || echo 0)"

    AMIGA_APP_LOG="$_scratch/app.log"
    {
        echo "window-open sentinel: $([ "$_alive" -eq 1 ] && echo present || echo MISSING)"
        echo "illegal-instruction lines: $_illegal (1 = Kickstart's own 68020 probe; >=2 = real crash)"
        grep 'Illegal instruction' "$_scratch/amiberry.log" 2>/dev/null | sed 's/^/  /'
        echo "screenshot: $_scratch/screen.png"
    } > "$AMIGA_APP_LOG"

    rm -f "$_wb/RETROWP" "$_wb/RETROWP_CI" "$_wb/retrowp_ci_alive"
    find "$_wb" -maxdepth 1 -name "trace*" -delete 2>/dev/null

    if [ "$_alive" -eq 1 ] && [ "$_illegal" -le 1 ]; then
        AMIGA_APP_STATUS=0
        return 0
    fi
    AMIGA_APP_STATUS=1
    return 1
}
