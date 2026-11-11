# SPDX-License-Identifier: GPL-2.0-or-later
"""SVG -> Windows metafile, for the OFFLOAD_DRAWING op (T16.4).

RetroWP keeps a drawing as a list of primitives and carries it in RTF as
a Windows metafile (engine/src/format_wmf.c), so that is what this hands
back: a placeable metafile -- its header says how many units make an
inch, so the drawing keeps its size -- of polygons and polylines in
pens and brushes. The engine reads it with the same code that reads a
.WMF from disk.

Standard library only (xml.etree), so the gateway needs nothing new.
What is read: <svg> width/height/viewBox; <g> with inherited style;
rect (and rounded), circle, ellipse, line, polyline, polygon, path (M L
H V C S Q T A Z, absolute and relative); transform (matrix, translate,
scale, rotate, skewX, skewY); fill and stroke as #rgb, #rrggbb, rgb(),
the common colour names, or a gradient (its stops' average colour);
fill-rule is even-odd throughout; opacity as a lighter shade;
stroke-width. Curves are flattened to straight segments. A path's
subpaths are filled as one even-odd polygon, joined by seams that cancel
out, so a letter's hole stays a hole; its outline is stroked subpath by
subpath. Not read: text, images, clipping, masks, markers, filters,
<use> -- they are skipped.
"""

import math
import re
import struct
import xml.etree.ElementTree as ET

NAMED = {
    'black': (0, 0, 0), 'white': (255, 255, 255), 'red': (255, 0, 0),
    'green': (0, 128, 0), 'lime': (0, 255, 0), 'blue': (0, 0, 255),
    'yellow': (255, 255, 0), 'cyan': (0, 255, 255), 'aqua': (0, 255, 255),
    'magenta': (255, 0, 255), 'fuchsia': (255, 0, 255), 'gray': (128, 128, 128),
    'grey': (128, 128, 128), 'silver': (192, 192, 192), 'maroon': (128, 0, 0),
    'olive': (128, 128, 0), 'purple': (128, 0, 128), 'teal': (0, 128, 128),
    'navy': (0, 0, 128), 'orange': (255, 165, 0), 'brown': (165, 42, 42),
    'pink': (255, 192, 203), 'gold': (255, 215, 0), 'darkgray': (169, 169, 169),
    'darkgrey': (169, 169, 169), 'lightgray': (211, 211, 211),
    'lightgrey': (211, 211, 211), 'darkred': (139, 0, 0),
    'darkgreen': (0, 100, 0), 'darkblue': (0, 0, 139), 'tan': (210, 180, 140),
    'beige': (245, 245, 220), 'ivory': (255, 255, 240), 'khaki': (240, 230, 140),
    'crimson': (220, 20, 60), 'indigo': (75, 0, 130), 'violet': (238, 130, 238),
    'salmon': (250, 128, 114), 'coral': (255, 127, 80), 'chocolate': (210, 105, 30),
    'sienna': (160, 82, 45), 'steelblue': (70, 130, 180), 'skyblue': (135, 206, 235),
}

UNITS = {'px': 1.0, '': 1.0, 'pt': 96.0 / 72.0, 'pc': 16.0, 'mm': 96.0 / 25.4,
         'cm': 96.0 / 2.54, 'in': 96.0}

NUM = re.compile(r'[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def _local(tag):
    return tag.rsplit('}', 1)[-1]


def _length(text, default=None):
    if text is None:
        return default
    m = re.match(r'\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)\s*([a-z%]*)', text)
    if not m:
        return default
    unit = m.group(2)
    if unit == '%':
        return default
    return float(m.group(1)) * UNITS.get(unit, 1.0)


def _mul(a, b):
    """Affine matrices (a b c d e f), a applied after b."""
    return (a[0] * b[0] + a[2] * b[1], a[1] * b[0] + a[3] * b[1],
            a[0] * b[2] + a[2] * b[3], a[1] * b[2] + a[3] * b[3],
            a[0] * b[4] + a[2] * b[5] + a[4], a[1] * b[4] + a[3] * b[5] + a[5])


IDENTITY = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)


