/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_BITMAPFILE_H
#define WP_BITMAPFILE_H

#include "types.h"
#include "docmodel.h"
#include "format_rtf.h"   /* wp_image_sink */

/* Bitmap pictures read on the machine, without the gateway (T16.5):
   GEM .IMG (mono, and XIMG colour), IFF ILBM and Deluxe Paint's PBM,
   MacPaint, PCX and BMP. Each comes out as the model's picture -- packed
   4-bit indexed rows through an image sink, a palette of at most 16 --
   made to fit the machine's colours and its largest picture.

   Nothing holds the whole image: each format is read a row at a time,
   twice -- the first pass counts its colours, the second maps them to
   the most used (as many as the machine can show) and shrinks the rows
   to fit, each output pixel the darkest of the block it stands for, so
   a thin line in line art survives being halved. */

/* Whether these bytes (the file's first 128 will do) are a bitmap this
   reader knows. `name` (may be NULL) helps with MacPaint, which has no
   signature: a .MAC or .PNTG name, or a MacBinary header saying PNTG. */
wp_bool bitmapfile_detect(const u8 *bytes, u32 len, const char *name);

/* The picture decoded, reduced to max_colors (2..16) and to fit
   max_w x max_h, written through `sink`; *desc gets its size, palette
   and cache path (in doc's string pool), display size left 0 for the
   caller. WP_UNSUPPORTED for a file this reader does not know, or one of
   its kinds it does not (compressed BMP, HAM); WP_BADFMT for one cut
   short. */
wp_status bitmapfile_read(const u8 *bytes, u32 len, const char *name,
                          u32 max_colors, i32 max_w, i32 max_h,
                          const wp_image_sink *sink, wp_document *doc,
                          wp_image_desc *desc);

#endif /* WP_BITMAPFILE_H */
