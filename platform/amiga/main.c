/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "app_shell.h"

/* A document named in the Shell is opened at start-up:
   "RETROWP SHEET.RTF". From Workbench argc is 0. */
int main(int argc, char **argv)
{
    app_shell_set_start_file(argc > 1 ? argv[1] : (const char *)0);
    app_shell_run();
    return 0;
}
