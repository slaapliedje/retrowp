#!/usr/bin/env python3
"""Hold RTS in a chosen state and keep the port open, so the far end can
be tested against a KNOWN, stable handshake level.

    python3 hold_rts.py off [secs]   # RTS deasserted (pin 7 negative)
    python3 hold_rts.py on  [secs]   # RTS asserted   (pin 7 positive)

Every link in the RTS path has now been verified continuous, the driver
is active, and the Pi genuinely asserts RTS -- and the Falcon still
refuses to transmit. The one thing left untested is the SENSE. If
anything in the chain inverts, the Falcon reads "not clear to send"
BECAUSE we are asserting. Holding the opposite state settles that in one
run, and costs nothing.

Also streams a marker so the far end's receive path stays observable.
"""
import fcntl, os, select, struct, sys, termios, time

sys.path.insert(0, os.path.expanduser('~/retrowp-gw'))
from wire_protocol import open_fifo_rdwr

want_on = (sys.argv[1].lower() in ('on', '1', 'true', 'assert')) if len(sys.argv) > 1 else True
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600

fd = open_fifo_rdwr('/dev/ttyAMA0', 9600, 'none')
fcntl.ioctl(fd, termios.TIOCMBIS if want_on else termios.TIOCMBIC,
            struct.pack('I', 0x004))

b = struct.unpack('I', fcntl.ioctl(fd, termios.TIOCMGET, struct.pack('I', 0)))[0]
print('RTS held %s  (TIOCMGET reports RTS=%s, CTS=%s)'
      % ('ASSERTED' if want_on else 'DEASSERTED',
         bool(b & 0x004), bool(b & 0x020)), flush=True)
print('  pin 7 should be %s. Run the Atari test now.\n'
      % ('positive' if want_on else 'negative'), flush=True)

end = time.monotonic() + secs
next_tx = 0.0
rx = 0
while time.monotonic() < end:
    now = time.monotonic()
    if now >= next_tx:
        os.write(fd, b'PI->ATARI rtshold ')
        next_tx = now + 0.05
    r, _, _ = select.select([fd], [], [], 0.2)
    if not r:
        continue
    chunk = os.read(fd, 256)
    if chunk:
        rx += len(chunk)
        asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chunk)
        print('  +%6.1fs RX %4d B | %s' % (now - (end - secs), len(chunk), asc[:40]),
              flush=True)

print('TOTAL RX: %d bytes' % rx, flush=True)
os.close(fd)
