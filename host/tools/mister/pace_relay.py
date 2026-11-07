#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A serial relay that paces what it sends the vintage machine.

/dev/ttyS1 (the core's serial port) <-> /dev/ttyUSB0 (the cable to the
gateway), both raw 8N1 at the same speed. Bytes from the core pass
straight through; bytes to the core go one at a time with GAP_MS
between them, for a machine that cannot take a burst at line rate.
Runs ON the MiSTer, with the core's UART mode at None (so Main does not
hold ttyS1) and restarted after every core load or mode change, which
kill whatever holds the port. Needed for the MacPlus core, whose SCC
model always runs at 115200 whatever the Mac programs and keeps no
receive FIFO: an unpaced burst (a gateway's 56-byte CAPS in 5 ms) is
overrun and lost, and RetroWP reports no gateway. 2 ms a byte works
(docs/mac-platform.md, "Printing from the MiSTer Mac Plus").

Usage: pace_relay.py [baud] [gap_ms]     (defaults 115200, 2)"""
import os, select, sys, termios, time

baud = int(sys.argv[1]) if len(sys.argv) > 1 else 115200
gap = (float(sys.argv[2]) if len(sys.argv) > 2 else 2.0) / 1000.0
speed = getattr(termios, 'B%d' % baud)

def open_raw(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[4] = a[5] = speed
    a[6][termios.VMIN] = 1; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

core, cable = open_raw('/dev/ttyS1'), open_raw('/dev/ttyUSB0')
log = open('/tmp/rwp-pace.log', 'a', buffering=1)
log.write('pace relay %d baud, %.1f ms a byte to the core\n' % (baud, gap * 1000))
while True:
    r, _, _ = select.select([core, cable], [], [])
    if core in r:
        d = os.read(core, 4096)
        os.write(cable, d)
        log.write('%.3f > %s\n' % (time.time(), d.hex(' ')))
    if cable in r:
        d = os.read(cable, 4096)
        for b in d:
            os.write(core, bytes([b]))
            time.sleep(gap)
        log.write('%.3f < %s\n' % (time.time(), d.hex(' ')))
