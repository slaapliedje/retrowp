#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A serial relay that follows OFFLOAD_SET_SPEED.

/dev/ttyS1 (the core's serial port) <-> /dev/ttyUSB0 (the cable to the
gateway Pi), raw 8N1, both at one speed. Runs ON the MiSTer, with the
core's UART mode at Serial and nothing else holding ttyS1. Bytes pass
straight through both ways, unpaced (the TT030 core keeps line rate
under TOS, measured by the mister-4c session 2026-11-19).

A fixed-speed relay breaks speed negotiation (docs/formats/
offload-protocol.md, OFFLOAD_SET_SPEED): the machine and the gateway
both move, the relay does not, and both fall back. So this one reads
the gateway's frames as they pass. When a RESPONSE to op 11 says OK and
names a rate, it lets that frame reach the core, then moves both of its
ports to the rate -- the moment the two ends move. If no CAPS comes back
from the gateway within SPEED_HOLD_S, it goes back to where it was, as
the gateway does (3 s) and before the client shakes hands again (3.2 s).

Usage: speed_relay.py [baud] [rtscts]     (defaults 9600, off)
Log: /tmp/rwp-speed.log"""
import os, select, struct, sys, termios, time

BASE = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
CRTSCTS = len(sys.argv) > 2 and sys.argv[2] == 'rtscts'
SPEED_HOLD_S = 2.9
T_CAPS, T_RESPONSE = 1, 3
OP_SET_SPEED = 11

log = open('/tmp/rwp-speed.log', 'a', buffering=1)

def say(msg):
    log.write('%.3f %s\n' % (time.time(), msg))

def set_speed(fd, baud, flow):
    a = termios.tcgetattr(fd)
    a[0] = 0; a[1] = 0; a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    if flow:
        a[2] |= termios.CRTSCTS
    sp = getattr(termios, 'B%d' % baud)
    a[4] = a[5] = sp
    a[6][termios.VMIN] = 1; a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)

def open_raw(path, flow):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    set_speed(fd, BASE, flow)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd

class Frames:
    """Finds whole frames in one direction's byte stream (magic 'WP',
    a 12-byte header, payload, CRC). Never holds bytes back: it only
    watches. Returns (type, op, status, payload) for each frame seen."""
    def __init__(self):
        self.buf = b''
    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b'WP')
            if i < 0:
                self.buf = self.buf[-1:]
                return out
            self.buf = self.buf[i:]
            if len(self.buf) < 12:
                return out
            ver, typ, op, st, seq, ln = struct.unpack('>BBBBHI', self.buf[2:12])
            if ver != 1 or ln > 65536:
                self.buf = self.buf[2:]
                continue
            if len(self.buf) < 14 + ln:
                return out
            out.append((typ, op, st, self.buf[12:12 + ln]))
            self.buf = self.buf[14 + ln:]

core, cable = open_raw('/dev/ttyS1', CRTSCTS), open_raw('/dev/ttyUSB0', False)
from_gw = Frames()
speed, pending_since = BASE, None
say('speed relay at %d%s' % (BASE, ' rtscts on ttyS1' if CRTSCTS else ''))

def move(baud, why):
    global speed
    termios.tcdrain(core)
    set_speed(core, baud, CRTSCTS)
    set_speed(cable, baud, False)
    say('%s: %d -> %d' % (why, speed, baud))
    speed = baud

while True:
    timeout = None
    if pending_since is not None:
        timeout = max(0.0, pending_since + SPEED_HOLD_S - time.time())
    r, _, _ = select.select([core, cable], [], [], timeout)
    if core in r:
        d = os.read(core, 4096)
        os.write(cable, d)
        say('> %d' % len(d))
    if cable in r:
        d = os.read(cable, 4096)
        os.write(core, d)
        say('< %d' % len(d))
        for typ, op, st, payload in from_gw.feed(d):
            if typ == T_RESPONSE and op == OP_SET_SPEED and st == 0 and len(payload) == 4:
                old = speed
                move(struct.unpack('>I', payload)[0], 'SET_SPEED answered')
                pending_since, back_to = time.time(), old
            elif typ == T_CAPS and pending_since is not None:
                say('CAPS at %d: the new speed holds' % speed)
                pending_since = None
    if pending_since is not None and time.time() >= pending_since + SPEED_HOLD_S:
        move(back_to, 'no CAPS in %.1f s, back' % SPEED_HOLD_S)
        pending_since = None
