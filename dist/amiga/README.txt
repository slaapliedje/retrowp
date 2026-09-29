RetroWP for Amiga
=================

RETROWP is a plain AmigaOS executable. You supply the Kickstart ROM
(real hardware doesn't need one) -- this build was tested against
Kickstart 3.1.

Real requirement, not optional: MUI (Magic User Interface) 3.8 or
later must already be installed on whatever Workbench you boot --
it's a third-party library, not bundled with AmigaOS. Get it from
Aminet (aminet.net/util/libs/mui38usr.lha) or use a Workbench image
that already has it. Note: the stock mui38usr.lha installer script has
a real packaging bug (a missing muipack.lzx) -- if Install-MUI fails
partway through, that's why; you may need to unpack the MUI archives
by hand into LIBS:/CLASSES:.

Quickest way to try it, with amiberry (https://github.com/BlitterStudio/amiberry):

  1. Put RETROWP in your Workbench disk/directory (wherever MUI is
     installed).
  2. amiberry --model A500 -r /path/to/kick31.rom -c 4 -F 8 \
              -s use_gui=no

  3. Once booted to Workbench (or a Shell), run RETROWP.

Notes:
- Ctrl+Q quits cleanly. F1 opens help. Vi-style keybindings by default
  -- see docs/keymaps.md in the source repo for the other four schemes.
- Advanced features (grammar/spell/thesaurus/chat/image-insert/ODT/PDF)
  need a separate offload gateway process reachable over a real or
  emulated serial link (this build looks for "serial:aux:9600") --
  not required to try basic editing.
