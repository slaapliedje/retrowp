#!/usr/bin/env python3
"""Validate the DIB inside an exported RTF's \\pict -- with a real decoder.

The C tests for image export assert the bytes this project writes, which
proves those bytes are what was intended and nothing more. Whether a
FOREIGN reader accepts the picture is a different question, and the only
honest way to answer it is to hand the bytes to one.

A \\dibitmap0 payload is exactly a .bmp minus its 14-byte
BITMAPFILEHEADER, so putting that header back gives a file Pillow (or
anything else) can open.

    python3 host/tools/rtf_dib_check.py saved.rtf [--png out.png]

Exit status is 0 when every picture in the file decodes.
"""
import io
import re
import struct
import sys


def dibs_in(rtf_text):
    """Every \\pict payload in the document, as raw DIB bytes."""
    for m in re.finditer(r"\{\\pict\\dibitmap0(.*?)\}", rtf_text, re.S):
        body = m.group(1)
        # the control words come first; the hex is everything after them
        hexdata = re.sub(r"\\[a-z]+-?[0-9]*", "", body)
        hexdata = re.sub(r"[^0-9a-fA-F]", "", hexdata)
        if hexdata:
            yield bytes.fromhex(hexdata)


def as_bmp(dib):
    hdr_size = struct.unpack_from("<I", dib, 0)[0]
    clr_used = struct.unpack_from("<I", dib, 32)[0]
    pixel_off = 14 + hdr_size + clr_used * 4
    return b"BM" + struct.pack("<IHHI", 14 + len(dib), 0, 0, pixel_off) + dib


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    path = argv[0]
    png = argv[argv.index("--png") + 1] if "--png" in argv else None

    try:
        from PIL import Image
    except ImportError:
        print("Pillow is not installed here -- nothing was checked")
        return 2

    text = open(path, "rb").read().decode("latin-1")
    found = 0
    for i, dib in enumerate(dibs_in(text)):
        w, h, planes, bits = struct.unpack_from("<iiHH", dib, 4)
        try:
            im = Image.open(io.BytesIO(as_bmp(dib)))
            im.load()
        except Exception as e:                       # noqa: BLE001
            print(f"picture {i}: REJECTED by Pillow -- {type(e).__name__}: {e}")
            return 1
        ok = (im.size == (abs(w), abs(h)))
        print(f"picture {i}: {im.size[0]}x{im.size[1]} {bits}bpp mode={im.mode} "
              f"{'' if ok else '-- SIZE MISMATCH vs header '}"
              f"{'' if ok else f'{w}x{h}'}")
        if not ok:
            return 1
        if png:
            im.convert("RGB").save(png if i == 0 else f"{png}.{i}.png")
        found += 1

    if found == 0:
        print("no \\pict\\dibitmap0 found in that file")
        return 1
    print(f"{found} picture(s) decoded")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
