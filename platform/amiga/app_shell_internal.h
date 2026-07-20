#ifndef WP_AMIGA_APP_SHELL_INTERNAL_H
#define WP_AMIGA_APP_SHELL_INTERNAL_H

#include "types.h"

/* On-target selfcheck surface, mirroring platform/atari/app_shell_internal.h's
   role exactly: functions declared here are exercised by a standalone smoke
   test (host/tools/amiga_smoke.c) cross-compiled and run under amiberry,
   never called from engine/ or services/ code. Grows alongside app_shell.c
   phase by phase (T9.1a's file+clipboard round-trip; T9.2 adds the real
   layout/metrics round-trip below, the same way the Atari backend's own
   selfcheck surface grew). */

wp_bool wp_amiga_file_clipboard_selfcheck(void);

/* T9.2: proves wp_paragraph_relayout/plat_measure_text/plat_font_open
   work correctly on real target hardware -- see app_shell.c's own
   comment on wp_amiga_layout_selfcheck for what it checks and why. */
wp_bool wp_amiga_layout_selfcheck(void);

#endif /* WP_AMIGA_APP_SHELL_INTERNAL_H */
