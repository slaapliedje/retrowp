# Shared by ci_afros.sh -- sourced, not executed directly. Boots
# bin/atari/SMOKE.PRG under ARAnyM running AFROS (FreeMiNT + XaAES) and
# reports pass/fail via the emulator process's own exit code, exactly
# the way atari_ci_lib.sh does under Hatari.
#
# WHY A SECOND ATARI HARNESS AT ALL
# ---------------------------------
# ci_atari_matrix.sh covers six real TOS/EmuTOS versions, but every one
# of them is SINGLE-TASKING. Three classes of defect cannot be expressed
# there at all:
#
#   - AES discipline. Omitting wind_update(BEG_UPDATE/END_UPDATE) or
#     graf_mouse(M_OFF/M_ON) around drawing is merely COSMETIC under
#     plain TOS (it corrupts the AES cursor save-under -- "mouse
#     droppings", which is exactly how that bug was found here). Under
#     XaAES the same omission is a correctness bug: another process can
#     draw into the window mid-paint, and unbalanced BEG/END can wedge
#     the screen semaphore. The fix is in the tree; this is what proves
#     it.
#   - Real colour depth and resolution. AFROS ships fVDI configured for
#     ARAnyM, so 8/16-bit and large screens are reachable. The Atari
#     colour path is a nearest-match into a small fixed palette and has
#     never run above 4 bitplanes.
#   - Memory protection. MEM_PROT in the AFROS config's BootstrapArgs
#     turns on MiNT's MMU-backed process isolation, which catches
#     handing private memory to another process (an OBJECT tree to
#     XaAES, buffers via scrap/AV). Plain TOS has no such concept.
#
# WHY AFROS RATHER THAN A HAND-BUILT MiNT
# ---------------------------------------
# AFROS (https://github.com/ragnar76/afros) boots MiNT through ARAnyM's
# own NF bootstrap call (`Bootstrap = system/mintara.prg`), which
# side-steps a real chicken-and-egg: ARAnyM's HostFS is provided by
# MiNT's hostfs.xfs, but MiNT itself lives on the HostFS drive. Booting
# a hand-assembled MiNT tree instead needs a bootable AHDI-partitioned
# disk image, and its fVDI has to match the emulated hardware -- an
# earlier attempt to lift a real-hardware (Vampire/SAGA) install in here
# failed on exactly those two points. AFROS also ships a working
# emutos-aranym.img; the one in ~/.aranym on the development machine
# bus-errors at boot, independently of anything in this project.
#
# The NF_STDERR/NF_EXIT trap in host/tools/atari_smoke.c needs no
# changes: ARAnyM implements the same Native Features interface Hatari
# does (see docs/atari-platform.md), so the emulator process really does
# exit with the smoke's own status and this stays an exit-code harness
# rather than a log-scraping one.
#
# Usage: run_afros_smoke_once <display-offset> [mem-prot]
#   <display-offset> must be a distinct small integer per call WITHIN
#   one script run -- same reason atari_ci_lib.sh's own comment gives.
#   [mem-prot] is NO (default) or YES, patched into BootstrapArgs.
# Sets on return (0 = pass, matching the function's own return code):
#   AFROS_CI_STATUS - the ARAnyM process's exit status
#   AFROS_CI_LOG     - combined stdout+stderr, carrying the smoke's own
#                       NF_STDERR PASS/FAIL lines; the caller owns
#                       cleanup (rm -rf "$(dirname "$AFROS_CI_LOG")").
AFROS_FIXTURE="${AFROS_FIXTURE:-$HOME/src/afros}"

run_afros_smoke_once() {
    _offset="$1"
    _memprot="${2:-NO}"
    _scratch="$(mktemp -d)"

    # PID-derived AND offset-derived -- same collision reasoning
    # atari_ci_lib.sh's own comment gives for its Xvfb display number.
    _xvfb_display=":$((97 + ($$ + _offset) % 400))"

    # Work on a copy. MiNT writes into drive_c during boot, and the
    # fixture is the developer's own AFROS install -- CI must not mutate
    # it, the same rule the Mac/Amiga fixtures follow.
    cp -a "$AFROS_FIXTURE" "$_scratch/afros"
    cp bin/atari/SMOKE.PRG "$_scratch/afros/drive_c/SMOKE.PRG"

    _xaaes_cnf="$_scratch/afros/drive_c/mint/1-19-cur/xaaes/xaaes.cnf"

    # Launch the smoke from XaAES's own run list rather than from
    # mint.cnf. mint.cnf executes during kernel init, BEFORE the AES
    # exists -- which would run every selfcheck in a single-tasking
    # context and quietly defeat the entire point of this harness.
    printf '\nrun C:\\SMOKE.PRG\n' >> "$_xaaes_cnf"

    # MEM_PROT is a runtime BootstrapArgs knob, not a build option --
    # no --enable-fullmmu ARAnyM rebuild is needed to exercise memory
    # protection.
    sed -i "s/MEM_PROT=[A-Za-z]*/MEM_PROT=$_memprot/" "$_scratch/afros/config"

    Xvfb "$_xvfb_display" -screen 0 1280x1024x24 >"$_scratch/xvfb.log" 2>&1 &
    _xvfb_pid=$!

    _xvfb_lock="/tmp/.X${_xvfb_display#:}-lock"
    _xvfb_wait=0
    while [ ! -e "$_xvfb_lock" ]; do
        _xvfb_wait=$((_xvfb_wait + 1))
        if [ "$_xvfb_wait" -gt 50 ]; then
            kill "$_xvfb_pid" 2>/dev/null
            wait "$_xvfb_pid" 2>/dev/null
            AFROS_CI_STATUS=1
            AFROS_CI_LOG="$_scratch/xvfb.log"
            return 1
        fi
        sleep 0.1
    done

    # AFROS reaches a full XaAES desktop in well under 35s on this
    # machine; 180s is real margin for a slower host, not a tight bound.
    # ARAnyM's config uses paths relative to its own directory, hence
    # the subshell cd rather than an absolute -c.
    set +e
    ( cd "$_scratch/afros" && \
      env DISPLAY="$_xvfb_display" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
        timeout 180 aranym -c config -N ) \
        >"$_scratch/stdout.log" 2>"$_scratch/stderr.log"
    AFROS_CI_STATUS=$?
    set -e

    kill "$_xvfb_pid" 2>/dev/null
    wait "$_xvfb_pid" 2>/dev/null

    AFROS_CI_LOG="$_scratch/combined.log"
    cat "$_scratch/stdout.log" "$_scratch/stderr.log" > "$AFROS_CI_LOG" 2>/dev/null

    # The whole fixture copy is disposable; only the log outlives the run.
    rm -rf "$_scratch/afros"

    return "$AFROS_CI_STATUS"
}
