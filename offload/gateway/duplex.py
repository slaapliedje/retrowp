#!/usr/bin/env python3
"""Listen and transmit at the same time, so ONE run of SERRAW exercises
both directions without any timing coordination.

Dumps everything that arrives (hex + ASCII) and, every 2 seconds, sends
a short marker. SERRAW's TX phases run first and its RX phase second, so
a periodic ping means there is always traffic waiting whenever its
listen window opens -- no need to synchronise the two machines by hand.
"""
import os, select, sys, time
sys.path.insert(0, os.path.expanduser('~/retrowp-gw'))
from wire_protocol import open_fifo_rdwr

baud = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 900

fd = open_fifo_rdwr('/dev/ttyAMA0', baud, 'none')
print('duplex on /dev/ttyAMA0 @ %d for %.0fs' % (baud, secs), flush=True)


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
print('  listening, and pinging "PI->ATARI" every 2s', flush=True)

t0 = time.monotonic()
end = t0 + secs
next_ping = t0
total_rx = 0
pings = 0

while time.monotonic() < end:
    now = time.monotonic()
    if now >= next_ping:
        os.write(fd, b'PI->ATARI ping %d\r\n' % pings)
        pings += 1
        next_ping = now + 2.0
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
os.close(fd)