def _transform(text):
    m = IDENTITY
    if not text:
        return m
    for name, args in re.findall(r'(\w+)\s*\(([^)]*)\)', text):
        v = [float(x) for x in NUM.findall(args)]
        t = IDENTITY
        if name == 'matrix' and len(v) == 6:
            t = tuple(v)
        elif name == 'translate' and v:
            t = (1, 0, 0, 1, v[0], v[1] if len(v) > 1 else 0)
        elif name == 'scale' and v:
            t = (v[0], 0, 0, v[1] if len(v) > 1 else v[0], 0, 0)
        elif name == 'rotate' and v:
            a = math.radians(v[0])
            r = (math.cos(a), math.sin(a), -math.sin(a), math.cos(a), 0, 0)
            if len(v) == 3:
                t = _mul((1, 0, 0, 1, v[1], v[2]), _mul(r, (1, 0, 0, 1, -v[1], -v[2])))
            else:
                t = r
        elif name == 'skewX' and v:
            t = (1, 0, math.tan(math.radians(v[0])), 1, 0, 0)
        elif name == 'skewY' and v:
            t = (1, math.tan(math.radians(v[0])), 0, 1, 0, 0)
        m = _mul(m, t)
    return m


def _apply(m, x, y):
    return (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])


class Converter:
    def __init__(self, root):
        self.root = root
        self.gradients = {}
        for el in root.iter():
            if _local(el.tag) in ('linearGradient', 'radialGradient') and el.get('id'):
                self.gradients[el.get('id')] = el
        self.shapes = []   # (fill rgb or None, stroke rgb or None, stroke width, subpaths)
        self.tol = 0.25    # flattening tolerance, in user units scaled to output

    # -- style ------------------------------------------------------------

    def _gradient_rgb(self, gid, depth=0):
        el = self.gradients.get(gid)
        if el is None or depth > 4:
            return None
        stops = [s for s in el if _local(s.tag) == 'stop']
        if not stops:
            ref = el.get('{http://www.w3.org/1999/xlink}href') or el.get('href')
            return self._gradient_rgb(ref[1:], depth + 1) if ref and ref.startswith('#') else None
        cols = []
        for s in stops:
            st = self._style(s, {})
            c = self._color(st.get('stop-color', 'black'))
            if c is not None:
                cols.append(c)
        if not cols:
            return None
        return tuple(sum(c[k] for c in cols) // len(cols) for k in range(3))

    def _color(self, v):
        if v is None:
            return None
        v = v.strip().lower()
        if v in ('none', 'transparent', ''):
            return None
        m = re.match(r'url\(\s*#([^)\s]+)\s*\)', v)
        if m:
            return self._gradient_rgb(m.group(1))
        if v.startswith('#'):
            h = v[1:]
            if len(h) == 3:
                return tuple(int(c * 2, 16) for c in h)
            if len(h) >= 6:
                return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
            return None
        m = re.match(r'rgb\(([^)]*)\)', v)
        if m:
            parts = [p.strip() for p in m.group(1).split(',')]
            out = []
            for p in parts[:3]:
                out.append(int(float(p[:-1]) * 2.55) if p.endswith('%') else int(float(p)))
            return tuple(max(0, min(255, c)) for c in out) if len(out) == 3 else None
        return NAMED.get(v, None if v != 'currentcolor' else (0, 0, 0))

    @staticmethod
    def _style(el, inherited):
        st = dict(inherited)
        for k in ('fill', 'stroke', 'stroke-width', 'opacity', 'fill-opacity',
                  'stroke-opacity', 'stop-color', 'display', 'visibility'):
            if el.get(k) is not None:
                st[k] = el.get(k)
        if el.get('style'):
            for part in el.get('style').split(';'):
                if ':' in part:
                    k, v = part.split(':', 1)
                    st[k.strip()] = v.strip()
        st.pop('opacity_applied', None)
        return st

    @staticmethod
    def _fade(rgb, alpha):
        if rgb is None or alpha >= 0.999:
            return rgb
        return tuple(int(255 - (255 - c) * alpha) for c in rgb)

    # -- geometry ---------------------------------------------------------

    def _flatten_cubic(self, p0, p1, p2, p3, out):
        d = (math.hypot(p1[0] - p0[0], p1[1] - p0[1]) + math.hypot(p2[0] - p1[0], p2[1] - p1[1]) +
             math.hypot(p3[0] - p2[0], p3[1] - p2[1]))
        n = max(2, min(64, int(math.sqrt(d / self.tol) / 2) + 1))
        for i in range(1, n + 1):
            t = i / n
            u = 1 - t
            out.append((u * u * u * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t * t * t * p3[0],
                        u * u * u * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t * t * t * p3[1]))

    def _flatten_arc(self, p0, rx, ry, phi, large, sweep, p1, out):
        """The SVG arc from p0 to p1 (F.6.5, endpoint to centre)."""
        if rx == 0 or ry == 0 or p0 == p1:
            out.append(p1)
            return
        rx, ry = abs(rx), abs(ry)
        c, s = math.cos(math.radians(phi)), math.sin(math.radians(phi))
        dx, dy = (p0[0] - p1[0]) / 2, (p0[1] - p1[1]) / 2
        x1, y1 = c * dx + s * dy, -s * dx + c * dy
        lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry)
        if lam > 1:
            rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
        num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1
        den = rx * rx * y1 * y1 + ry * ry * x1 * x1
        k = math.sqrt(max(0.0, num / den)) if den else 0.0
        if large == sweep:
            k = -k
        cx1, cy1 = k * rx * y1 / ry, -k * ry * x1 / rx
        cx = c * cx1 - s * cy1 + (p0[0] + p1[0]) / 2
        cy = s * cx1 + c * cy1 + (p0[1] + p1[1]) / 2
        a0 = math.atan2((y1 - cy1) / ry, (x1 - cx1) / rx)
        a1 = math.atan2((-y1 - cy1) / ry, (-x1 - cx1) / rx)
        da = a1 - a0
        if sweep and da < 0:
            da += 2 * math.pi
        elif not sweep and da > 0:
            da -= 2 * math.pi
        n = max(2, min(64, int(abs(da) * math.sqrt(max(rx, ry) / self.tol) / 2) + 1))
        for i in range(1, n + 1):
            a = a0 + da * i / n
            ex, ey = rx * math.cos(a), ry * math.sin(a)
            out.append((c * ex - s * ey + cx, s * ex + c * ey + cy))

    def _ellipse(self, cx, cy, rx, ry):
        n = max(12, min(64, int(math.sqrt(max(rx, ry) / self.tol) * 2)))
        return [[(cx + rx * math.cos(2 * math.pi * i / n), cy + ry * math.sin(2 * math.pi * i / n))
                 for i in range(n)]]

    def _path(self, d):
        toks = re.findall(r'[MmLlHhVvCcSsQqTtAaZz]|' + NUM.pattern, d)
        subs, cur = [], []
        x = y = sx = sy = 0.0
        cmd, prev_cmd = None, None
        cpx = cpy = 0.0
        i = 0

        def num():
            nonlocal i
            v = float(toks[i])
            i += 1
            return v

        while i < len(toks):
            if re.match(r'[A-Za-z]', toks[i]):
                cmd = toks[i]
                i += 1
                if cmd in 'Zz':
                    if cur:
                        subs.append((cur, True))
                    cur = []
                    x, y = sx, sy
                    prev_cmd = cmd
                    continue
            if cmd is None:
                break
            rel = cmd.islower()
            c = cmd.upper()
            try:
                if c == 'M':
                    if cur:
                        subs.append((cur, False))
                    nx, ny = num(), num()
                    x, y = (x + nx, y + ny) if rel else (nx, ny)
                    sx, sy = x, y
                    cur = [(x, y)]
                    cmd = 'l' if rel else 'L'
                elif c == 'L':
                    nx, ny = num(), num()
                    x, y = (x + nx, y + ny) if rel else (nx, ny)
                    cur.append((x, y))
                elif c == 'H':
                    nx = num()
                    x = x + nx if rel else nx
                    cur.append((x, y))
                elif c == 'V':
                    ny = num()
                    y = y + ny if rel else ny
                    cur.append((x, y))
                elif c in 'CS':
                    if c == 'C':
                        x1, y1 = num(), num()
                        if rel:
                            x1, y1 = x + x1, y + y1
                    else:
                        if prev_cmd and prev_cmd.upper() in 'CS':
                            x1, y1 = 2 * x - cpx, 2 * y - cpy
                        else:
                            x1, y1 = x, y
                    x2, y2, nx, ny = num(), num(), num(), num()
                    if rel:
                        x2, y2, nx, ny = x + x2, y + y2, x + nx, y + ny
                    if not cur:
                        cur = [(x, y)]
                    self._flatten_cubic((x, y), (x1, y1), (x2, y2), (nx, ny), cur)
                    cpx, cpy = x2, y2
                    x, y = nx, ny
                elif c in 'QT':
                    if c == 'Q':
                        qx, qy = num(), num()
                        if rel:
                            qx, qy = x + qx, y + qy
                    else:
                        if prev_cmd and prev_cmd.upper() in 'QT':
                            qx, qy = 2 * x - cpx, 2 * y - cpy
                        else:
                            qx, qy = x, y
                    nx, ny = num(), num()
                    if rel:
                        nx, ny = x + nx, y + ny
                    if not cur:
                        cur = [(x, y)]
                    self._flatten_cubic((x, y), (x + 2 * (qx - x) / 3, y + 2 * (qy - y) / 3),
                                        (nx + 2 * (qx - nx) / 3, ny + 2 * (qy - ny) / 3), (nx, ny), cur)
                    cpx, cpy = qx, qy
                    x, y = nx, ny
                elif c == 'A':
                    rx, ry, phi = num(), num(), num()
                    large, sweep = num() != 0, num() != 0
                    nx, ny = num(), num()
                    if rel:
                        nx, ny = x + nx, y + ny
                    if not cur:
                        cur = [(x, y)]
                    self._flatten_arc((x, y), rx, ry, phi, large, sweep, (nx, ny), cur)
                    x, y = nx, ny
                else:
                    i += 1
            except (IndexError, ValueError):
                break
            prev_cmd = cmd
        if cur:
            subs.append((cur, False))
        return subs

    # -- the tree ---------------------------------------------------------

    def walk(self, el, m, inherited):
        tag = _local(el.tag)
        if tag in ('defs', 'clipPath', 'mask', 'symbol', 'marker', 'pattern', 'title',
                   'desc', 'metadata', 'linearGradient', 'radialGradient', 'style', 'text',
                   'image', 'use', 'filter'):
            return
        st = self._style(el, inherited)
        if st.get('display') == 'none' or st.get('visibility') == 'hidden':
            return
        m = _mul(m, _transform(el.get('transform')))
        if tag in ('svg', 'g', 'a', 'switch') and el is not self.root:
            alpha = _length(st.get('opacity'), 1.0)
            st['_alpha'] = str(float(st.get('_alpha', 1.0)) * alpha)
            st.pop('opacity', None)
        if tag in ('svg', 'g', 'a', 'switch'):
            for child in el:
                self.walk(child, m, st)
            return
        subs = self._shape(tag, el)
        if not subs:
            return
        alpha = float(st.get('_alpha', 1.0)) * (_length(st.get('opacity'), 1.0) or 1.0)
        fill = self._fade(self._color(st.get('fill', 'black')),
                          alpha * (_length(st.get('fill-opacity'), 1.0) or 1.0))
        stroke = self._fade(self._color(st.get('stroke', 'none')),
                            alpha * (_length(st.get('stroke-opacity'), 1.0) or 1.0))
        scale = math.sqrt(abs(m[0] * m[3] - m[1] * m[2]))
        sw = (_length(st.get('stroke-width'), 1.0) or 0.0) * scale
        mapped = [([_apply(m, px, py) for px, py in pts], closed) for pts, closed in subs]
        if tag in ('line', 'polyline'):
            fill = None
        self.shapes.append((fill, stroke, sw, mapped))

    def _shape(self, tag, el):
        g = lambda k, d=0.0: _length(el.get(k), d)
        if tag == 'rect':
            x, y, w, h = g('x'), g('y'), g('width'), g('height')
            if w <= 0 or h <= 0:
                return None
            rx, ry = el.get('rx'), el.get('ry')
            rx = _length(rx, None) if rx is not None else None
            ry = _length(ry, None) if ry is not None else None
            if rx is None and ry is None:
                return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
            rx = ry if rx is None else rx
            ry = rx if ry is None else ry
            rx, ry = min(rx, w / 2), min(ry, h / 2)
            pts = [(x + rx, y)]
            pts.append((x + w - rx, y))
            self._flatten_arc((x + w - rx, y), rx, ry, 0, False, True, (x + w, y + ry), pts)
            pts.append((x + w, y + h - ry))
            self._flatten_arc((x + w, y + h - ry), rx, ry, 0, False, True, (x + w - rx, y + h), pts)
            pts.append((x + rx, y + h))
            self._flatten_arc((x + rx, y + h), rx, ry, 0, False, True, (x, y + h - ry), pts)
            pts.append((x, y + ry))
            self._flatten_arc((x, y + ry), rx, ry, 0, False, True, (x + rx, y), pts)
            return [(pts, True)]
        if tag == 'circle':
            r = g('r')
            return [(p, True) for p in self._ellipse(g('cx'), g('cy'), r, r)] if r > 0 else None
        if tag == 'ellipse':
            rx, ry = g('rx'), g('ry')
            return [(p, True) for p in self._ellipse(g('cx'), g('cy'), rx, ry)] if rx > 0 and ry > 0 else None
        if tag == 'line':
            return [([(g('x1'), g('y1')), (g('x2'), g('y2'))], False)]
        if tag in ('polyline', 'polygon'):
            v = [float(n) for n in NUM.findall(el.get('points', ''))]
            pts = list(zip(v[0::2], v[1::2]))
            return [(pts, tag == 'polygon')] if len(pts) >= 2 else None
        if tag == 'path':
            return self._path(el.get('d', ''))
        return None


