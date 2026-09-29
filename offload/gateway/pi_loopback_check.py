#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
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

# A SEPARATE, much larger payload for the timing figure, because 256
# bytes cannot measure a fast line. At 115200 the transfer itself is 22ms
# and the select/read loop's own overhead is comparable, so the first
# version of this script reported 19200 running at 9600's throughput and
# 115200 at a third of its rate -- an artifact that reads exactly like a
# misconfigured port. 8 KiB puts every rate in this range at 99-100% of
# theoretical, which is what makes a genuine shortfall visible.
THROUGHPUT_BYTES = 8192
THROUGHPUT_PATTERN = bytes((i * 7 + 3) & 0xFF for i in range(THROUGHPUT_BYTES))


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


def echo(fd, payload, baud):
    """Write payload, read it back, return (bytes_got, elapsed)."""
    drain(fd)
    t0 = time.monotonic()
    os.write(fd, payload)

    got = b''
    deadline = t0 + 5.0 + len(payload) * 12.0 / baud
    while len(got) < len(payload):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        r, _, _ = select.select([fd], [], [], remaining)
        if not r:
            break
        chunk = os.read(fd, len(payload) - len(got))
        if not chunk:
            break
        got += chunk
    return got, time.monotonic() - t0


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
        deadline = t0 + 5.0 + len(PATTERN) * 12.0 / baud
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
            # All 256 byte values survived. Now measure on a payload big
            # enough for the number to mean something -- on a FRESH fd.
            #
            # Not merely tidy: a preceding rtscts run with CTS low leaves
            # its bytes stuck in the output buffer, and measuring after
            # that on a carried-over descriptor reported each rate at the
            # PREVIOUS one's throughput (19200 as 960 B/s, 115200 as 1920)
            # -- a clean, plausible, entirely wrong number that looks
            # exactly like a misconfigured port. Reopening isolates each
            # measurement from whatever the last configuration left
            # behind.
            os.close(fd)
            time.sleep(0.2)
            fd = open_fifo_rdwr(path, baud, flow)
            big, big_el = echo(fd, THROUGHPUT_PATTERN, baud)
            if big == THROUGHPUT_PATTERN and big_el > 0:
                rate = len(big) / big_el
                print('OK  all 256 byte values, %.0f B/s (%.0f%% of theoretical)'
                      % (rate, 100.0 * rate / (baud / 10.0)))
            else:
                print('OK  all 256 byte values, throughput pass returned %d/%d'
                      % (len(big), len(THROUGHPUT_PATTERN)))
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
