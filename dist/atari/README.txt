RetroWP for Atari ST
====================

RETROWP.PRG is a plain TOS executable. You supply the TOS ROM image
(real hardware doesn't need one) -- EmuTOS (free, GPL) works fine and
is what this build was tested against: https://emutos.sourceforge.io/

Quickest way to try it, with Hatari (https://hatari.tuxfamily.org/):

  1. Put RETROWP.PRG in an empty folder, e.g. ~/atari_disk/
  2. hatari --tos /path/to/etos512k.img --harddrive ~/atari_disk \
            --auto RETROWP.PRG

That boots straight into the app with the folder mounted as drive C:.
Without --auto, it just boots to the GEM desktop -- double-click
RETROWP.PRG on drive C: instead.

What it does
------------
Fonts, sizes, bold/italic/underline, superscript and subscript; named
paragraph styles; footnotes; find and replace; multi-level undo.
Page View shows the page as it will print, with zoom and a layout grid,
newspaper columns, tables, borders and shading, frames, vector drawings,
pictures and the bundled clipart (Tools > Insert Clipart reads the
CLIPART folder beside the program -- keep it there). Paper sizes,
envelopes, CD/DVD labels and label sheets; mail merge into letters,
labels or straight to the printer. RTF in and out, Atari Works (.STW),
WordStar, Markdown and plain text. Printing in PostScript, Epson, PCL
or plain text, and on the ST through GDOS (SpeedoGDOS, NVDI) in its fonts.

The spelling checker on the machine reads a dictionary file
(C:\WPSPELL.DIC); the source repo holds only a small sample one, so
for real checking use the offload gateway's large dictionary.

Notes:
- Runs in ST Medium (640x200, 4 colours) and ST High (640x400, mono);
  1 MB on plain TOS is the baseline.
- Ctrl+Q quits cleanly. F1 opens help. Vi-style keybindings by default
  (i/Esc/j/k) -- see docs/keymaps.md in the source
  repo for the other four schemes (Emacs/WordStar/WordPerfect/MS Word).
- Advanced features (grammar/spell/thesaurus/chat/image-insert/ODT/PDF)
  need a separate offload gateway process reachable over a real or
  emulated serial link -- not required to try basic editing.
