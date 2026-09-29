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
