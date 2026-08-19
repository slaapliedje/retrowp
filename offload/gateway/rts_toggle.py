#!/usr/bin/env python3
"""Toggle RTS on a slow 2-second cycle so a multimeter can follow it.

Measure HAT DB9 pin 7 against pin 5 (GND). RS-232 signalling is +/-, and
the MAX3232 inverts, so an asserted RTS (GPIO17 low) drives pin 7
POSITIVE:

    asserted   -> pin 7 about +5 V
    deasserted -> pin 7 about -5 V

A pin that swings between the two has a good solder joint and a working
driver. A pin that sits at 0 V and never moves is not connected to the
board, whatever a continuity probe touching the plated ring may say.
"""
import fcntl, os, struct, sys, termios, time

sys.path.insert(0, os.path.expanduser('~/retrowp-gw'))
from wire_protocol import open_fifo_rdwr

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 120
fd = open_fifo_rdwr('/dev/ttyAMA0', 9600, 'none')
print('Toggling RTS every 2s for %.0fs.' % secs)
print('Measure HAT DB9 pin 7 vs pin 5 (GND), DC volts.\n', flush=True)

end = time.monotonic() + secs
state = True
while time.monotonic() < end:
    if state:
        fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', 0x004))
    else:
        fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack('I', 0x004))
    print('  RTS %-10s -> pin 7 should read %s'
          % ('ASSERTED' if state else 'deasserted',
             'about +5V' if state else 'about -5V'), flush=True)
    state = not state
    time.sleep(2.0)

fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', 0x004))   # leave asserted
print('\nDone -- RTS left ASSERTED.', flush=True)
os.close(fd)
