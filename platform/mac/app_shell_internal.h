#ifndef WP_MAC_APP_SHELL_INTERNAL_H
#define WP_MAC_APP_SHELL_INTERNAL_H

#include "types.h"

/* On-target selfcheck surface, mirroring platform/atari/app_shell_internal.h
   and platform/amiga/app_shell_internal.h exactly: functions declared here
   are exercised by a standalone smoke test (host/tools/mac/smoke.c)
   cross-compiled and run under BasiliskII, never called from engine/ or
   services/ code. Grows alongside app_shell.c phase by phase. */

wp_bool wp_mac_file_clipboard_selfcheck(void);

#endif /* WP_MAC_APP_SHELL_INTERNAL_H */
