/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_APPNAME_H
#define WP_APPNAME_H

/* Names the writer sees.
 *
 * RetrWrks is the suite, in the line of Atari Works and AppleWorks:
 * eight letters, so it fits 8.3 on every machine. Each program in it
 * has a name of its own -- RetroWP, the word processor (this one);
 * RetroSS and RetroDB to come -- and RetrWrks itself is to be the
 * launcher that asks whether you want a new document, spreadsheet or
 * database.
 *
 * WP_APP_NAME is this program's: window titles, About, the Amiga's
 * $VER, the PostScript %%Creator, messages that name it.
 *
 * Some things say "RetroWP" as a TAG that files and other programs
 * read back, not as a name for the writer, and stay that word even if
 * the shown name ever changes:
 *   - the files: RETROWP.PRG/.EXE, RETROWP.CFG, RETROWP.REC
 *   - the recovery file's first line (WP_RECOVERY_MAGIC, recovery.h)
 *   - the RTF \*\generator, which this program and the offload gateway
 *     look for to know a file is their own (format_rtf_export.c,
 *     format_rtf_import.c, odt_gateway.py)
 *   - the Mac creator code 'RWP1' its documents carry
 * Changing one of those is a file-format change, not a rename. */
#define WP_APP_NAME     "RetroWP"
#define WP_APP_NAME_LEN 7
#define WP_SUITE_NAME   "RetrWrks"

#endif
