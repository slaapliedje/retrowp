/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PSDEV_H
#define WP_PSDEV_H

#include "platform.h"
#include "docmodel.h"

/* PostScript that matches the screen (M16 follow-up): the page printer
   (pageprint.h) draws the document -- laid out as this machine lays it
   out, line for line -- onto a device that writes PostScript instead of
   pixels (devout.h). Borders, shading, tables, frames and drawings are
   paths; pictures are dithered bands (imagemask); each word is set in
   the standard PostScript face its style asks for (Times, Helvetica or
   Courier, bold and italic), stretched or narrowed to the width this
   machine measured, so the line breaks are the screen's. Coordinates
   are tenths of a point. Language level 1; the paper size and
   duplex are asked of setpagedevice where there is one.

   `write` takes the PostScript as it is made. `sides` is a
   wp_prn_sides (format_prn.h). *pages_out gets the pages written. */
typedef wp_status (*wp_ps_write_fn)(void *user, const u8 *bytes, u32 len);

wp_status wp_ps_print(wp_document *doc, wp_ps_write_fn write, void *user, u8 sides,
                      u32 *pages_out);

#endif /* WP_PSDEV_H */
