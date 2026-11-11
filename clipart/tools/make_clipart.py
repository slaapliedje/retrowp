#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Makes RetroWP's bundled clipart (T16.5): clipart/*.WMF and CLIPART.TXT.

Every piece is drawn here, by geometry, so the set is RetroWP's own --
rules, corners, frames, headings, flourishes and the symbols a game's
character sheet wants. Each is written as SVG, then turned into a
placeable Windows metafile by offload/gateway/svg_drawing.py (the
gateway's own converter), which every RetroWP machine reads without the
gateway. The .WMF files and the index are committed, so no machine needs
Python; run this again after changing a design:

    python3 clipart/tools/make_clipart.py

The pieces this makes are dedicated to the public domain (CC0 1.0 where
that is not possible; clipart/README.txt). This program itself is
GPL-2.0-or-later, as the line above says.

CLIPART.TXT: one piece a line, "Category|Title|FILE.WMF", in the order
Insert Clipart shows them. Names are 8.3 for every machine's file system;
a category holds at most 8 pieces (the ST's chooser takes 9 choices).
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'offload', 'gateway'))
from svg_drawing import svg_to_wmf  # noqa: E402

INK = '#000'
GREY = '#999'
LIGHT = '#ddd'


def svg(w_in, h_in, vb_w, vb_h, body):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%gin" height="%gin" '
            'viewBox="0 0 %g %g">%s</svg>' % (w_in, h_in, vb_w, vb_h, body)).encode()


def pts(points):
    return ' '.join('%.2f,%.2f' % p for p in points)


def circle_pts(cx, cy, r, n=24, a0=0.0):
    return [(cx + r * math.cos(a0 + 2 * math.pi * i / n), cy + r * math.sin(a0 + 2 * math.pi * i / n))
            for i in range(n)]


# ---- Rules: 6 x 0.5 in, viewBox 600 x 50 --------------------------------

def rule_diamond():
    b = ('<line x1="10" y1="22" x2="270" y2="22" stroke="%s" stroke-width="2"/>'
         '<line x1="10" y1="28" x2="270" y2="28" stroke="%s" stroke-width="2"/>'
         '<line x1="330" y1="22" x2="590" y2="22" stroke="%s" stroke-width="2"/>'
         '<line x1="330" y1="28" x2="590" y2="28" stroke="%s" stroke-width="2"/>'
         '<polygon points="300,8 320,25 300,42 280,25" fill="%s"/>'
         '<polygon points="300,16 310,25 300,34 290,25" fill="#fff"/>') % ((INK,) * 5)
    return svg(6, 0.5, 600, 50, b)


def rule_beads():
    b = '<line x1="10" y1="25" x2="590" y2="25" stroke="%s" stroke-width="1.5"/>' % INK
    for i in range(13):
        x = 30 + i * 45
        r = 7 if i % 2 == 0 else 4
        b += '<circle cx="%d" cy="25" r="%d" fill="%s"/>' % (x, r, INK)
    return svg(6, 0.5, 600, 50, b)


def rule_swell():
    top = [(10 + i * 580 / 60, 25 - 9 * math.sin(math.pi * i / 60)) for i in range(61)]
    bot = [(x, 50 - y) for x, y in reversed(top)]
    return svg(6, 0.5, 600, 50, '<polygon points="%s" fill="%s"/>' % (pts(top + bot), INK))


def rule_wave():
    p = [(10 + i * 2, 25 + 10 * math.sin(i * 2 * math.pi / 40)) for i in range(291)]
    return svg(6, 0.5, 600, 50, '<polyline points="%s" fill="none" stroke="%s" stroke-width="3"/>' % (pts(p), INK))


def rule_rope():
    b = ''
    for k in range(2):
        p = [(10 + i * 2, 25 + 9 * math.sin(i * 2 * math.pi / 30 + k * math.pi)) for i in range(291)]
        b += '<polyline points="%s" fill="none" stroke="%s" stroke-width="3"/>' % (pts(p), INK)
    b += '<line x1="10" y1="12" x2="590" y2="12" stroke="%s" stroke-width="1"/>' % INK
    b += '<line x1="10" y1="38" x2="590" y2="38" stroke="%s" stroke-width="1"/>' % INK
    return svg(6, 0.5, 600, 50, b)