def _viewport(root):
    vb = [float(n) for n in NUM.findall(root.get('viewBox', ''))]
    w = _length(root.get('width'))
    h = _length(root.get('height'))
    if len(vb) == 4 and vb[2] > 0 and vb[3] > 0:
        if w is None and h is None:
            w, h = vb[2], vb[3]
        elif w is None:
            w = h * vb[2] / vb[3]
        elif h is None:
            h = w * vb[3] / vb[2]
        m = (w / vb[2], 0, 0, h / vb[3], -vb[0] * w / vb[2], -vb[1] * h / vb[3])
    else:
        w = w or 300.0
        h = h or 150.0
        m = IDENTITY
    return w, h, m


class _Wmf:
    """A placeable metafile of pens, brushes, polygons and polylines."""

    def __init__(self, ext_w, ext_h, inch):
        self.recs = bytearray()
        self.words = 0
        self.max_rec = 0
        self.ext_w, self.ext_h, self.inch = ext_w, ext_h, inch
        self.slots = [False, False, False]
        self.pen = self.brush = None
        self.pen_slot = self.brush_slot = -1
        self.rec(0x020B, [0, 0])
        self.rec(0x020C, [ext_h, ext_w])

    def rec(self, fn, params):
        size = 3 + len(params)
        self.max_rec = max(self.max_rec, size)
        self.words += size
        self.recs += struct.pack('<IH', size, fn)
        # Coordinates are signed, colour words unsigned: each word kept to
        # its 16 bits either way, a coordinate clamped first.
        self.recs += b''.join(struct.pack('<H', max(-32768, min(65535, int(p))) & 0xFFFF)
                              for p in params)

    def _swap(self, slot, which):
        self.rec(0x012D, [slot])
        cur = self.pen_slot if which == 'pen' else self.brush_slot
        if cur >= 0:
            self.rec(0x01F0, [cur])
            self.slots[cur] = False
        if which == 'pen':
            self.pen_slot = slot
        else:
            self.brush_slot = slot

    def _free(self):
        for i in range(3):
            if not self.slots[i]:
                self.slots[i] = True
                return i
        return 0

    def set_pen(self, rgb, width):
        key = (rgb, width)
        if self.pen == key:
            return
        self.pen = key
        s = self._free()
        r, g, b = rgb or (0, 0, 0)
        self.rec(0x02FA, [5 if rgb is None else 0, width, 0, r | (g << 8), b])
        self._swap(s, 'pen')

    def set_brush(self, rgb):
        if self.brush == ('b', rgb):
            return
        self.brush = ('b', rgb)
        s = self._free()
        r, g, b = rgb or (0, 0, 0)
        self.rec(0x02FC, [1 if rgb is None else 0, r | (g << 8), b, 0])
        self._swap(s, 'brush')

    def poly(self, fn, pts):
        params = [len(pts)]
        for x, y in pts:
            params += [x, y]
        self.rec(fn, params)

    def bytes(self):
        self.rec(0x0000, [])
        total = 9 + self.words
        head = struct.pack('<HHHIHIH', 1, 9, 0x0300, total, 3, self.max_rec, 0)
        place = struct.pack('<IHhhhhHI', 0x9AC6CDD7, 0, 0, 0, self.ext_w, self.ext_h, self.inch, 0)
        checksum = 0
        for k in range(0, 20, 2):
            checksum ^= struct.unpack_from('<H', place, k)[0]
        place += struct.pack('<H', checksum)
        return place + head + bytes(self.recs)


