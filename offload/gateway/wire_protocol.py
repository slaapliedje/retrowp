"""Shared wire-protocol plumbing for this project's Python gateways
(offload/gateway/odt_gateway.py, offload/gateway/image_gateway.py).

Factored out during M15 (image support) once a SECOND Python gateway
process needed the exact same frame/CRC/HELLO/CAPS/serve-loop
boilerplate odt_gateway.py already had -- the same "share it once
instead of risking drift between two copies" reasoning
offload/gateway/gw_common.c already applies on the C side
(mock_gateway.c/ollama_gateway.c). Re-implements the wire protocol
independently of offload/transport/wire.c (which is C) rather than
linking against it -- there is no Python binding for that module, and
docs/formats/offload-protocol.md's own byte layout is the shared
source of truth both language sides derive from separately. Deliberately
does NOT reimplement wire.c's magic-byte resync-on-corrupt-frame
behavior -- this is host-side tooling on a reliable local transport
(TCP loopback, or the FIFO bridge ci_atari_serial.sh-style tooling
uses), and a malformed frame here just tears down the connection, an
honest, narrower scope than wire.c's own resync contract, not a silent
gap.
"""

import argparse
import os
import select
import socket
import struct
import time

WIRE_MAGIC0 = 0x57
WIRE_MAGIC1 = 0x50
WIRE_VERSION = 1
WIRE_HEADER_SIZE = 12
WIRE_CRC_SIZE = 2

MSG_HELLO, MSG_CAPS, MSG_REQUEST, MSG_RESPONSE, MSG_ERROR = range(5)

(OP_SPELL_CHECK, OP_GRAMMAR_CHECK, OP_THESAURUS,
 OP_CONVERT_IMPORT, OP_CONVERT_EXPORT, OP_CHAT, OP_IMAGE) = range(7)

STATUS_OK, STATUS_TIMEOUT, STATUS_UNSUPPORTED, STATUS_ERR = range(4)

CAPS_PAYLOAD_SIZE = 42  # 2 (version) + 4 (ops_supported) + 4 (max_payload) + 32 (name)


