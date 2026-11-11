RetroWP's clipart
=================

The pieces Tools > Insert Clipart offers: rules, corners, frames,
headings, flourishes and the symbols a game's character sheet wants.
Each is a small Windows metafile (.WMF), which RetroWP reads on every
machine without the gateway and draws as sharp as the screen or printer
it lands on; Word and other word processors read them too.

CLIPART.TXT is the list Insert Clipart shows, one piece a line:

    Category|Title|FILE.WMF

A category holds at most eight pieces (the Atari's chooser takes nine
choices); file names are 8.3 for every machine.

Installing: copy this folder, as CLIPART, beside the program --
RETROWP.PRG on the Atari, RETROWP.EXE on DOS, RetroWP on the Amiga
(PROGDIR:CLIPART) or the Mac (a folder named CLIPART beside the
application).

Making: every piece is drawn by tools/make_clipart.py, by geometry, and
turned into a metafile by the gateway's own SVG converter
(offload/gateway/svg_drawing.py). Run "make clipart" (Python 3) after
changing a design; the .WMF files and the list are kept in the
repository so no machine needs Python.

Licence: the pieces -- the .WMF files and CLIPART.TXT -- are dedicated
to the public domain. Use them in any document, for anything, with no
conditions and no credit needed. Where a dedication to the public domain
is not possible in law, they are offered under CC0 1.0 Universal
(https://creativecommons.org/publicdomain/zero/1.0/). The program that
draws them, tools/make_clipart.py, is part of RetroWP and stays
GPL-2.0-or-later like the rest of it.