# ---- Corners: 1 x 1 in, viewBox 100 x 100, the top-left corner ----------

def corner_simple():
    b = ('<polyline points="10,90 10,10 90,10" fill="none" stroke="%s" stroke-width="4"/>'
         '<polyline points="20,90 20,20 90,20" fill="none" stroke="%s" stroke-width="1.5"/>'
         '<circle cx="30" cy="30" r="5" fill="%s"/>') % (INK, INK, INK)
    return svg(1, 1, 100, 100, b)


def corner_scroll():
    # Two rules meeting at the corner, each ending in a curl turned inward.
    def curl(cx, cy, r0, sign_x, sign_y, a_start):
        p = []
        for i in range(40):
            a = a_start + i * 0.16
            r = r0 * (1 - i / 48)
            p.append((cx + sign_x * r * math.cos(a), cy + sign_y * r * math.sin(a)))
        return p
    top = [(95, 12), (40, 12)] + curl(40, 26, 14, 1, 1, -math.pi / 2)
    left = [(12, 95), (12, 40)] + curl(26, 40, 14, 1, 1, math.pi)
    b = ('<polyline points="%s" fill="none" stroke="%s" stroke-width="4"/>'
         '<polyline points="%s" fill="none" stroke="%s" stroke-width="4"/>'
         '<circle cx="14" cy="14" r="6" fill="%s"/>') % (pts(top), INK, pts(left), INK, INK)
    return svg(1, 1, 100, 100, b)


def corner_bracket():
    b = ('<path d="M 10 95 L 10 30 Q 10 10 30 10 L 95 10" fill="none" stroke="%s" stroke-width="5"/>'
         '<path d="M 22 95 L 22 36 Q 22 22 36 22 L 95 22" fill="none" stroke="%s" stroke-width="1.5"/>'
         '<polygon points="10,10 26,10 10,26" fill="%s"/>') % (INK, INK, INK)
    return svg(1, 1, 100, 100, b)


def corner_knot():
    b = ''
    for (cx, cy) in ((30, 30), (30, 62), (62, 30)):
        b += '<circle cx="%d" cy="%d" r="16" fill="none" stroke="%s" stroke-width="5"/>' % (cx, cy, INK)
        b += '<circle cx="%d" cy="%d" r="16" fill="none" stroke="#fff" stroke-width="1.5"/>' % (cx, cy)
    b += '<line x1="62" y1="14" x2="98" y2="14" stroke="%s" stroke-width="3"/>' % INK
    b += '<line x1="14" y1="62" x2="14" y2="98" stroke="%s" stroke-width="3"/>' % INK
    return svg(1, 1, 100, 100, b)


# ---- Frames: 6 x 4 in, viewBox 600 x 400 --------------------------------

def frame_double():
    b = ('<rect x="10" y="10" width="580" height="380" fill="none" stroke="%s" stroke-width="5"/>'
         '<rect x="22" y="22" width="556" height="356" fill="none" stroke="%s" stroke-width="1.5"/>') % (INK, INK)
    for (x, y) in ((4, 4), (576, 4), (4, 376), (576, 376)):
        b += '<rect x="%d" y="%d" width="20" height="20" fill="%s"/>' % (x, y, INK)
    return svg(6, 4, 600, 400, b)


def frame_notched():
    d = 'M 40 10 L 560 10 L 560 40 L 590 40 L 590 360 L 560 360 L 560 390 L 40 390 L 40 360 L 10 360 L 10 40 L 40 40 Z'
    d2 = 'M 46 18 L 554 18 L 554 46 L 582 46 L 582 354 L 554 354 L 554 382 L 46 382 L 46 354 L 18 354 L 18 46 L 46 46 Z'
    b = ('<path d="%s" fill="none" stroke="%s" stroke-width="4"/>'
         '<path d="%s" fill="none" stroke="%s" stroke-width="1.5"/>') % (d, INK, d2, INK)
    return svg(6, 4, 600, 400, b)


