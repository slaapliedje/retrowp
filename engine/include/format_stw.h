/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_STW_H
#define WP_FORMAT_STW_H

#include "types.h"
#include "docmodel.h"

/* Atari Works word-processor documents (.STW), both versions the
   corpus holds ("10" and "11"). See docs/formats/atariworks.md for the
   map this reads by -- it was taken from 263 documents that also exist
   as RTF and plain text, and scored against them.
 *
 * Read: the text (Atari character set to UTF-8), paragraphs, each run's
 * face, size, bold and underline (italic from an italic face's name),
 * each paragraph's alignment, indents, line spacing and tab stops, and
 * the page's paper and margins. A merge field reads as its name,
 * "LABELS.STD:Company", as Atari Works' own text export writes it.
 * Placed objects become pictures in frames: a GEM metafile through
 * drawfile.c, a box, rule or shape Atari Works drew itself as a drawing;
 * each follows the paragraph that ends above it, so it stays beside its
 * text. Not read: an IMG (its pixels are in a file of its own), text
 * inside a metafile (drawfile.c draws none), headers and footers. */

/* Parses `bytes` into a fresh document (initialised here, exactly like
   format_txt_import -- the caller owns it and must wp_document_free it).
   WP_ERR for a file that is not Atari Works or is cut short; nothing
   is half-read into the document. */
wp_status format_stw_import(const u8 *bytes, u32 len, wp_document *doc);

/* Whether `bytes` opens as Atari Works: the "10" or "11" magic and a
   structure that walks to its body. */
wp_bool format_stw_looks_like(const u8 *bytes, u32 len);

#endif /* WP_FORMAT_STW_H */
