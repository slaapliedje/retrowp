#ifndef WP_DOS_APP_SHELL_INTERNAL_H
#define WP_DOS_APP_SHELL_INTERNAL_H

#include "types.h"

/* Private to platform/dos -- shared between app_shell.c (via
   app_shell.h's app_shell_run()) and host/tools/dos_smoke.c (the
   DOSBox-based CI smoke test). Not installed; never included from
   engine/ or services/. */

/* T14.2: relayouts a fixture paragraph through the real engine
   (format_txt_import + wp_paragraph_relayout) against the real
   metrics_dos.c/draw_dos.c seam and asserts the wrap is sane -- the
   same "hand-computed bar" every other backend's own *_layout_
   selfcheck already meets, adapted for a fixed monospace cell instead
   of a real proportional font: DOS_CELL_WIDTH_TWIPS (metrics_dos_
   internal.h) makes every width a simple, exactly-verifiable multiple. */
wp_bool wp_dos_layout_selfcheck(void);

/* T14.3: real file round-trip (file_dos.c, Open Watcom's own libc) +
   real clipboard round-trip (clipboard_dos.c, in-process buffer --
   DOS has no OS-level clipboard service to integrate with, but
   platform.h's clipboard interface never requires cross-app sharing,
   only persistent storage during a session, so this is a conformant
   implementation, not a shortfall). Mirrors wp_atari_file_clipboard_
   selfcheck's exact shape. */
wp_bool wp_dos_file_clipboard_selfcheck(void);

/* T14.4: real INT 14h serial open/write/read(timeout)/close on real
   hardware, without needing a live gateway on the other end -- plus
   proves offload_init/offload_available/offload_shutdown behave
   correctly when no gateway is attached. Mirrors
   wp_amiga_transport_selfcheck's exact shape. */
wp_bool wp_dos_transport_selfcheck(void);

/* T14.5: proves dos_translate_key's scan-code table against real PC
   scan-code-set-1 values. Mirrors wp_amiga_key_translate_selfcheck's
   exact shape. */
wp_bool wp_dos_key_translate_selfcheck(void);

/* T14.5: real file round trip proving wp_config_load resolves all five
   real keymap= values. Mirrors wp_atari_config_selfcheck exactly. */
wp_bool wp_dos_config_selfcheck(void);

/* T14.5: proves the help system's non-blocking pieces against the real
   compiled-in help content and real metrics_dos.c wrapping. Mirrors
   wp_atari_help_selfcheck exactly. */
wp_bool wp_dos_help_selfcheck(void);

/* M15 T15.4: pure bit-math over a hand-built offload_capabilities,
   proving offload_caps_supports() itself -- the shared pre-check
   do_insert_image() relies on before ever building a request. Mirrors
   platform/atari/app_shell.c's own wp_atari_chat_selfcheck exactly;
   not a real gateway round trip (that's host/tests/test_image_convert.c's
   job). */
wp_bool wp_dos_image_selfcheck(void);

/* The menu selfcheck for the backend that has no menu: since every
   action here is reachable only by shortcut, this checks reachability
   itself, plus that the advertised quit chord is real under every
   keymap scheme (msword once had no quit binding at all, making the app
   unexitable). See app_shell.c. */
wp_bool wp_dos_menu_selfcheck(void);

#endif /* WP_DOS_APP_SHELL_INTERNAL_H */
