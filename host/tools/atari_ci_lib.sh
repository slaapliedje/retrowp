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
# Video mode and RAM differ per machine family, and getting them wrong is
# not cosmetic: RETROWP puts up a modal "Screen is under 80 columns" alert
# at startup and then WAITS for it, so a machine booted into a narrow mode
# looks exactly like a hung app to every marker this harness polls for.
# That is precisely what the Falcon did the first time it was pointed at
# this harness -- alive, drawing correctly, and blocked on its own dialog.
#
#   ST family  --tos-res med, the ST's own 640x200 80-column mode.
#   TT030      --tos-res ttmed ("med" is an ST-only value).
#
# The FALCON is deliberately absent, and the matrix has no Falcon entry
# yet, because none of the obvious levers work -- recorded here so the
# next attempt does not repeat them:
#   --tos-res   Hatari: "TOS resolution setting doesn't work with Falcon
#               (yet)". No effect.
#   --monitor vga alone   still boots 40 columns; the app then sits on
#               its own "Screen is under 80 columns" alert forever, which
#               every marker here reads as a hang.
#   --vdi       Hatari refuses: "Please use 512k EmuTOS instead of TOS v4
#               for proper extended VDI resolutions support on Falcon."
#
# What DOES work is NEWDESK.INF: TOS 4 takes its boot video mode from the
# "#E" line, whose bytes 3-4 are a videl mode word (a real Falcon here
# has "#E 18 04 00 06 00 1A ...", 0x001A = VGA|COL80|BPS4 = 640x480x16).
# Writing one drove the emulated Falcon to a full 640x480 desktop. The
# catch, and the remaining work: Hatari'"'"'s --auto launches the program
# WITHOUT the desktop ever processing the INF, so the Falcon path needs
# "#Z 01 C:\RETROWP.PRG@" autorun in the INF and no --auto at all -- and
# that #Z form did not fire on the first attempt.
#
# RAM likewise: 1MB is the project's ST baseline (CLAUDE.md rule 5) and is
# what the ST entries must keep testing, but a TT030 will not boot in it.
atari_ci_video_args() {
    case "$1" in
    tt) echo "--monitor vga --tos-res ttmed" ;;
    *)  echo "--monitor rgb --tos-res med" ;;
    esac
}

atari_ci_memsize() {
    case "$1" in
    tt|falcon) echo 4 ;;
    *)         echo 1 ;;
    esac
}

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
        --memsize "$(atari_ci_memsize "$_machine")" \
        --fast-boot yes \
        $(atari_ci_video_args "$_machine") --zoom 2 --statusbar no --borders no \
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

# ---------------------------------------------------------------------
# T169: boot the REAL RETROWP.PRG, not SMOKE.PRG.
#
# SMOKE.PRG links the same engine but never installs a menu bar, never
# opens a window and never runs the paint loop, so an app that is
# completely broken for a user still passes the matrix above. That is
# not hypothetical: the identical gap on the Amiga (T158) was hiding two
# real defects (T164) that its own app gate found on its first run.
#
# TWO INDEPENDENT SIGNALS, neither of which parses a screenshot:
#   1. RETROWP.ALV appears  -> the app installed its menu bar, opened its
#      window, laid out the sample document and reached its event loop.
#   2. its event COUNT advances after real injected keystrokes -> the app
#      is still scheduling and still reading the keyboard N seconds
#      later. This is what catches a crash or a wedge AFTER the first
#      paint, and it is deliberately not a log grep: Hatari does not
#      report a guest CPU fault in any greppable form the way amiberry
#      does, and a hung event loop raises no exception to report anyway.
#   3. its PAINTED-FRAGMENT count is nonzero -> the paint loop actually
#      put document text on the screen. Signals 1 and 2 both pass for an
#      app that comes up, paints its chrome and then shows an empty
#      page; the DOS backend was found doing exactly that while this
#      gate was being built, which is why this third signal exists.
#
# The GEMDOS --harddrive directory is a real host directory, so both
# markers are read directly off the host filesystem with no emulator
# cooperation. TOS uppercases 8.3 names, hence the case-insensitive
# lookup below rather than a hardcoded filename.
#
# Usage: run_atari_app_once <tos-image-path> <display-offset> [machine]
# Sets: ATARI_APP_STATUS (0 = pass), ATARI_APP_LOG (caller owns cleanup)
WP_ATARI_APP_SETTLE_SECS="${WP_ATARI_APP_SETTLE_SECS:-6}"

# The marker's payload is "<events> <painted_fragments>". $2 selects
# which field; empty output means "no usable marker yet".
_atari_ci_field() {
    _acf="$(find "$1" -maxdepth 1 -iname 'RETROWP.ALV' 2>/dev/null | head -1)"
    [ -n "$_acf" ] || { echo ""; return; }
    awk -v n="$2" 'NF >= 2 { print $n; found = 1; exit } END { if (!found) print "" }' "$_acf"
}

_atari_ci_alive_count()   { _atari_ci_field "$1" 1; }
_atari_ci_painted_count() { _atari_ci_field "$1" 2; }

