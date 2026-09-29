/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_RTF_H
#define WP_FORMAT_RTF_H

#include "types.h"
#include "docmodel.h"

/* Classic RTF 1.x subset import/export. Supported control words, scope,
   and documented simplifications are in docs/formats/rtf.md -- read it
   before changing this file. */

/* Receives one image's pixels during import -- the mirror of
 * wp_image_source, and needed for the same reason: the engine decodes the
 * picture but may not create the file the model's blob_path has to name.
 *
 * `begin` is called once the geometry is known and before any write. The
 * caller creates its own pixel-cache file, names it however it likes
 * (the Atari shell counter-suffixes, so a second image in one session
 * cannot overwrite the first), and returns that native path; the engine
 * records it in the descriptor verbatim.
 *
 * `write` receives the pixels top-down and unpadded -- the model's own
 * layout, already undone from the DIB's bottom-up padded rows -- in
 * order, one row per call. `end` closes the file.
 *
 * Any non-WP_OK abandons that one image: the paragraph is dropped and
 * the rest of the document still imports. A picture that cannot be
 * written is not a reason to lose the text around it. */
typedef struct {
    wp_status (*begin)(void *user, i32 width_px, i32 height_px,
                        char *path_out, u32 path_cap, u32 *path_len);
    wp_status (*write)(void *user, const u8 *bytes, u32 len);
    wp_status (*end)(void *user);
    void *user;
} wp_image_sink;

/* Imports RTF bytes into doc, materialising any \pict\dibitmap through
   `sink`. With a NULL sink -- which is what format_rtf_import is --
   pictures are skipped exactly as they were before import understood
   them at all. */
wp_status format_rtf_import_images(const u8 *bytes, u32 len, wp_document *doc,
                                    const wp_image_sink *sink);

/* Imports RTF bytes into doc. doc must NOT already be initialized --
   this function calls wp_document_init itself. On any failure, doc is
   left freed (safe to discard, not safe to use).
   WP_BADFMT if the input doesn't start "{\rtf1", or a hex/brace token is
   malformed.
   WP_UNSUPPORTED if the character set isn't \ansi/\ansicpg1252.
   WP_RANGE if group nesting or the color table exceeds this parser's
   fixed bounds. */
wp_status format_rtf_import(const u8 *bytes, u32 len, wp_document *doc);

/* Supplies one image's packed pixel bytes during export.
 *
 * The engine holds a PATH to a pixel-cache file, never the pixels
 * themselves (docs/document-model.md's Image section), and the engine
 * performs no file I/O -- CLAUDE.md rule 1. So the only way an image
 * reaches a saved document is for the caller, which does have a file
 * seam, to hand the bytes over a row at a time.
 *
 * `read` is asked for `cap` bytes starting at `offset` within the file
 * named by `blob_path` (a span in doc->strings). It should set *out_len
 * to what it actually produced. Anything but WP_OK, or a short read, is
 * not fatal: the exporter emits zero bytes for the rest of that row and
 * carries on, because the size of the output was already promised to the
 * caller by the measure pass and must not now change. A missing cache
 * file therefore costs the image's appearance, never the document. */
typedef struct {
    wp_status (*read)(void *user, wp_span blob_path, u32 offset,
                       u8 *out, u32 cap, u32 *out_len);
    void *user;
} wp_image_source;

/* Exports doc as RTF bytes into out (caller-owned, out_cap bytes).
   *out_len is always set to the exact number of bytes the export needs,
   whether or not it fit. Returns WP_RANGE (nothing written) if out_cap
   is too small; call once with out_cap == 0 to just measure.

   `src` may be NULL, and format_rtf_export is exactly that case. Then an
   image paragraph exports as a bracketed text placeholder naming the
   image and its size -- the same honest marker platform/dos already
   draws on a machine with no pixel graphics at all. What it must never
   be again is what it was before this existed: an image paragraph has no
   runs, so the exporter walked its empty run list and wrote an empty
   paragraph, and a document saved and reopened had lost its pictures
   with nothing said. */
wp_status format_rtf_export_images(const wp_document *doc, u8 *out, u32 out_cap,
                                    u32 *out_len, const wp_image_source *src);

wp_status format_rtf_export(const wp_document *doc, u8 *out, u32 out_cap,
                             u32 *out_len);

#endif /* WP_FORMAT_RTF_H */
