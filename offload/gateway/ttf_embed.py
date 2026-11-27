# SPDX-License-Identifier: GPL-2.0-or-later
"""A TrueType face read and cut down for a PDF, with the standard library.

The PDF writer (odt_gateway.py) embeds Liberation Sans, Serif and Mono
so a document's every character prints -- Greek, typographic quotes and
dashes, anything the face has -- where the PDF's built-in faces stop at
Latin-1. The gateway's Pi has the fonts but not fontTools, so this reads
them itself (the OpenType spec's table formats: head, hhea, maxp, hmtx,
cmap 4 and 12, loca, glyf, OS/2, post).

A subset holds the glyphs used, numbered afresh in their order, with the
face's hinting programs; it says where each glyph went, which the PDF
gives as its CIDToGIDMap so the page text can name glyphs by their
number in the whole face. Deterministic: the same glyphs make the same
bytes.
"""

import struct

# What a PDF's TrueType font program needs (PDF 1.7, 9.9), in tag order.
_KEEP = (b'cvt ', b'fpgm', b'glyf', b'head', b'hhea', b'hmtx', b'loca', b'maxp', b'prep')

# Composite glyph flags (OpenType 'glyf').
_ARG_WORDS = 0x0001
_HAVE_SCALE = 0x0008
_MORE = 0x0020
_HAVE_XY_SCALE = 0x0040
_HAVE_2X2 = 0x0080


def _u16(b, o):
    return struct.unpack_from('>H', b, o)[0]


def _i16(b, o):
    return struct.unpack_from('>h', b, o)[0]


def _u32(b, o):
    return struct.unpack_from('>I', b, o)[0]


