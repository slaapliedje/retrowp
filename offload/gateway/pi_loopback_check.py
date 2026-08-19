#!/usr/bin/env python3
"""Prove the Pi's own UART works before any of it is blamed on a cable.

Run with the HAT's jumpers set to LOOPBACK. Whatever this writes should
come straight back. It opens the port through wire_protocol's own
open_fifo_rdwr, so it exercises the exact termios path the gateway uses
-- baud, raw mode, VMIN=1, CLOCAL and CRTSCTS -- rather than a
lookalike that could pass while the real one fails.

What a failure here tells you, which is the useful part:

  * Fails at 'none' but the wiring is looped back -> the UART itself,
    the overlay, or the device path is wrong. Nothing about the Falcon
    or the cable is involved yet.
  * Passes at 'none', fails at 'rtscts' -> CTS is not reaching the SoC.
    On a Pi 5 that is almost always the missing dtoverlay=uart0-pi5,ctsrts
    (CTS/RTS live on GPIO16/17 and are NOT routed by default), or the
    HAT's handshake jumpers not looping RTS back to CTS.
  * Passes at both -> the Pi end is sound. Anything that fails after
    this point is the cable, the Falcon, or the far end's settings.

    python3 pi_loopback_check.py /dev/ttyAMA0 19200
"""

import os
import select
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wire_protocol import open_fifo_rdwr  # noqa: E402

# Deliberately includes 0x11 and 0x13 -- XON and XOFF. If anything in the
# path is doing software flow control these two vanish and the compare
# fails loudly, which is the whole reason this protocol refuses to offer
# XON/XOFF in the first place.
PATTERN = bytes(range(0, 256))


def drain(fd, timeout=0.4):
    while True:
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return
        try:
            if not os.read(fd, 4096):
                return
        except OSError:
            return


def run(path, baud, flow):
    print('  %-7s @ %-6d : ' % (flow, baud), end='', flush=True)
    try:
        fd = open_fifo_rdwr(path, baud, flow)
    except Exception as exc:                       # noqa: BLE001
        print('open failed: %s' % exc)
        return False

    try:
        drain(fd)
        t0 = time.monotonic()
        os.write(fd, PATTERN)

        got = b''
        deadline = t0 + 5.0 + len(PATTERN) * 10.0 / baud
        while len(got) < len(PATTERN):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            r, _, _ = select.select([fd], [], [], remaining)
            if not r:
                break
            chunk = os.read(fd, len(PATTERN) - len(got))
            if not chunk:
                break
            got += chunk
        elapsed = time.monotonic() - t0

        if got == PATTERN:
            print('OK  %d/%d bytes in %.2fs (%.0f B/s)'
                  % (len(got), len(PATTERN), elapsed,
                     len(got) / elapsed if elapsed else 0))
            return True

        missing = sorted(set(PATTERN) - set(got))
        print('MISMATCH %d/%d bytes back' % (len(got), len(PATTERN)), end='')
        if missing and set(missing) <= {0x11, 0x13}:
            print('  -- ONLY 0x11/0x13 missing: something is eating XON/XOFF')
        elif len(got) == 0:
            print('  -- nothing came back at all')
        else:
            print('')
        return False
    finally:
        os.close(fd)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyAMA0'
    bauds = [int(x) for x in sys.argv[2:]] or [9600, 19200, 38400, 115200]

    print('Loopback check on %s -- set the HAT jumpers to LOOPBACK first.\n' % path)
    ok = 0
    for baud in bauds:
        for flow in ('none', 'rtscts'):
            ok += 1 if run(path, baud, flow) else 0
    print('\n%d configuration(s) looped back cleanly.' % ok)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
