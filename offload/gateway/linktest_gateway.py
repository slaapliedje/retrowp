#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A gateway whose only job is to prove a serial link works.

Deliberately the dumbest gateway in this directory. It imports nothing
beyond wire_protocol (itself stdlib-only), touches no microphone, loads
no model, shells out to nothing, and reads no files. That is the whole
point: during link bring-up the ONLY thing that should be able to make
this fail is the link. whisper_gateway.py would serve a handshake fine
too, right up until the run where it doesn't and the half hour spent
deciding whether the cable or the audio stack was at fault.

It answers HELLO with CAPS advertising every op, and answers any request
with a short fixed ASCII payload naming the op. That is enough to prove
framing, CRC, byte order and both directions of the wire, which is
everything a link test needs to establish.

Usage on the Pi (see docs/atari-platform.md's bring-up section):

    python3 linktest_gateway.py --serial-rx=/dev/ttyAMA0 \\
                                --serial-tx=/dev/ttyAMA0 \\
                                --serial-baud=19200 --serial-flow=rtscts
"""

import sys

from wire_protocol import (STATUS_OK, parse_gateway_args,
                           run_gateway_with_args)

# Every op bit set. A link test should never be the thing that decides a
# capability is unavailable -- if a caller gets an op wrong, the reply
# below says so in plain text rather than an UNSUPPORTED status that
# looks like a transport problem.
ALL_OPS = 0xFFFFFFFF


def handle_request(op, payload):
    reply = 'linktest: op=%d, %d payload bytes received' % (op, len(payload))
    return STATUS_OK, reply.encode('utf-8')


def main():
    args = parse_gateway_args(description=__doc__)
    return run_gateway_with_args(args, handle_request, ALL_OPS, 'linktest')


if __name__ == '__main__':
    sys.exit(main())
