#ifndef WP_AMIGA_APP_SHELL_INTERNAL_H
#define WP_AMIGA_APP_SHELL_INTERNAL_H

#include "types.h"

/* On-target selfcheck surface, mirroring platform/atari/app_shell_internal.h's
   role exactly: functions declared here are exercised by a standalone smoke
   test (host/tools/amiga_smoke.c) cross-compiled and run under amiberry,
   never called from engine/ or services/ code. Grows alongside app_shell.c
   phase by phase (T9.1a's file+clipboard round-trip today; T9.1b will add
   the MUI shell's own selfchecks the same way the Atari backend did). */

wp_bool wp_amiga_file_clipboard_selfcheck(void);

#endif /* WP_AMIGA_APP_SHELL_INTERNAL_H */