def frame_scallop():
    p = []
    def edge(x0, y0, x1, y1, n):
        for i in range(n):
            ax, ay = x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n
            bx, by = x0 + (x1 - x0) * (i + 1) / n, y0 + (y1 - y0) * (i + 1) / n
            mx, my = (ax + bx) / 2, (ay + by) / 2
            nx, ny = -(by - ay), (bx - ax)          # inward, for a clockwise walk
            l = math.hypot(nx, ny) or 1
            for k in range(8):
                t = k / 8
                bump = math.sin(math.pi * t) * 10
                p.append((ax + (bx - ax) * t + nx / l * bump, ay + (by - ay) * t + ny / l * bump))
    edge(15, 15, 585, 15, 19)
    edge(585, 15, 585, 385, 12)
    edge(585, 385, 15, 385, 19)
    edge(15, 385, 15, 15, 12)
    b = ('<polygon points="%s" fill="none" stroke="%s" stroke-width="3"/>'
         '<rect x="35" y="35" width="530" height="330" fill="none" stroke="%s" stroke-width="1"/>') % (pts(p), INK, INK)
    return svg(6, 4, 600, 400, b)


# ---- Headings: 5 x 1.2 in, viewBox 500 x 120 ----------------------------

def head_ribbon():
    b = ('<polygon points="10,40 70,40 70,100 10,100 30,70" fill="%s"/>'
         '<polygon points="490,40 430,40 430,100 490,100 470,70" fill="%s"/>'
         '<polygon points="50,25 450,25 450,90 50,90" fill="#fff" stroke="%s" stroke-width="4"/>'
         '<polygon points="50,90 70,100 70,90" fill="%s"/>'
         '<polygon points="450,90 430,100 430,90" fill="%s"/>'
         '<line x1="62" y1="35" x2="438" y2="35" stroke="%s" stroke-width="1"/>'
         '<line x1="62" y1="80" x2="438" y2="80" stroke="%s" stroke-width="1"/>') % (
        GREY, GREY, INK, INK, INK, INK, INK)
    return svg(5, 1.2, 500, 120, b)


def head_cartouche():
    b = ('<ellipse cx="250" cy="60" rx="200" ry="45" fill="#fff" stroke="%s" stroke-width="4"/>'
         '<ellipse cx="250" cy="60" rx="188" ry="36" fill="none" stroke="%s" stroke-width="1.5"/>'
         '<polygon points="10,60 50,40 50,80" fill="%s"/>'
         '<polygon points="490,60 450,40 450,80" fill="%s"/>') % (INK, INK, INK, INK)
    return svg(5, 1.2, 500, 120, b)


def head_plaque():
    d = 'M 30 15 L 470 15 Q 470 35 490 35 L 490 85 Q 470 85 470 105 L 30 105 Q 30 85 10 85 L 10 35 Q 30 35 30 15 Z'
    b = ('<path d="%s" fill="%s" stroke="%s" stroke-width="4"/>'
         '<rect x="45" y="28" width="410" height="64" fill="#fff" stroke="%s" stroke-width="1.5"/>') % (
        d, LIGHT, INK, INK)
    return svg(5, 1.2, 500, 120, b)


# ---- Flourishes ----------------------------------------------------------

def fleuron():
    # A heart-shaped leaf on a curling stem: 1.2 x 1 in.
    b = ('<path d="M 60 85 C 20 60 10 35 30 22 C 45 12 58 25 60 35 C 62 25 75 12 90 22 '
         'C 110 35 100 60 60 85 Z" fill="%s"/>'
         '<path d="M 60 80 C 60 60 60 50 60 38" fill="none" stroke="#fff" stroke-width="2"/>'
         '<path d="M 60 85 C 60 95 75 98 85 92 C 95 86 92 76 84 78" fill="none" stroke="%s" stroke-width="3"/>') % (
        INK, INK)
    return svg(1.2, 1, 120, 100, b)


def section_mark():
    b = ''
    for cx in (40, 100, 160):
        s = 18 if cx == 100 else 12
        b += '<polygon points="%d,%d %d,30 %d,%d %d,30" fill="%s"/>' % (cx, 30 - s, cx + s, cx, 30 + s, cx - s, INK)
    return svg(2, 0.6, 200, 60, b)


