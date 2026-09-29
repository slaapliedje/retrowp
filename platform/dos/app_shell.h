/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_DOS_APP_SHELL_H
#define WP_DOS_APP_SHELL_H

/* Public entry point for the DOS text-mode application shell (M14
   T14.1). Phase 1 scope only: sets 80x25 text mode, runs a keyboard
   echo loop entirely through direct B800 video-memory writes and BIOS
   INT 16h reads (no engine/document linkage yet -- that starts once
   enough of platform.h exists, T14.2+), until Esc is pressed. */
void app_shell_run(void);

#endif /* WP_DOS_APP_SHELL_H */
