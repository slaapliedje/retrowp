/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_IMGCACHE_H
#define WP_IMGCACHE_H

#include "platform.h"
#include "format_rtf.h"   /* wp_image_sink */

/* The picture cache every backend keeps: each picture's packed pixels in
   a file of its own, named by the backend (fmt writes the name for a
   counter value into out, 128 bytes, NUL-terminated) from a counter it shares
   with its own Insert Image, so no two pictures in a session share a
   file. The sink is what RTF import and the shared picture readers
   (bitmapfile.h) write through. One picture at a time. */
typedef void (*wp_cache_path_fn)(char *out, u32 counter);

void wp_image_cache_sink(wp_image_sink *sink, wp_cache_path_fn fmt, u32 *counter);

/* The other way, for saving (format_rtf_export_images): a picture's
   bytes read back from its cache file, which `doc`'s string pool names. */
void wp_image_cache_source(wp_image_source *src, const wp_document *doc);

#endif /* WP_IMGCACHE_H */