def s_scroll():
    b = ('<path d="M 20 50 C 20 20 60 20 60 40 C 60 55 40 55 40 45" fill="none" stroke="%s" stroke-width="4"/>'
         '<path d="M 60 40 C 80 70 120 30 140 60" fill="none" stroke="%s" stroke-width="4"/>'
         '<path d="M 180 50 C 180 80 140 80 140 60 C 140 45 160 45 160 55" fill="none" stroke="%s" stroke-width="4"/>'
         '<circle cx="100" cy="50" r="5" fill="%s"/>') % (INK, INK, INK, INK)
    return svg(2, 0.5, 200, 100, b)


# ---- Game symbols: 1 x 1 in, viewBox 100 x 100 --------------------------

def d20():
    hexa = [(50 + 44 * math.cos(math.radians(90 + 60 * i)), 52 - 44 * math.sin(math.radians(90 + 60 * i)))
            for i in range(6)]
    tri = [(50, 22), (76, 66), (24, 66)]
    b = '<polygon points="%s" fill="#fff" stroke="%s" stroke-width="4"/>' % (pts(hexa), INK)
    b += '<polygon points="%s" fill="%s" stroke="%s" stroke-width="2"/>' % (pts(tri), LIGHT, INK)
    for (a, c) in ((0, 0), (1, 1), (2, 2), (3, 2), (4, 1), (5, 0)):
        hx, hy = hexa[a]
        tx, ty = tri[c]
        b += '<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" stroke-width="2"/>' % (hx, hy, tx, ty, INK)
    return svg(1, 1, 100, 100, b)


def d6():
    top = [(50, 10), (88, 28), (50, 46), (12, 28)]
    left = [(12, 28), (50, 46), (50, 92), (12, 74)]
    right = [(50, 46), (88, 28), (88, 74), (50, 92)]
    b = '<polygon points="%s" fill="#fff" stroke="%s" stroke-width="3"/>' % (pts(top), INK)
    b += '<polygon points="%s" fill="%s" stroke="%s" stroke-width="3"/>' % (pts(left), LIGHT, INK)
    b += '<polygon points="%s" fill="%s" stroke="%s" stroke-width="3"/>' % (pts(right), GREY, INK)
    b += '<ellipse cx="50" cy="28" rx="6" ry="3.5" fill="%s"/>' % INK
    for (x, y) in ((24, 46), (38, 74)):
        b += '<ellipse cx="%d" cy="%d" rx="3.5" ry="5" fill="%s"/>' % (x, y, INK)
    for (x, y) in ((62, 52), (69, 63), (76, 74), (62, 75), (76, 51)):
        b += '<ellipse cx="%d" cy="%d" rx="3.5" ry="5" fill="#fff"/>' % (x, y)
    return svg(1, 1, 100, 100, b)


def swords():
    def sword(angle):
        blade = '<polygon points="47,8 53,8 53,62 50,70 47,62" fill="%s" stroke="%s" stroke-width="1.5"/>' % (LIGHT, INK)
        guard = '<rect x="36" y="62" width="28" height="6" fill="%s"/>' % INK
        grip = '<rect x="47" y="68" width="6" height="16" fill="%s"/>' % GREY
        pommel = '<circle cx="50" cy="88" r="5" fill="%s"/>' % INK
        return '<g transform="rotate(%d 50 50)">%s%s%s%s</g>' % (angle, blade, guard, grip, pommel)
    # Each drawn point-up, then leant about the middle, crossing there.
    return svg(1, 1, 100, 100, sword(35) + sword(-35))


def shield():
    d = 'M 15 12 L 85 12 L 85 45 C 85 70 65 85 50 95 C 35 85 15 70 15 45 Z'
    b = ('<path d="%s" fill="#fff" stroke="%s" stroke-width="5"/>'
         '<path d="M 15 12 L 85 12 L 85 30 L 15 70 Z" fill="%s"/>'
         '<path d="%s" fill="none" stroke="%s" stroke-width="5"/>') % (d, INK, GREY, d, INK)
    return svg(1, 1, 100, 100, b)