def _checksum(data):
    data = data + b'\0' * (-len(data) % 4)
    return sum(struct.unpack('>%dI' % (len(data) // 4), data)) & 0xFFFFFFFF


class Face:
    """One .ttf file's metrics and outlines. Raises ValueError for a file
    that is not a glyf-outline TrueType face."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = data = f.read()
        if data[:4] not in (b'\0\1\0\0', b'true'):
            raise ValueError('%s: not a TrueType face' % path)
        self.tables = {}
        for i in range(_u16(data, 4)):
            tag, _, off, length = struct.unpack_from('>4sIII', data, 12 + 16 * i)
            self.tables[tag] = data[off:off + length]
        for tag in (b'head', b'hhea', b'maxp', b'hmtx', b'cmap', b'loca', b'glyf'):
            if tag not in self.tables:
                raise ValueError('%s: no %s table' % (path, tag.decode()))
        head, hhea = self.tables[b'head'], self.tables[b'hhea']
        self.upm = _u16(head, 18)
        self.bbox = [_i16(head, 36 + 2 * k) for k in range(4)]
        self.long_loca = _i16(head, 50) == 1
        self.ascent, self.descent = _i16(hhea, 4), _i16(hhea, 6)
        self.n_hmetrics = _u16(hhea, 34)
        self.n_glyphs = _u16(self.tables[b'maxp'], 4)
        os2 = self.tables.get(b'OS/2')
        self.cap_height = _i16(os2, 88) if os2 and _u16(os2, 0) >= 2 and len(os2) >= 90 else self.ascent
        post = self.tables.get(b'post')
        self.italic_angle = struct.unpack_from('>i', post, 4)[0] / 65536.0 if post else 0.0
        self.cmap = self._read_cmap()

    # -- metrics -------------------------------------------------------

    def advance(self, gid):
        """The glyph's advance in the face's own units."""
        hmtx = self.tables[b'hmtx']
        return _u16(hmtx, 4 * min(gid, self.n_hmetrics - 1))

    def _lsb(self, gid):
        hmtx = self.tables[b'hmtx']
        if gid < self.n_hmetrics:
            return _i16(hmtx, 4 * gid + 2)
        return _i16(hmtx, 4 * self.n_hmetrics + 2 * (gid - self.n_hmetrics))

    def width1000(self, gid):
        return int(round(self.advance(gid) * 1000.0 / self.upm))

    def scale1000(self, v):
        return int(round(v * 1000.0 / self.upm))

    def _read_cmap(self):
        """Unicode -> glyph, from the face's Unicode cmap: format 12 (all of
        Unicode) where there is one, else format 4 (the BMP)."""
        cmap = self.tables[b'cmap']
        subtables = {}
        for i in range(_u16(cmap, 2)):
            plat, enc, off = struct.unpack_from('>HHI', cmap, 4 + 8 * i)
            subtables[(plat, enc, _u16(cmap, off))] = off
        for key in ((3, 10, 12), (0, 4, 12), (0, 6, 12)):
            if key in subtables:
                return self._cmap12(cmap, subtables[key])
        for key in ((3, 1, 4), (0, 3, 4), (0, 1, 4), (0, 0, 4)):
            if key in subtables:
                return self._cmap4(cmap, subtables[key])
        raise ValueError('no Unicode cmap')

    @staticmethod
    def _cmap12(b, off):
        out = {}
        for i in range(_u32(b, off + 12)):
            start, end, gid = struct.unpack_from('>III', b, off + 16 + 12 * i)
            for cp in range(start, end + 1):
                if gid + cp - start:
                    out[cp] = gid + cp - start
        return out

    @staticmethod
    def _cmap4(b, off):
        out = {}
        segs = _u16(b, off + 6) // 2
        ends = off + 14
        starts = ends + 2 * segs + 2
        deltas = starts + 2 * segs
        ranges = deltas + 2 * segs
        for s in range(segs):
            end, start = _u16(b, ends + 2 * s), _u16(b, starts + 2 * s)
            delta, ro = _u16(b, deltas + 2 * s), _u16(b, ranges + 2 * s)
            for cp in range(start, end + 1):
                if cp == 0xFFFF:
                    break
                if ro == 0:
                    gid = (cp + delta) & 0xFFFF
                else:
                    g = _u16(b, ranges + 2 * s + ro + 2 * (cp - start))
                    gid = (g + delta) & 0xFFFF if g else 0
                if gid:
                    out[cp] = gid
        return out

    # -- outlines ------------------------------------------------------

    def _glyph_span(self, gid):
        loca = self.tables[b'loca']
        if self.long_loca:
            a, b = _u32(loca, 4 * gid), _u32(loca, 4 * gid + 4)
        else:
            a, b = 2 * _u16(loca, 2 * gid), 2 * _u16(loca, 2 * gid + 2)
        return a, b

    def _glyph(self, gid):
        a, b = self._glyph_span(gid)
        return self.tables[b'glyf'][a:b]

    def _components(self, g):
        """The glyphs a composite glyph is made of, each with where its
        number sits in g (none for a simple glyph)."""
        out = []
        if len(g) < 10 or _i16(g, 0) >= 0:
            return out
        o = 10
        while True:
            flags, gid = _u16(g, o), _u16(g, o + 2)
            out.append((gid, o + 2))
            o += 4 + (4 if flags & _ARG_WORDS else 2)
            o += 8 if flags & _HAVE_2X2 else 4 if flags & _HAVE_XY_SCALE else 2 if flags & _HAVE_SCALE else 0
            if not flags & _MORE:
                return out

    def subset(self, gids):
        """A face holding glyph 0 and `gids` (and what their composites are
        built of), as TrueType file bytes, and a dict from each kept glyph's
        number in this face to its number in the subset. The kept glyphs
        keep their order; a composite's parts are renumbered with them."""
        keep = {0}
        todo = [g for g in gids if 0 <= g < self.n_glyphs]
        while todo:
            g = todo.pop()
            if g in keep:
                continue
            keep.add(g)
            todo.extend(c for c, _ in self._components(self._glyph(g)) if c < self.n_glyphs)
        order = sorted(keep)
        new = {g: i for i, g in enumerate(order)}
        n = len(order)

        glyf = bytearray()
        loca = []
        for g in order:
            loca.append(len(glyf))
            data = bytearray(self._glyph(g))
            for c, at in self._components(data):
                struct.pack_into('>H', data, at, new.get(c, 0))
            glyf += data + b'\0' * (-len(data) % 4)
        loca.append(len(glyf))

        hmtx = b''.join(struct.pack('>Hh', self.advance(g), self._lsb(g)) for g in order)
        head = bytearray(self.tables[b'head'])
        struct.pack_into('>I', head, 8, 0)          # checkSumAdjustment, set below
        struct.pack_into('>h', head, 50, 1)         # long loca
        hhea = bytearray(self.tables[b'hhea'])
        struct.pack_into('>H', hhea, 34, n)
        maxp = bytearray(self.tables[b'maxp'])
        struct.pack_into('>H', maxp, 4, n)

        tables = {b'head': bytes(head), b'hhea': bytes(hhea), b'maxp': bytes(maxp),
                  b'hmtx': hmtx, b'glyf': bytes(glyf),
                  b'loca': struct.pack('>%dI' % len(loca), *loca)}
        for tag in (b'cvt ', b'fpgm', b'prep'):
            if tag in self.tables:
                tables[tag] = self.tables[tag]

        tags = [t for t in _KEEP if t in tables]
        count = len(tags)
        power = 1
        while power * 2 <= count:
            power *= 2
        search = power * 16
        selector = power.bit_length() - 1
        out = bytearray(struct.pack('>IHHHH', 0x00010000, count, search, selector, count * 16 - search))
        offset = 12 + 16 * count
        body = bytearray()
        head_at = 0
        for tag in tags:
            data = tables[tag]
            if tag == b'head':
                head_at = offset + len(body)
            out += struct.pack('>4sIII', tag, _checksum(data), offset + len(body), len(data))
            body += data + b'\0' * (-len(data) % 4)
        out += body
        struct.pack_into('>I', out, head_at + 8, (0xB1B0AFBA - _checksum(bytes(out))) & 0xFFFFFFFF)
        return bytes(out), new
