#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Writes host/tests/fixtures/CHARSHT.RTF, the worked example (T16.6).

An original character sheet -- not any published game's -- that uses
everything M16 built: a decorated title (a clipart banner drawing, with
the title set over it in a frame), a bordered table of who the character
is, the attributes and skills side by side in one table with a shaded
head row, a boxed equipment section, ruled lines to write the background
on, and a closing flourish. It is plain RTF of the kind Word writes and
reads; the drawings are the bundled clipart's Windows metafiles, carried
as Word carries them (\\pict\\wmetafile8, the placeable header dropped
and its size given as \\picwgoal/\\pichgoal).

The gates open it on every machine, screenshot it and print it (docs/
milestones.md, T16.6). Run this again after changing it:

    python3 host/tests/fixtures/make_charsheet.py
"""

import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CLIP = os.path.join(ROOT, 'clipart')

COL = 12240 - 2 * 1080          # Letter, three-quarter-inch margins: 10080 twips


def pict(name, width_twips=None):
    """A clipart metafile as an RTF picture, at its own size or scaled to
    width_twips."""
    data = open(os.path.join(CLIP, name), 'rb').read()
    key, _hmf, left, top, right, bottom, inch = struct.unpack_from('<IHhhhhH', data, 0)
    assert key == 0x9AC6CDD7
    w = (right - left) * 1440 // inch
    h = (bottom - top) * 1440 // inch
    if width_twips:
        h = h * width_twips // w
        w = width_twips
    body = data[22:]
    hexs = body.hex()
    lines = '\r\n'.join(hexs[i:i + 128] for i in range(0, len(hexs), 128))
    return ('{\\pict\\wmetafile8\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d\r\n%s}'
            % (w * 2540 // 1440, h * 2540 // 1440, w, h, lines))


def row(cells, edges, borders=True, shade=None, bold=False, gap_cells=()):
    """A table row: cells' texts, their right edges, each cell boxed (but
    the gap cells), optionally shaded."""
    out = ['\\trowd\\trgaph80\\trleft0']
    for i, e in enumerate(edges):
        if borders and i not in gap_cells:
            out.append('\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10'
                       '\\clbrdrb\\brdrs\\brdrw10\\clbrdrr\\brdrs\\brdrw10')
        if shade and i not in gap_cells:
            out.append('\\clshdng%d' % shade)
        out.append('\\cellx%d' % e)
    out.append('\r\n')
    for c in cells:
        t = ('{\\b %s}' % c) if (bold and c) else c
        out.append('\\pard\\intbl %s\\cell ' % t)
    out.append('\\row\r\n')
    return ''.join(out)


def heading(text):
    return ('\\pard\\sb200\\sa60\\brdrb\\brdrth\\brdrw15\\brsp20 {\\b\\fs26 %s}\\par\r\n' % text)


def main():
    rtf = []
    rtf.append('{\\rtf1\\ansi\\ansicpg1252\\deff0'
               '{\\fonttbl{\\f0\\froman Times New Roman;}{\\f1\\fswiss Helvetica;}}\r\n'
               '\\paperw12240\\paperh15840\\margl1080\\margr1080\\margt1080\\margb1080\r\n')

    # The title: the banner, then the words over it in a frame.
    rtf.append('\\pard\\qc ' + pict('HEADRIBN.WMF', 8640) + '\\par\r\n')
    rtf.append('\\pard\\phmrg\\pvpara\\posx1440\\posy-1290\\absw7200\\qc'
               '{\\b\\fs36 ADVENTURER\\\'92S RECORD}\\par\r\n')

    # Who the character is.
    e4 = [2520, 5040, 7560, COL]
    rtf.append('\\pard\\sb120\\par\r\n')
    rtf.append(row(['Name:', '', 'Player:', ''], e4))
    rtf.append(row(['Class:', '', 'Level:', ''], e4))

    # Attributes and skills, side by side in one table, a gap between.
    rtf.append(heading('ATTRIBUTES AND SKILLS'))
    e7 = [1800, 2700, 3600, 3960, 6480, 7560, COL]
    gap = (3,)
    rtf.append(row(['Attribute', 'Score', 'Mod', '', 'Skill', 'Rank', 'Notes'], e7,
                   shade=2500, bold=True, gap_cells=gap))
    attrs = ['Strength', 'Dexterity', 'Constitution', 'Intellect', 'Wisdom', 'Charisma',
             'Hit Points', 'Armour']
    skills = ['Climbing', 'Lore', 'Riding', 'Stealth', 'Swimming', 'Tracking', 'Weapons', '']
    for a, s in zip(attrs, skills):
        rtf.append(row([a, '', '', '', s, '', ''], e7, gap_cells=gap))

    # Equipment: a box of four lines.
    rtf.append(heading('EQUIPMENT'))
    for i in range(4):
        rtf.append('\\pard\\li120\\ri120\\box\\brdrs\\brdrw10\\brsp60 %s\\par\r\n'
                   % ('Weapons, armour, coin and kit:' if i == 0 else ''))

    # Background: ruled lines to write on -- a one-column table, each row
    # ruled below. (Paragraphs alike in their borders are one group, in
    # Word as here, with one rule under the lot.)
    rtf.append(heading('BACKGROUND'))
    for i in range(5):
        rtf.append('\\trowd\\trgaph0\\trleft0\\clbrdrb\\brdrs\\brdrw5\\cellx%d\r\n'
                   '\\pard\\intbl\\sb200 \\cell \\row\r\n' % COL)

    # A closing flourish.
    rtf.append('\\pard\\qc\\sb240 ' + pict('SECTMARK.WMF', 1440) + '\\par\r\n')
    rtf.append('}\r\n')
    with open(os.path.join(HERE, 'CHARSHT.RTF'), 'w', newline='') as f:
        f.write(''.join(rtf))
    print('CHARSHT.RTF', sum(len(x) for x in rtf), 'bytes')


if __name__ == '__main__':
    main()
