/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_AMIGA_APP_SHELL_H
#define WP_AMIGA_APP_SHELL_H

/* Public entry point for the Amiga MUI application shell (M9 T9.1b).
   Opens muimaster.library/intuition.library/graphics.library/utility.
   library/asl.library/keymap.library, builds the Application/Window/
   Menustrip, pumps the MUI event loop until Quit, then closes everything
   down cleanly before returning. Mirrors platform/atari/app_shell.h's
   role exactly. */
void app_shell_run(void);
/* A document named in the Shell ("RETROWP SHEET.RTF"), opened at
   start-up in place of the sample. NULL or "" for none (a Workbench
   start passes no arguments). */
void app_shell_set_start_file(const char *path);

#endif /* WP_AMIGA_APP_SHELL_H */
