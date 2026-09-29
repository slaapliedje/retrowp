RetroWP for the Atari 8-bit (gem4xe)
====================================

RETROWP.G4A is a GEM application for gem4xe -- a port of GEM (the VDI
and the AES) to an Atari XL/XE. It is the same word processor as the ST
build, from the same source: the application shell is unchanged between
them, and what differs is the CPU and the binding of the same calls.

WHAT YOU NEED

This is not a stock 800XL program. gem4xe requires:

  - a 65C816 accelerator (Rapidus)
  - VBXE, for the 640x240 16-colour display
  - Ultimate 1MB
  - SpartaDOS X, or SpartaDOS 3.2 from a disk

gem4xe itself (GEM.COM) is a separate download:
https://github.com/slaapliedje/gem4xe

RUNNING IT UNDER ALTIRRA

Stage the program as DESKTOP.G4A on a SpartaDOS disk alongside GEM.COM,
using gem4xe's own tool:

  python3 tools/mkspdisk.py <sparta.atr> gem.xex out.atr \
      --sectors 4096 --name GEM.COM --boot GEM \
      --add RETROWP.G4A DESKTOP.G4A

then boot it with the SpartaDOS X cartridge:

  AltirraSDL --pal --hardware 800xl --nobasic \
      --disk out.atr --cart SDX450_maxflash1.car

A 200 KB program takes a few thousand frames to load from a floppy --
that is the medium, not a hang.

NOTES

- Verified under emulation only, never on real hardware.
- The offload features (grammar, spell, thesaurus, chat, ODT/PDF,
  image insert) need a separate gateway over a serial link and are not
  required for editing.
- Vi-style keybindings by default; four other schemes are described in
  the source repository's docs/keymaps.md.

See docs/gem4xe-port.md in the source repository for how this port
works and which gem4xe kit a given build used -- the kit's licence
changed on 2026-09-28, so the terms in NOTICE.txt depend on when the
binary was built.