def crc16_ccitt_false(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc


def encode_frame(msg_type, op, status, seq, payload):
    header_tail = bytes([WIRE_VERSION, msg_type, op, status]) + struct.pack('>H', seq) + struct.pack('>I', len(payload))
    crc = crc16_ccitt_false(header_tail + payload)
    return bytes([WIRE_MAGIC0, WIRE_MAGIC1]) + header_tail + payload + struct.pack('>H', crc)


def encode_caps(ops_supported, name):
    name_bytes = name.encode('utf-8')[:32].ljust(32, b'\x00')
    return struct.pack('>HII', WIRE_VERSION, ops_supported, 65536) + name_bytes


class GatewayError(Exception):
    pass


class GatewayTimeout(GatewayError):
    pass


def read_exact(read_fd, n, deadline):
    buf = b''
    while len(buf) < n:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise GatewayTimeout()
        r, _, _ = select.select([read_fd], [], [], remaining)
        if not r:
            raise GatewayTimeout()
        chunk = os.read(read_fd, n - len(buf))
        if not chunk:
            raise GatewayError('peer closed')
        buf += chunk
    return buf


def read_frame(read_fd, timeout_s):
    deadline = time.monotonic() + timeout_s
    header = read_exact(read_fd, WIRE_HEADER_SIZE, deadline)
    if header[0] != WIRE_MAGIC0 or header[1] != WIRE_MAGIC1:
        raise GatewayError('bad magic')
    version, msg_type, op, status = header[2], header[3], header[4], header[5]
    seq = struct.unpack('>H', header[6:8])[0]
    length = struct.unpack('>I', header[8:12])[0]
    if version != WIRE_VERSION:
        raise GatewayError('bad version')
    payload = read_exact(read_fd, length, deadline) if length else b''
    crc_bytes = read_exact(read_fd, WIRE_CRC_SIZE, deadline)
    expected = crc16_ccitt_false(header[2:] + payload)
    if struct.unpack('>H', crc_bytes)[0] != expected:
        raise GatewayError('crc mismatch')
    return {'type': msg_type, 'op': op, 'status': status, 'seq': seq}, payload


def open_fifo_rdwr(path):
    """O_RDWR, not O_RDONLY/O_WRONLY -- avoids the classic named-FIFO
    open-blocks-until-peer-connects deadlock (same trick
    gw_common.c's gw_open_fifo_rdwr and platform/host/transport_host.c's
    pipe: backend already rely on)."""
    if not os.path.exists(path):
        os.mkfifo(path, 0o600)
    return os.open(path, os.O_RDWR)


def serve(read_fd, write_fd, hello_timeout_ms, ops_supported, gateway_name, handle_request):
    """The HELLO/CAPS handshake plus the request/response loop, shared
    verbatim by every Python gateway: read HELLO, answer CAPS, then
    repeatedly read a REQUEST and dispatch it to handle_request(op,
    payload) -> (status, response_payload). Returns a process exit
    code (0 = clean shutdown, e.g. the peer closed the connection; 1 =
    handshake failure)."""
    try:
        hdr, _ = read_frame(read_fd, hello_timeout_ms / 1000.0)
    except GatewayError:
        return 1
    if hdr['type'] != MSG_HELLO:
        return 1

    caps_payload = encode_caps(ops_supported, gateway_name)
    os.write(write_fd, encode_frame(MSG_CAPS, 0, 0, 0, caps_payload))

    while True:
        try:
            hdr, payload = read_frame(read_fd, 120.0)
        except GatewayError:
            break
        status, resp_payload = handle_request(hdr['op'], payload)
        os.write(write_fd, encode_frame(MSG_RESPONSE, hdr['op'], status, hdr['seq'], resp_payload))

    return 0


def parse_gateway_args(description=None, extra_args=None):
    """Parses the standard --serial-rx/--serial-tx/--hello-timeout-ms
    flags every gateway shares, plus any gateway-specific flags a
    caller needs BEFORE it can decide what to pass to run_gateway_with_
    args (e.g. image_gateway.py's --force-pillow-unavailable test hook,
    which determines its own ops_supported bitmask -- mirroring
    mock_gateway.c's own fault-injection-flag convention). extra_args
    is an iterable of (name_tuple, kwargs_dict) pairs, each applied as
    `parser.add_argument(*name_tuple, **kwargs_dict)`."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('--serial-rx')
    parser.add_argument('--serial-tx')
    parser.add_argument('--hello-timeout-ms', type=int, default=5000)
    if extra_args:
        for name_tuple, kwargs in extra_args:
            parser.add_argument(*name_tuple, **kwargs)
    return parser.parse_args()


def run_gateway_with_args(args, handle_request, ops_supported, gateway_name):
    """The transport-setup half of run_gateway(), taking already-parsed
    args (see parse_gateway_args) -- split out so a gateway that needs
    its OWN flags to decide ops_supported can parse once and pass the
    result through, rather than run_gateway parsing internally with no
    way to hand results back before ops_supported is fixed. Opens
    either the named-FIFO pair (T6.3's Hatari serial bridge) or a TCP
    listen socket (printing the OS-assigned port as one line on stdout
    first, the same convention host/tests/gateway_harness.c and
    mock_gateway.c already share), then hands off to serve()."""
    if args.serial_rx and args.serial_tx:
        rx_fd = open_fifo_rdwr(args.serial_rx)
        tx_fd = open_fifo_rdwr(args.serial_tx)
        print('serial', flush=True)
        return serve(rx_fd, tx_fd, args.hello_timeout_ms, ops_supported, gateway_name, handle_request)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', 0))
    srv.listen(1)
    print(srv.getsockname()[1], flush=True)
    conn, _ = srv.accept()
    srv.close()
    fd = conn.fileno()
    return serve(fd, fd, args.hello_timeout_ms, ops_supported, gateway_name, handle_request)


def run_gateway(handle_request, ops_supported, gateway_name, description=None):
    """Full CLI entry point for the common case (no gateway-specific
    flags needed before ops_supported is decided, e.g. odt_gateway.py):
    parse_gateway_args() then run_gateway_with_args(). Returns a
    process exit code -- callers do `sys.exit(run_gateway(...))` from
    their own `if __name__ == '__main__':` block."""
    args = parse_gateway_args(description)
    return run_gateway_with_args(args, handle_request, ops_supported, gateway_name)
