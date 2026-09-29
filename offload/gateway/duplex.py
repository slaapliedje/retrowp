#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Listen and transmit at the same time, so ONE run of SERRAW exercises
both directions without any timing coordination.

Dumps everything that arrives (hex + ASCII) and, every 2 seconds, sends
a short marker. SERRAW's TX phases run first and its RX phase second, so
a periodic ping means there is always traffic waiting whenever its
listen window opens -- no need to synchronise the two machines by hand.
"""
import fcntl, os, select, struct, sys, termios, time
sys.path.insert(0, os.path.expanduser('~/retrowp-gw'))
from wire_protocol import open_fifo_rdwr

baud = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 900

fd = open_fifo_rdwr('/dev/ttyAMA0', baud, 'none')
print('duplex on /dev/ttyAMA0 @ %d for %.0fs' % (baud, secs), flush=True)

# TIOCM_* bit values are stable Linux ABI (asm-generic/termios.h); named
# here rather than imported because termios exposes only some of them.
TIOCM_DTR, TIOCM_RTS = 0x002, 0x004
TIOCM_CTS, TIOCM_DSR, TIOCM_CD, TIOCM_RI = 0x020, 0x100, 0x040, 0x080


def modem_bits(fd):
    return struct.unpack('I', fcntl.ioctl(fd, termios.TIOCMGET,
                                          struct.pack('I', 0)))[0]


def modem_str(bits):
    return 'CTS=%d DSR=%d DCD=%d RI=%d' % (bool(bits & TIOCM_CTS),
                                           bool(bits & TIOCM_DSR),
                                           bool(bits & TIOCM_CD),
                                           bool(bits & TIOCM_RI))


def assert_rts_dtr(fd):
    """Drive RTS and DTR high explicitly, and keep them there.

    We run FLOW_NONE, so the kernel never touches RTS on our behalf --
    whatever state the open() happened to leave it in is what the far end
    sees, and that is not something to leave to a default. It matters
    because of how the cable is wired: our RTS (DB9 pin 7) crosses to the
    Atari's pin 8, which IS its CTS, and our DTR (pin 4) crosses to its
    pin 6, its DSR. The Falcon's transmitter is blocked with Bcostat
    reporting not-ready, and a handshake input it is waiting on is the
    standing hypothesis -- so assert both, unconditionally, for the whole
    session. If that unblocks it, the answer is on this side of the link
    and needs no probing inside the machine.
    """
    fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', TIOCM_DTR | TIOCM_RTS))


assert_rts_dtr(fd)
print('  asserted our RTS + DTR (-> far end CTS + DSR)', flush=True)
print('  far end is driving: %s' % modem_str(modem_bits(fd)), flush=True)


def loopback_probe(fd):
    """Refuse to run while the port is talking to itself.

    A loopback jumper makes every symptom of a dead link -- nothing
    reaches the far end, nothing arrives from it -- while the local
    tooling looks perfectly healthy, because its own bytes come back.
    That cost a full round of testing here: an entire sweep was declared
    a dead cable when in fact nothing had ever left the HAT. Cheap to
    detect, so detect it every single time rather than remembering to.
    """
    while True:
        r, _, _ = select.select([fd], [], [], 0.3)
        if not r:
            break
        os.read(fd, 4096)
    probe = b'LOOPBACK-PROBE-%d\r\n' % os.getpid()
    os.write(fd, probe)
    got = b''
    end = time.monotonic() + 1.5
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], end - time.monotonic())
        if not r:
            break
        got += os.read(fd, 256)
    return probe.strip() in got


if loopback_probe(fd):
    print('  *** ABORT: the port is LOOPED BACK -- our own bytes came', flush=True)
    print('      straight back. Nothing can reach the far end. Remove the', flush=True)
    print("      HAT's loopback jumper before reading anything into a result.", flush=True)
    os.close(fd)
    sys.exit(2)

print('  loopback check passed (nothing echoed)', flush=True)
print('  listening, and streaming "PI->ATARI" continuously', flush=True)
print('  -> the HAT TX LED should be flickering solidly RIGHT NOW', flush=True)

t0 = time.monotonic()
end = t0 + secs
next_ping = t0
total_rx = 0
pings = 0
last_bits = modem_bits(fd)

while time.monotonic() < end:
    now = time.monotonic()
    bits = modem_bits(fd)
    if bits != last_bits:
        # Printed on CHANGE, not on a timer: what matters is the edge
        # (the far end powering on, or dropping a line the instant it
        # tries to transmit), and a periodic dump would bury it.
        print('  +%6.1fs MODEM %s' % (now - t0, modem_str(bits)), flush=True)
        last_bits = bits
    if now >= next_ping:
        # A near-continuous stream, not an occasional blink. The HAT has
        # a TX LED, and a 2-second blink is easy to miss while looking at
        # a board from an angle; a solid flicker is unmistakable. Being
        # able to SEE the signal reach the driver chip rules out the
        # whole software stack in one glance.
        os.write(fd, b'PI->ATARI ping %d ' % pings)
        pings += 1
        next_ping = now + 0.05
    r, _, _ = select.select([fd], [], [], 0.25)
    if not r:
        continue
    chunk = os.read(fd, 256)
    if not chunk:
        continue
    total_rx += len(chunk)
    hexs = ' '.join('%02X' % b for b in chunk)
    asc = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
    print('  +%6.1fs RX %4d B | %-48s | %s'
          % (now - t0, len(chunk), hexs[:48], asc[:32]), flush=True)

print('TOTAL RX: %d bytes over %d pings sent' % (total_rx, pings), flush=True)
print('final far-end state: %s' % modem_str(modem_bits(fd)), flush=True)
os.close(fd)
