/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_DRAWFILE_H
#define WP_DRAWFILE_H

#include "types.h"
#include "drawing.h"

/* Drawing files read straight into a drawing (drawing.h), on every
   machine and without the gateway: GEM metafiles (.GEM -- GDOS's own,
   GEM Draw's, Kandinsky's) and Windows metafiles (.WMF, placeable or
   not). docs/document-model.md, "Drawings", lists what is played back. */

typedef struct {
    i32 ext_w, ext_h;           /* the drawing's own extent */
    i32 width_twips;            /* its size on paper, where the file says; */
    i32 height_twips;           /* 0 where it does not */
} wp_drawfile_info;

/* Whether these bytes (the file's start will do: 32 bytes) are a drawing
   file this reader knows. */
wp_bool drawfile_detect(const u8 *bytes, u32 len);

/* The file's primitives appended to b, and its extent and size in *info.
   WP_UNSUPPORTED for a file that is not a drawing; WP_BADFMT for one that
   is, but cut short or with nothing drawable in it. */
wp_status drawfile_read(const u8 *bytes, u32 len, wp_drawing_builder *b,
                        wp_drawfile_info *info);

#endif /* WP_DRAWFILE_H */
