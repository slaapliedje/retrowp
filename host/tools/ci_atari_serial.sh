#!/bin/sh
# T6.3 CI entrypoint: proves offload_init()'s HELLO/CAPS handshake (and
# one full request/response round trip) actually works over real
# emulated RS-232, not just in principle. Bridges Hatari's
# --rs232-in/--rs232-out to offload/gateway/mock_gateway.c's
# --serial-rx/--serial-tx mode via two named FIFOs, exactly like
# host/tools/ci_atari.sh bridges Hatari's video/AES to the rest of the
# smoke-test suite. See docs/atari-platform.md for the fifo role
# mapping and the open-order rationale (both sides open O_RDWR so
# neither blocks waiting for the other to connect first).
set -e

cd "$(dirname "$0")/../.."

TOS_IMAGE=/usr/share/hatari/etos512k.img
SCRATCH="$(mktemp -d)"
XVFB_DISPLAY=":95"
GW_PID=""
XVFB_PID=""

cleanup() {
    [ -n "$GW_PID" ] && kill "$GW_PID" 2>/dev/null
    [ -n "$XVFB_PID" ] && kill "$XVFB_PID" 2>/dev/null
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

make atari-serial-smoke
make "$PWD/bin/mock_gateway" >/dev/null 2>&1 || make bin/mock_gateway

# Atari's outgoing bytes (Bconout) land in this file (Hatari's
# --rs232-out); the gateway reads its requests from here (--serial-rx).
FIFO_ATARI_OUT="$SCRATCH/atari_out.fifo"
# The gateway writes its responses here (--serial-tx); Hatari delivers
# them to the Atari side (Bconin) via --rs232-in.
FIFO_ATARI_IN="$SCRATCH/atari_in.fifo"

# Start the gateway FIRST: it creates and O_RDWR-opens both FIFOs
# (never blocks), so by the time Hatari opens the same paths a peer
# already exists on each end. --hello-timeout-ms is generous (not the
# default 5000) because a real Atari boot + program load under Hatari's
# cycle-exact CPU emulation can take well over 5 real seconds before the
# ST program ever reaches its first Bconout call -- see
# offload/gateway/mock_gateway.c's file comment for the exact failure
# this caused when it was still hardcoded at 5000 (a SIGPIPE that killed
# Hatari itself, not just a failed test).
./bin/mock_gateway --serial-rx="$FIFO_ATARI_OUT" --serial-tx="$FIFO_ATARI_IN" \
    --hello-timeout-ms=25000 \
    >"$SCRATCH/gw_stdout.log" 2>"$SCRATCH/gw_stderr.log" &
GW_PID=$!

i=0
while [ ! -s "$SCRATCH/gw_stdout.log" ] && [ "$i" -lt 50 ]; do
    sleep 0.1
    i=$((i + 1))
done
if ! grep -q serial "$SCRATCH/gw_stdout.log" 2>/dev/null; then
    echo "ATARI SERIAL CI: gateway did not report readiness"
    cat "$SCRATCH/gw_stderr.log"
    exit 1
fi

Xvfb "$XVFB_DISPLAY" -screen 0 1280x800x24 >"$SCRATCH/xvfb.log" 2>&1 &
XVFB_PID=$!
sleep 1

mkdir -p "$SCRATCH/c_drive"
cp bin/atari/SERIALTST.PRG "$SCRATCH/c_drive/SERIALTST.PRG"

set +e
HOME="$SCRATCH" DISPLAY="$XVFB_DISPLAY" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
    timeout 45 hatari \
    --natfeats on \
    --tos "$TOS_IMAGE" \
    --machine st \
    --memsize 1 \
    --monitor rgb --tos-res med --zoom 2 --statusbar no --borders no \
    --confirm-quit no \
    --sound off \
    --rs232-in "$FIFO_ATARI_IN" --rs232-out "$FIFO_ATARI_OUT" \
    --harddrive "$SCRATCH/c_drive" \
    --auto SERIALTST.PRG \
    >"$SCRATCH/hatari_stdout.log" 2>"$SCRATCH/hatari_stderr.log"
STATUS=$?
set -e

echo "--- SERIALTST log ---"
grep -E "SMOKE|PASS|FAIL" "$SCRATCH/hatari_stderr.log" || true
echo "--- Hatari exit status: $STATUS ---"

if [ "$STATUS" -ne 0 ]; then
    echo "ATARI SERIAL CI: FAILED (exit $STATUS)"
    echo "--- full hatari stderr ---"; cat "$SCRATCH/hatari_stderr.log"
    echo "--- gateway stderr ---"; cat "$SCRATCH/gw_stderr.log"
    exit 1
fi

echo "ATARI SERIAL CI: PASSED"