run_atari_app_once() {
    _tos_image="$1"
    _offset="$2"
    _machine="${3:-st}"
    _scratch="$(mktemp -d)"
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    Xvfb "$_xvfb_display" -screen 0 1280x800x24 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!
    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null
            ATARI_APP_STATUS=1
            ATARI_APP_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    mkdir -p "$_scratch/c_drive"
    cp bin/atari/RETROWP.PRG "$_scratch/c_drive/RETROWP.PRG"
    echo "ci" > "$_scratch/c_drive/RETROWP.CI"

    # Backgrounded, not `timeout`-wrapped-and-waited like the smoke run:
    # the real app never exits on its own, so this polls for the markers
    # and kills it itself. Deliberately NOT wrapped in `timeout` -- $!
    # would then be timeout's PID, and the kill -9 below would reap the
    # wrapper while leaving a real Hatari process orphaned on the host
    # once per matrix entry. The poll loops below are already bounded,
    # so the kill is reached on every path anyway.
    HOME="$_scratch" DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        hatari \
        --natfeats on \
        --tos "$_tos_image" \
        --machine "$_machine" \
        --memsize "$(atari_ci_memsize "$_machine")" \
        --fast-boot yes \
        $(atari_ci_video_args "$_machine") --zoom 2 --statusbar no --borders no \
        --confirm-quit no \
        --sound off \
        --conout 2 \
        --harddrive "$_scratch/c_drive" \
        --auto RETROWP.PRG \
        >"$_scratch/stdout.log" 2>"$_scratch/stderr.log" &
    _hatari_pid=$!

    # Same 150s class of budget run_atari_smoke_once documents: a real
    # TOS ROM's POST plus GEM startup, not a tight bound.
    # Polls for the marker's CONTENT, never its mere existence. `find`
    # sees the file the instant fopen(...,"wb") creates it, which is
    # before fwrite/fclose have put the digits in -- an existence poll
    # wins that race often enough to read an empty file and conclude the
    # app never started, which is exactly the false failure this gate is
    # supposed to be immune to. Requiring a digit makes the check
    # self-synchronising with no sleep-and-hope anywhere.
    #
    # Bounded by wall clock rather than an iteration count: each pass
    # spawns find/head/tr, so an iteration is not a fixed unit of time
    # and "1500 iterations" silently meant anywhere from 150s to 250s
    # depending on host load.
    _deadline=$(( $(date +%s) + 150 ))
    while [ -z "$(_atari_ci_alive_count "$_scratch/c_drive")" ]; do
        [ "$(date +%s)" -ge "$_deadline" ] && break
        sleep 0.1
    done

    _count_before="$(_atari_ci_alive_count "$_scratch/c_drive")"
    _alive=0
    _responsive=0
    _rendered=0

    if [ -n "$_count_before" ]; then
        _alive=1

        _wid="$(DISPLAY="$_xvfb_display" xdotool search --name -i 'hatari' 2>/dev/null | head -1)"
        [ -n "$_wid" ] && DISPLAY="$_xvfb_display" xdotool windowfocus "$_wid" 2>/dev/null
        sleep 1

        # Motion keys only -- this gate must not mutate the document, so
        # a later assertion (or a human reading the screenshot) still
        # sees the untouched sample. ~150ms apart: faster synthetic input
        # outruns the app under Hatari and proves nothing (see T148).
        for _k in Right Right Left Escape; do
            DISPLAY="$_xvfb_display" xdotool key --delay 150 "$_k" 2>/dev/null
            sleep 0.4
        done

        sleep "$WP_ATARI_APP_SETTLE_SECS"
        # Read AFTER the settle, not before: on the Atari the first
        # document paint happens in response to the first redraw EVENT,
        # i.e. inside the event loop, so the marker written just before
        # that loop legitimately still reads 0 painted fragments. Only a
        # post-keystroke read means "has this app ever painted text".
        _count_after="$(_atari_ci_alive_count "$_scratch/c_drive")"
        _painted="$(_atari_ci_painted_count "$_scratch/c_drive")"
        [ -n "$_painted" ] && [ "$_painted" -gt 0 ] 2>/dev/null && _rendered=1
        if [ -n "$_count_after" ] && [ "$_count_after" -gt "$_count_before" ] 2>/dev/null; then
            _responsive=1
        fi
    fi

    DISPLAY="$_xvfb_display" import -window root "$_scratch/screen.png" 2>/dev/null || true

    kill -9 "$_hatari_pid" 2>/dev/null; wait "$_hatari_pid" 2>/dev/null
    kill "$_xvfb_pid" 2>/dev/null; wait "$_xvfb_pid" 2>/dev/null

    ATARI_APP_LOG="$_scratch/app.log"
    {
        echo "window-open sentinel: $([ "$_alive" -eq 1 ] && echo present || echo MISSING)"
        echo "event count before keys: ${_count_before:-<none>}"
        echo "event count after keys:  ${_count_after:-<none>}"
        echo "still responsive: $([ "$_responsive" -eq 1 ] && echo yes || echo NO)"
        echo "painted text fragments: ${_painted:-<none>}"
        echo "rendered document: $([ "$_rendered" -eq 1 ] && echo yes || echo 'NO -- app came up but painted an EMPTY page')"
        echo "screenshot: $_scratch/screen.png"
        echo "--- hatari stderr ---"
        cat "$_scratch/stderr.log" 2>/dev/null
    } > "$ATARI_APP_LOG"

    if [ "$_alive" -eq 1 ] && [ "$_responsive" -eq 1 ] && [ "$_rendered" -eq 1 ]; then
        ATARI_APP_STATUS=0
        return 0
    fi
    ATARI_APP_STATUS=1
    return 1
}