def _emit(conv, w, h, size_px, cap):
    """The shapes as a metafile, the longer side `size_px` units."""
    k = size_px / max(w, h)
    ext_w, ext_h = max(1, int(round(w * k))), max(1, int(round(h * k)))
    out = _Wmf(ext_w, ext_h, int(round(96 * k)))

    def q(pts):
        res = []
        for x, y in pts:
            p = (int(round(x * k)), int(round(y * k)))
            if not res or res[-1] != p:
                res.append(p)
        return res

    for fill, stroke, sw, subs in conv.shapes:
        if fill is not None:
            # One even-odd polygon: the first subpath and back to its start,
            # then each other subpath, back to its own start and then to the
            # first's -- every seam walked once each way, so it cancels.
            joined = []
            for pts, _closed in subs:
                qp = q(pts)
                if len(qp) < 2:
                    continue
                if not joined:
                    joined = qp + [qp[0]]
                else:
                    joined += qp + [qp[0], joined[0]]
            if len(joined) >= 3:
                out.set_pen(None, 0)
                out.set_brush(fill)
                out.poly(0x0324, joined[:16000])
        if stroke is not None:
            out.set_pen(stroke, max(0, int(round(sw * k))))
            for pts, closed in subs:
                qp = q(pts + ([pts[0]] if closed and pts else []))
                if len(qp) >= 2:
                    out.poly(0x0325, qp[:16000])
        if len(out.recs) > cap:
            return None
    return out.bytes()


def svg_to_wmf(data, cap=60000):
    """SVG bytes -> placeable metafile bytes, at most `cap` long. Raises
    ValueError for something that is not an SVG or has nothing to draw."""
    try:
        root = ET.fromstring(data)
    except ET.ParseError as e:
        raise ValueError('not XML: %s' % e)
    if _local(root.tag) != 'svg':
        raise ValueError('not an SVG')
    w, h, m = _viewport(root)
    if w <= 0 or h <= 0:
        raise ValueError('no size')
    for size_px, tol in ((8000, 2.0), (4000, 2.0), (2000, 1.5), (1000, 1.0)):
        conv = Converter(root)
        conv.tol = tol * max(w, h) / size_px
        conv.walk(root, m, {})
        if not conv.shapes:
            raise ValueError('nothing to draw')
        out = _emit(conv, w, h, size_px, cap)
        if out is not None and len(out) <= cap:
            return out
    raise ValueError('too detailed to send')
