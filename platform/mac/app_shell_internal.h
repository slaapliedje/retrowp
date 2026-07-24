#ifndef WP_MAC_APP_SHELL_INTERNAL_H
#define WP_MAC_APP_SHELL_INTERNAL_H

#include "types.h"

/* On-target selfcheck surface, mirroring platform/atari/app_shell_internal.h
   and platform/amiga/app_shell_internal.h exactly: functions declared here
   are exercised by a standalone smoke test (host/tools/mac/smoke.c)
   cross-compiled and run under BasiliskII, never called from engine/ or
   services/ code. Grows alongside app_shell.c phase by phase. */

wp_bool wp_mac_file_clipboard_selfcheck(void);

/* Phase 3: proves wp_paragraph_relayout/plat_measure_text/plat_font_open
   work correctly against real QuickDraw metrics on real target hardware
   -- see app_shell.c's own comment on wp_mac_layout_selfcheck for what
   it checks and why. */
wp_bool wp_mac_layout_selfcheck(void);

/* Phase 4: proves transport_mac.c's real Device Manager async I/O
   lifecycle and offload_client.c's graceful-no-gateway behavior -- see
   app_shell.c's own comment on wp_mac_transport_selfcheck for what it
   checks, why, and what it deliberately does not. */
wp_bool wp_mac_transport_selfcheck(void);

/* Phase 5: proves wp_config_load resolves all five real keymap= values
   against a real on-target file -- see app_shell.c's own comment on
   wp_mac_config_selfcheck for what it checks and why. */
wp_bool wp_mac_config_selfcheck(void);

/* Phase 5: proves the help system's non-blocking pieces against the
   real compiled-in help content and real QuickDraw font metrics -- see
   app_shell.c's own comment on wp_mac_help_selfcheck for what it checks
   and why. */
wp_bool wp_mac_help_selfcheck(void);

/* M15 T15.4: pure bit-math over a hand-built offload_capabilities,
   proving offload_caps_supports() itself -- the shared pre-check
   do_insert_image() relies on before ever building a request. Mirrors
   platform/atari/app_shell.c's own wp_atari_chat_selfcheck exactly;
   not a real gateway round trip (that's host/tests/test_image_convert.c's
   job). */
wp_bool wp_mac_image_selfcheck(void);

#endif /* WP_MAC_APP_SHELL_INTERNAL_H */
