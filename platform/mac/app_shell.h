#ifndef WP_MAC_APP_SHELL_H
#define WP_MAC_APP_SHELL_H

/* Public entry point for the Mac Toolbox application shell (M12
   Phase 2). Runs Toolbox bring-up (InitGraf/InitFonts/InitWindows/
   InitMenus/TEInit/InitDialogs/InitCursor), builds the window and menu
   bar, pumps the WaitNextEvent loop until quit, then tears everything
   down before returning. Mirrors platform/amiga/app_shell.h's role
   exactly. */
void app_shell_run(void);

#endif /* WP_MAC_APP_SHELL_H */
