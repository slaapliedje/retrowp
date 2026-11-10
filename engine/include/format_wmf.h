/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_WMF_H
#define WP_FORMAT_WMF_H

#include "types.h"
#include "drawing.h"

/* Windows metafiles, the form a drawing takes in RTF (\pict\wmetafile8)
   and the one Word and every other word processor reads. Little-endian,
   unlike this project's own formats. docs/document-model.md, "Drawings",
   lists the records read and written. */

/* A metafile's records turned into a drawing's primitives, appended to
   b. Coordinates are moved by the window origin and flipped where the
   window extent is negative; *ext_w and *ext_h get the extent, or 0
   where the file sets none (a placeable header's box is used if there
   is one). Records this reader does not draw are skipped. WP_BADFMT for
   a file that is not a metafile or runs short. */
wp_status wmf_to_drawing(const u8 *wmf, u32 len, wp_drawing_builder *b,
                         i32 *ext_w, i32 *ext_h);

/* A drawing written as a metafile (no placeable header -- RTF gives the
   size), its bytes handed to put a piece at a time. Allocates nothing,
   so a writer measuring and then writing gets the same bytes twice.
   *len_out (may be NULL) gets the byte count. */
typedef void (*wmf_put_fn)(void *user, const u8 *bytes, u32 n);
wp_status drawing_to_wmf(const u8 *ops, u32 len, i32 ext_w, i32 ext_h,
                         wmf_put_fn put, void *user, u32 *len_out);

#endif /* WP_FORMAT_WMF_H */
