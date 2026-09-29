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

Notes:
- Boots into ST Medium resolution (640x200, 4 colors) -- this is the
  project's real target resolution, not a display bug.
- Ctrl+Q quits cleanly. F1 opens help. Vi-style keybindings by default
  (i/Esc/j/k like the screenshot) -- see docs/keymaps.md in the source
  repo for the other four schemes (Emacs/WordStar/WordPerfect/MS Word).
- Advanced features (grammar/spell/thesaurus/chat/image-insert/ODT/PDF)
  need a separate offload gateway process reachable over a real or
  emulated serial link -- not required to try basic editing.
