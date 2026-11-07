/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_PRN_H
#define WP_FORMAT_PRN_H

#include "types.h"
#include "docmodel.h"

/* A document laid out for a character printer: the bytes File > Print
   sends to a machine's own printer port (the ST's PRN:, the Amiga's
   PRT:, DOS's PRN) when there is no gateway printer to use. Export-only;
   docs/formats/prn.md has the whole policy. In short:

   - Fixed pitch: cpi characters to the inch across, lpi lines down (10
     and 6, a dot-matrix printer's own defaults, when given 0). Each
     section's paper and margins become columns and lines at that pitch,
     so the printout keeps the document's page layout.
   - Paragraphs wrap at word boundaries inside their indents, and keep
     their alignment (justify pads the gaps), tab stops (every stop is a
     left stop here), spacing before and after, and line spacing -- 1.5
     alternates single and double, since a line printer cannot do half a
     line.
   - The header and footer print in the middle of the top and bottom
     margins, '|' parts left, centre and right and '#' the page number,
     as the PDF prints them. Every page ends with a form feed.
   - A picture prints as "[Picture]".
   - `driver` says what the printer understands: plain text, or a
     printer language that also carries bold, italic and underline --
     Epson ESC/P, HP PCL 5 (with paper size and two-sided printing), the
     Amiga's printer.device codes (which its Printer preferences turn
     into the chosen printer's own), or PostScript, which lays the same
     grid out as a page description in Courier.
   - Text goes out in `codepage` (codepage.h's numbers; 0 is 7-bit
     ASCII). What it cannot hold is printed as the nearest ASCII --
     typographic quotes and dashes, "..." for an ellipsis -- else '?'.
     Code 1252 never sends 0x80-0x9F, which an ISO 8859-1 printer (the
     Amiga's) reads as control codes. */

typedef enum {
    WP_PRN_PLAIN = 0,      /* text, no codes */
    WP_PRN_EPSON = 1,      /* Epson ESC/P */
    WP_PRN_PCL = 2,        /* HP PCL 5 */
    WP_PRN_AMIGA = 3,      /* the Amiga's printer.device command codes */
    WP_PRN_POSTSCRIPT = 4  /* PostScript (Latin-1; codepage is ignored) */
} wp_prn_driver;

typedef enum {
    WP_PRN_SIMPLEX = 0,
    WP_PRN_DUPLEX_LONG = 1,   /* two-sided, bound on the long edge */
    WP_PRN_DUPLEX_SHORT = 2   /* two-sided, bound on the short edge */
} wp_prn_sides;

typedef struct {
    u16 codepage;   /* 0 = ASCII; 437 for an Epson-style printer, 1252 Latin-1 */
    u8  cpi;        /* characters per inch; 0 = 10 */
    u8  lpi;        /* lines per inch; 0 = 6 */
    u8  crlf;       /* WP_TRUE: lines end CR LF (a printer); else LF */
    u8  driver;     /* wp_prn_driver */
    u8  sides;      /* wp_prn_sides; PCL and PostScript act on it */
    u8  _pad;
} wp_prn_options;

/* Takes the printout as it is made, a line or less per call. A non-OK
   return stops the export, which then returns it. */
typedef wp_status (*wp_prn_write_fn)(void *user, const u8 *bytes, u32 len);

/* Lays doc out and sends it to `write`. *out_pages (when not NULL) is
   the number of pages sent -- 0 for a document with no paragraphs, which
   sends nothing. WP_NOMEM if a working buffer cannot be had. */
wp_status format_prn_export(const wp_document *doc, const wp_prn_options *opt,
                            wp_prn_write_fn write, void *user, u32 *out_pages);

#endif /* WP_FORMAT_PRN_H */
