# Shared by ci_atari.sh (single, fast EmuTOS gate) and ci_atari_matrix.sh
# (real-TOS-version coverage) -- sourced, not executed directly. Boots
# bin/atari/SMOKE.PRG under Hatari against one TOS image and reports
# pass/fail via the emulator process's own exit code (the NF_STDERR/
# NF_EXIT Native Features trap in host/tools/atari_smoke.c). Assumes
# SMOKE.PRG is already built (callers run `make atari-smoke` once --
# the binary is identical across every TOS image, no reason to rebuild
# per image).
#
# Usage: run_atari_smoke_once <tos-image-path> <display-offset> [machine]
#   <display-offset> must be a distinct small integer per call WITHIN
#   one script run (e.g. a loop index) -- see the display-number
#   comment below for why $$ alone isn't enough inside a loop.
#   [machine] is one of Hatari's --machine values (st/ste/megaste/...);
#   defaults to "st". Real STE/MegaSTE-only ROMs refuse to run (or, in
#   this environment, hang) under the wrong machine type -- see
#   docs/atari-platform.md's TOS matrix section for which image needs
#   which value (Hatari itself will print an ERROR line naming the
#   right family when given the wrong one, which is how this was
#   determined empirically rather than guessed).
# Sets on return (0 = pass, matching the function's own return code):
#   ATARI_CI_STATUS - the Hatari process's exit status
#   ATARI_CI_LOG     - path to a combined stdout+stderr log; the caller
#                       owns cleanup (rm -rf "$(dirname "$ATARI_CI_LOG")")
#                       once done reading it.
run_atari_smoke_once() {
    _tos_image="$1"
    _offset="$2"
    _machine="${3:-st}"
    _scratch="$(mktemp -d)"

    # PID-derived AND offset-derived. PID alone (ci_atari.sh's original
    # approach) stops two SEPARATE script invocations from colliding on
    # a fixed display, but every call inside the SAME script's loop
    # shares one PID -- without the offset, every image in the matrix
    # would race on the identical display number, reintroducing the
    # exact "new Xvfb silently fails to bind, stale lock file from the
    # dying old one fools the readiness poll" bug ci_atari.sh's own
    # history already found once (see its comment).
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    Xvfb "$_xvfb_display" -screen 0 1280x800x24 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!

    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            ATARI_CI_STATUS=1
            ATARI_CI_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    mkdir -p "$_scratch/c_drive"
    cp bin/atari/SMOKE.PRG "$_scratch/c_drive/SMOKE.PRG"

    # 25s (this harness's original budget, tuned against EmuTOS's
    # near-instant boot) is not enough for a real TOS ROM's slower
    # POST/self-test sequence -- confirmed empirically: TOS 1.04 doesn't
    # even reach SMOKE.PRG's first NF_STDERR line until ~40-50s in, and
    # the MegaSTE ROM (tos206us) needs over 90s to reach the LAST check
    # (more hardware to initialize at boot). 150s covers every real
    # image in the curated matrix with real margin, at the cost of a
    # slower failure signal on a genuine hang (rare, and correctness
    # beats a few extra seconds of worst-case CI time).
    # --fast-boot patches TOS/memvalid system variables to skip the
    # real memory-test/POST sequence real TOS ROMs run on boot (TOS
    # 2.06+ in particular visibly runs a full "Memory Test: ST RAM"
    # scan before ever reaching GEMDOS) -- most of the slow-real-TOS-
    # boot time the 150s budget above exists for, so this is real
    # margin, not just a bigger number.
    set +e
    HOME="$_scratch" DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        timeout 150 hatari \
        --natfeats on \
        --tos "$_tos_image" \
        --machine "$_machine" \
        --memsize 1 \
        --fast-boot yes \
        --monitor rgb --tos-res med --zoom 2 --statusbar no --borders no \
        --confirm-quit no \
        --sound off \
        --conout 2 \
        --harddrive "$_scratch/c_drive" \
        --auto SMOKE.PRG \
        >"$_scratch/stdout.log" 2>"$_scratch/stderr.log"
    ATARI_CI_STATUS=$?
    set -e

    kill "$_xvfb_pid" 2>/dev/null
    wait "$_xvfb_pid" 2>/dev/null

    ATARI_CI_LOG="$_scratch/combined.log"
    cat "$_scratch/stdout.log" "$_scratch/stderr.log" > "$ATARI_CI_LOG" 2>/dev/null

    return "$ATARI_CI_STATUS"
}
