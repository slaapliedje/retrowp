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

/* T9.3: proves transport_amiga.c's real serial.device IORequest
   lifecycle and offload_client.c's graceful-no-gateway behavior --
   see app_shell.c's own comment on wp_amiga_transport_selfcheck for
   what it checks, why, and what it deliberately does not (a real,
   investigated environment limitation, not an oversight). */
wp_bool wp_amiga_transport_selfcheck(void);

/* M9 Phase 6: proves the help system's non-blocking pieces against the
   real compiled-in help content and real diskfont-measured font metrics
   -- see app_shell.c's own comment on wp_amiga_help_selfcheck for what
   it checks and why. */
wp_bool wp_amiga_help_selfcheck(void);

/* M10 Phase 2: proves translate_rawkey's real IEQUALIFIER Alt-bit
   extraction -- see app_shell.c's own comment on
   wp_amiga_key_translate_selfcheck for what it checks and why. */
wp_bool wp_amiga_key_translate_selfcheck(void);

/* M10 Phase 6: proves wp_config_load resolves all five real keymap=
   values against a real file at WP_AMIGA_CONFIG_PATH -- see
   app_shell.c's own comment on wp_amiga_config_selfcheck for what it
   checks and why. */
wp_bool wp_amiga_config_selfcheck(void);

/* M15 T15.4: pure bit-math over a hand-built offload_capabilities,
   proving offload_caps_supports() itself -- the shared pre-check
   do_insert_image() (and do_chat before it) relies on before ever
   building a request. Mirrors platform/atari/app_shell.c's own
   wp_atari_chat_selfcheck exactly; not a real gateway round trip
   (that's host/tests/test_image_convert.c's job). */
wp_bool wp_amiga_image_selfcheck(void);

/* Menu construction check that needs no live Menustrip: the portable
   table checks plus the shortcut-buffer LIFETIME property that actually
   broke here (MUI does not copy MUIA_Menuitem_Shortcut, so every
   pointer must be distinct, in-range process-lifetime storage that is
   still valid after the builder returns). See app_shell.c. */
wp_bool wp_amiga_menu_selfcheck(void);

#endif /* WP_AMIGA_APP_SHELL_INTERNAL_H */