def potion():
    d = ('M 42 10 L 58 10 L 58 18 L 55 18 L 55 38 C 75 46 84 60 80 75 C 76 90 62 95 50 95 '
         'C 38 95 24 90 20 75 C 16 60 25 46 45 38 L 45 18 L 42 18 Z')
    liquid = 'M 21 70 C 30 64 40 76 50 70 C 60 64 70 76 79 70 C 78 88 64 94 50 94 C 36 94 22 88 21 70 Z'
    b = ('<path d="%s" fill="#fff" stroke="%s" stroke-width="3"/>'
         '<path d="%s" fill="%s"/>'
         '<rect x="44" y="4" width="12" height="7" fill="%s"/>'
         '<circle cx="38" cy="58" r="4" fill="#fff" stroke="%s" stroke-width="1.5"/>') % (d, INK, liquid, GREY, INK, INK)
    return svg(1, 1, 100, 100, b)


def parchment():
    b = ('<path d="M 20 20 L 80 20 L 80 82 L 20 82 Z" fill="%s" stroke="%s" stroke-width="2.5"/>'
         '<ellipse cx="50" cy="20" rx="34" ry="7" fill="#fff" stroke="%s" stroke-width="3"/>'
         '<ellipse cx="50" cy="82" rx="34" ry="7" fill="#fff" stroke="%s" stroke-width="3"/>') % (LIGHT, INK, INK, INK)
    for y in (36, 46, 56, 66):
        b += '<line x1="30" y1="%d" x2="70" y2="%d" stroke="%s" stroke-width="2"/>' % (y, y, INK)
    return svg(1, 1, 100, 100, b)


PIECES = [
    ('Rules', 'Double rule, diamond', 'RULEDIAM', rule_diamond),
    ('Rules', 'Beads', 'RULEBEAD', rule_beads),
    ('Rules', 'Swelled rule', 'RULESWEL', rule_swell),
    ('Rules', 'Wave', 'RULEWAVE', rule_wave),
    ('Rules', 'Rope', 'RULEROPE', rule_rope),
    ('Corners', 'Plain corner', 'CORNPLAN', corner_simple),
    ('Corners', 'Scroll corner', 'CORNSCRL', corner_scroll),
    ('Corners', 'Bracket corner', 'CORNBRKT', corner_bracket),
    ('Corners', 'Knot corner', 'CORNKNOT', corner_knot),
    ('Frames', 'Double frame', 'FRAMEDBL', frame_double),
    ('Frames', 'Notched frame', 'FRAMENOT', frame_notched),
    ('Frames', 'Scalloped frame', 'FRAMESCL', frame_scallop),
    ('Headings', 'Ribbon banner', 'HEADRIBN', head_ribbon),
    ('Headings', 'Cartouche', 'HEADCART', head_cartouche),
    ('Headings', 'Plaque', 'HEADPLAQ', head_plaque),
    ('Flourishes', 'Fleuron', 'FLEURON', fleuron),
    ('Flourishes', 'Section mark', 'SECTMARK', section_mark),
    ('Flourishes', 'S scroll', 'SSCROLL', s_scroll),
    ('Game symbols', 'Twenty-sided die', 'DICED20', d20),
    ('Game symbols', 'Six-sided die', 'DICED6', d6),
    ('Game symbols', 'Crossed swords', 'SWORDS', swords),
    ('Game symbols', 'Shield', 'SHIELD', shield),
    ('Game symbols', 'Potion', 'POTION', potion),
    ('Game symbols', 'Scroll', 'PARCHMNT', parchment),
]


def main():
    out = os.path.join(ROOT, 'clipart')
    lines = []
    seen = {}
    for cat, title, name, fn in PIECES:
        assert len(name) <= 8 and name.isupper() and name.isalnum(), name
        seen[cat] = seen.get(cat, 0) + 1
        assert seen[cat] <= 8, cat
        assert len(cat) < 32 and len(title) < 32, title
        data = svg_to_wmf(fn())
        with open(os.path.join(out, name + '.WMF'), 'wb') as f:
            f.write(data)
        lines.append('%s|%s|%s.WMF' % (cat, title, name))
        print('%-12s %-24s %s.WMF %6d bytes' % (cat, title, name, len(data)))
    with open(os.path.join(out, 'CLIPART.TXT'), 'w', newline='\r\n') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
