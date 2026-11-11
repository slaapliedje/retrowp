/* SPDX-License-Identifier: GPL-2.0-or-later */
/* LAUNCH.PRG, for the gates: starts C:\RETROWP.PRG with C:\TEST.RTF as
   its document, as a desktop does when a document is opened on it. Hatari
   runs it with --auto, which takes a program but no arguments
   (host/tools/ci_charsheet.sh). */
#include <osbind.h>

int main(void)
{
    static char tail[] = "\013C:\\TEST.RTF";   /* a Pexec tail: its length, then the text */
    return (int)Pexec(0, "C:\\RETROWP.PRG", tail, 0L);
}
