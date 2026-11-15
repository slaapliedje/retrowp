RetroWP for DOS
===============

RETROWP.EXE is a plain 16-bit real-mode DOS executable (Open Watcom).
No ROM/BIOS image needed -- runs under any DOS emulator or real DOS.

Quickest way to try it, with DOSBox (https://www.dosbox.com/ or
DOSBox Staging):

  1. Put RETROWP.EXE in an empty folder, e.g. ~/dos_disk/
  2. dosbox
  3. At the DOSBox prompt:
       mount C ~/dos_disk
       C:
       RETROWP.EXE

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
or plain text (DOS shows the text in its own text-mode view rather than Page View).

The spelling checker on the machine reads a dictionary file
(C:\WPSPELL.DIC); the source repo holds only a small sample one, so
for real checking use the offload gateway's large dictionary.

Notes:
- Plain 80x25 text mode -- no graphics mode switch, works on any
  DOSBox machine profile (cga/ega/svga/vga). Hercules mode is a known,
  documented exception (real Hercules text mode uses a different video
  segment this build doesn't target).
- Ctrl+Q quits cleanly. F1 opens help. Vi-style keybindings by default
  -- see docs/keymaps.md in the source repo for the other four schemes.
- Images are shown as a bracketed text marker ("[image: name WxH]"),
  not real pixels -- DOS text mode has no addressable graphics, this
  is an intentional, documented limitation, not a bug.
- Advanced features (grammar/spell/thesaurus/chat/image-insert/ODT/PDF)
  need a separate offload gateway process reachable over a real or
  emulated serial link (this build looks for "serial:0:9600") -- not
  required to try basic editing.
