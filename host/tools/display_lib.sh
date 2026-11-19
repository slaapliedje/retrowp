# Sourced, not executed. wp_free_display <n>: the first X display at or
# after :<n> that nobody has -- no socket in /tmp/.X11-unix and no lock
# file -- printed as ":<n>". Every gate starts its Xvfb on one.
#
# Why: other sessions on this machine run their own Xvfb displays. A
# gate that picked a number from its PID alone and found it taken saw
# its Xvfb fail while the OTHER server's lock file made it look started,
# and then ran its emulator -- and sent its keys -- on someone else's
# display (2026-11-18). Checked just before Xvfb starts, so what is
# left is the narrow race of another server taking it in between.
wp_free_display() {
    _wfd_n=$1
    _wfd_tries=0
    while [ -e "/tmp/.X11-unix/X$_wfd_n" ] || [ -e "/tmp/.X$_wfd_n-lock" ]; do
        _wfd_n=$((_wfd_n + 1))
        _wfd_tries=$((_wfd_tries + 1))
        if [ "$_wfd_tries" -gt 500 ]; then break; fi
    done
    echo ":$_wfd_n"
}
