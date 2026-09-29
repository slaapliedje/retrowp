/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_ATARI_APP_SHELL_H
#define WP_ATARI_APP_SHELL_H

/* Public entry point for the Atari ST AES application shell (M4 T4.1).
   Runs appl_init, installs the menu bar, opens the main window, pumps
   the evnt_multi event loop until Quit/close, then tears everything
   down cleanly (menu_bar remove, wind_close/delete, v_clsvwk,
   appl_exit) before returning. */
void app_shell_run(void);

#endif /* WP_ATARI_APP_SHELL_H */
