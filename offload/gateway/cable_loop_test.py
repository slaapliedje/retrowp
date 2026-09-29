#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Test the CABLE, with the Atari removed from the equation entirely.

Unplug the cable from the Atari and short pins 2 and 3 together at that
loose end (a paperclip or jumper wire does it). Then run this. Whatever
the Pi transmits travels the full path -- UART, HAT driver, jumper
block, HAT's DB9, connector, the whole cable -- reaches the shorted far
end, turns around, and comes all the way back.

That single result splits the remaining problem cleanly:

  bytes come back  -> the entire physical path from the Pi to the far
                      END OF THE CABLE is good, including the jumper
                      settings. Whatever is wrong is then at the Atari's
                      connector or port, and nothing before it.
  nothing back     -> the fault is in the HAT, the jumpers or the cable
                      itself, and the Atari has never been involved.

This is the opposite polarity to duplex.py's startup guard, which aborts
when it sees an echo. Here the echo IS the pass condition, so it needs
to be its own script rather than a flag nobody will remember to set.

    python3 cable_loop_test.py [baud]
"""

import os
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wire_protocol import open_fifo_rdwr  # noqa: E402

PATTERN = b'CABLE-LOOP-0123456789-abcdefghij\r\n'


def main():
    baud = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
    fd = open_fifo_rdwr('/dev/ttyAMA0', baud, 'none')

    print('Cable loop test @ %d baud.' % baud)
    print('Pins 2 and 3 must be shorted at the FAR end of the cable,')
    print('with the cable UNPLUGGED from the Atari.\n')

    while True:                                   # drain anything stale
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            break
        os.read(fd, 4096)

    os.write(fd, PATTERN)
    got = b''
    end = time.monotonic() + 3.0
    while len(got) < len(PATTERN) and time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], end - time.monotonic())
        if not r:
            break
        got += os.read(fd, len(PATTERN) - len(got))

    os.close(fd)

    if got == PATTERN:
        print('  PASS -- all %d bytes returned.' % len(got))
        print('  The Pi, the HAT, the jumper block, both DB9 connectors and')
        print('  the cable are ALL good. Look at the Atari end from here.')
        return 0
    if got:
        print('  PARTIAL -- %d of %d bytes back: %r' % (len(got), len(PATTERN), got))
        print('  Something is getting through but corrupting. Suspect a')
        print('  marginal connection or a baud mismatch rather than wiring.')
        return 1
    print('  FAIL -- nothing came back.')
    print('  With pins 2-3 shorted the Pi cannot even hear itself, so the')
    print('  break is between the UART and the far end of the cable: the')
    print('  jumper block (TX and RX must be on the SAME row), a connector,')
    print('  or the cable. The Atari is not involved and can be ruled out.')
    return 1


if __name__ == '__main__':
    sys.exit(main())
