/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "app_shell.h"

/* A document named on the command line is opened at startup: what the
   TOS desktop passes when a file is opened with an installed application
   or dropped on the program. MiNTLib builds the argument vector; gem4xe's
   runtime has none, and keeps the plain entry point. */
#ifdef __MINT__
int main(int argc, char **argv)
{
    app_shell_set_start_file(argc > 1 ? argv[1] : (const char *)0);
    app_shell_run();
    return 0;
}
#else
int main(void)
{
    app_shell_run();
    return 0;
}
#endif
