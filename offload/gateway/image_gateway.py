#!/usr/bin/env python3
"""Standalone image-downconversion gateway (M15, T15.4).

A fourth offload/gateway executable, alongside mock_gateway.c
(fixture-backed), ollama_gateway.c (real-LLM-backed), and
odt_gateway.py (ODT/PDF conversion). Answers the real OFFLOAD_IMAGE op:
an arbitrary source image (whatever Pillow's Image.open can decode --
PNG/JPEG/GIF/BMP) in, a small palette-indexed bitmap matched to the
REQUESTING target's own real color ceiling out. Everything else
responds OFFLOAD_UNSUPPORTED.

Why this needs its own gateway process rather than a new format_id on
odt_gateway.py's existing CONVERT_IMPORT/EXPORT op (M15 plan, Section
4): OFFLOAD_IMAGE is a genuinely different op, not another document
format, and this gateway's one real dependency (Pillow) is optional --
gating it per-process (this whole gateway simply isn't started, or its
CAPS never sets the IMAGE bit, when Pillow isn't installed) is cleaner
than threading an optional-dependency check through odt_gateway.py's
existing format_id dispatch.

Wire format (docs/formats/offload-protocol.md's OFFLOAD_IMAGE section):
  Request:  target_max_colors(1) target_flags(1) target_max_width_px(2)
            target_max_height_px(2) source_bytes(...)
  Response: width_px(2) height_px(2) palette_count(1)
            palette(palette_count*3) packed_4bit_pixels(...)
The response's packed pixel format (2 pixels/byte, high nibble = even
x) and the WP_IMAGE_MAX_WIDTH_PX/HEIGHT_PX ceiling (320x200, the
Atari ST low-res limit -- the most permissive of this project's real
targets) are chosen so the worst case (a full 320x200, 16-color image)
fits comfortably under OFFLOAD_MAX_RESPONSE_PAYLOAD (32768 bytes,
engine/src/offload_client.c) with zero changes needed to the wire
protocol layer itself -- see the M15 plan's own arithmetic.

Uses Pillow (PIL), gated exactly like odt_gateway.py's own optional
`fonttools` dependency (PDF Tier-1 font embedding): imported inside a
try/except, with OFFLOAD_IMAGE's CAPS bit set only when the import
succeeded. Fully deterministic (decode/resize/quantize -- no LLM in
the loop), the same testing category as odt_gateway.py, unlike
ollama_gateway.c.
"""

import io
import struct
import sys

from wire_protocol import (
    STATUS_OK, STATUS_UNSUPPORTED, STATUS_ERR,
    OP_IMAGE,
    parse_gateway_args, run_gateway_with_args,
)

try:
    from PIL import Image
    _PILLOW_AVAILABLE = True
except ImportError:
    _PILLOW_AVAILABLE = False

WP_IMAGE_MAX_WIDTH_PX = 320
WP_IMAGE_MAX_HEIGHT_PX = 200

TARGET_GRAYSCALE = 0x01


def _pack_4bit_indexed(indices, width, height):
    """Row-major, 2 pixels/byte, high nibble = even x, matching the
    wire format's own convention. An odd width's final byte per row
    has an unused low nibble (left 0, per the protocol doc)."""
    out = bytearray()
    row_bytes = (width + 1) // 2
    for y in range(height):
        row = bytearray(row_bytes)
        base = y * width
        for x in range(width):
            nibble = indices[base + x] & 0x0F
            byte_idx = x // 2
            if x % 2 == 0:
                row[byte_idx] |= nibble << 4
            else:
                row[byte_idx] |= nibble
        out += row
    return bytes(out)


def _downconvert(source_bytes, target_max_colors, grayscale,
                  target_max_width_px, target_max_height_px):
    """Real decode + resize + quantize/dither via Pillow. Returns
    (width_px, height_px, palette (list of (r,g,b)), packed_indices)."""
    decoded = Image.open(io.BytesIO(source_bytes)).convert('RGB')

    scale = min(target_max_width_px / decoded.width,
                target_max_height_px / decoded.height, 1.0)
    new_w = max(1, int(decoded.width * scale))
    new_h = max(1, int(decoded.height * scale))
    resized = decoded.resize((new_w, new_h), Image.LANCZOS)

    if grayscale:
        mono = resized.convert('1', dither=Image.FLOYDSTEINBERG)
        palette = [(0, 0, 0), (255, 255, 255)]
        pixels = mono.load()
        indices = [1 if pixels[x, y] else 0
                   for y in range(new_h) for x in range(new_w)]
    else:
        colors = max(1, min(16, target_max_colors))
        quantized = resized.quantize(colors=colors)
        raw_palette = quantized.getpalette()[:colors * 3]
        palette = [tuple(raw_palette[i:i + 3]) for i in range(0, len(raw_palette), 3)]
        pixels = quantized.load()
        indices = [pixels[x, y] for y in range(new_h) for x in range(new_w)]

    return new_w, new_h, palette, indices


def handle_image(payload, pillow_ok):
    if not pillow_ok:
        return STATUS_UNSUPPORTED, b''
    if len(payload) < 6:
        return STATUS_ERR, b''

    target_max_colors = payload[0]
    target_flags = payload[1]
    target_max_width_px = struct.unpack('>H', payload[2:4])[0]
    target_max_height_px = struct.unpack('>H', payload[4:6])[0]
    source_bytes = payload[6:]

    target_max_width_px = min(target_max_width_px, WP_IMAGE_MAX_WIDTH_PX)
    target_max_height_px = min(target_max_height_px, WP_IMAGE_MAX_HEIGHT_PX)
    if target_max_width_px < 1 or target_max_height_px < 1:
        return STATUS_ERR, b''

    grayscale = bool(target_flags & TARGET_GRAYSCALE)

    try:
        width_px, height_px, palette, indices = _downconvert(
            source_bytes, target_max_colors, grayscale,
            target_max_width_px, target_max_height_px)
    except Exception:
        return STATUS_ERR, b''

    palette_count = len(palette)
    if palette_count < 1 or palette_count > 16:
        return STATUS_ERR, b''

    packed = _pack_4bit_indexed(indices, width_px, height_px)

    resp = bytearray()
    resp += struct.pack('>H', width_px)
    resp += struct.pack('>H', height_px)
    resp += bytes([palette_count])
    for r, g, b in palette:
        resp += bytes([r, g, b])
    resp += packed

    return STATUS_OK, bytes(resp)


def handle_request(op, payload, pillow_ok):
    if op == OP_IMAGE:
        return handle_image(payload, pillow_ok)
    return STATUS_UNSUPPORTED, b''


def main():
    args = parse_gateway_args(description=__doc__, extra_args=[
        (('--force-pillow-unavailable',), {
            'action': 'store_true',
            'help': 'Test-only hook: report OFFLOAD_IMAGE as unsupported '
                     'regardless of whether Pillow is actually importable '
                     '(host/tests/test_image_convert.c uses this to '
                     'deterministically exercise the no-Pillow-installed '
                     'degrade path in an environment that DOES have '
                     'Pillow, mirroring mock_gateway.c\'s own fault-'
                     'injection-flag convention).',
        }),
    ])
    pillow_ok = _PILLOW_AVAILABLE and not args.force_pillow_unavailable
    ops_supported = (1 << OP_IMAGE) if pillow_ok else 0
    return run_gateway_with_args(
        args, lambda op, payload: handle_request(op, payload, pillow_ok),
        ops_supported, 'image-gateway')


if __name__ == '__main__':
    sys.exit(main() or 0)
