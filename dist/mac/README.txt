RetroWP for Mac Classic
=======================

Two copies of the same app, in two formats:

  RETROWP.dsk          -- an 800K HFS floppy disk image with RETROWP
                          already on it. Easiest option: just mount
                          this as a floppy.
  RETROWP.macbinary.bin -- the same app encoded as a single MacBinary
                          file (data fork + resource fork + Finder
                          info combined), for copying onto an existing
                          disk image via a tool that understands
                          MacBinary.

You supply the Mac ROM image (real hardware doesn't need one) -- this
build was tested against a Quadra 650-class ROM (modelid 14) and a
Mac IIci ROM (modelid 5); ROMs aren't legally redistributable, so
you'll need to dump one from real hardware or otherwise source one
yourself.

Quickest way to try it, with BasiliskII (https://basilisk.cebix.net/
or the SheepShaver/macemu-mon fork):

  1. In BasiliskII's settings (or a prefs file), set:
       disk <your existing boot disk image>
       disk RETROWP.dsk
       rom /path/to/your.rom
       modelid 14
       cpu 4
  2. Boot. RETROWP.dsk appears as a second floppy on the desktop --
     open it and double-click RETROWP.

What it does
------------
Fonts, sizes, bold/italic/underline, superscript and subscript; named
paragraph styles; footnotes; find and replace; multi-level undo.
Page View shows the page as it will print, with zoom and a layout grid,
newspaper columns, tables, borders and shading, frames, vector drawings,
pictures and the bundled clipart. Paper sizes, envelopes, CD/DVD labels
and label sheets; mail merge into letters, labels or straight to the
printer. RTF in and out, Atari Works (.STW), WordStar, Markdown and
plain text. Printing in PostScript, Epson, PCL or plain text.

Tools > Insert Clipart looks for a CLIPART folder next to the
application. RETROWP.dsk does not carry one: copy the CLIPART folder
from this archive onto the same disk as RETROWP (with a tool that
writes HFS images, or from inside the emulated Mac).

The spelling checker on the machine reads WPSPELL.DIC next to the
application; the source repo holds only a small sample one, so for
real checking use the offload gateway's large dictionary.

Notes:
- Needs a real System 6.x/7-era boot disk to boot INTO -- RETROWP.dsk
  is just the app, not a bootable system. Mini vMac or SheepShaver
  should also work with this same image.
- Ctrl+Q (or Cmd+Q) quits cleanly. Vi-style keybindings by default --
  see docs/keymaps.md in the source repo for the other four schemes.
- Advanced features (grammar/spell/thesaurus/chat/image-insert/ODT/PDF)
  need a separate offload gateway process reachable over a real or
  emulated serial link (this build looks for "serial:modem:9600") --
  not required to try basic editing.
