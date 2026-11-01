/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_MD_H
#define WP_FORMAT_MD_H

#include "types.h"
#include "docmodel.h"

/* Markdown, read and written around the document model like every other
   format (CLAUDE.md rule 2). What the model can hold is what survives:

     # Heading        a paragraph whose runs are bold at 24/18/14 pt
                      (H1/H2/H3; H4 and deeper come in as 12 pt bold and
                      go out as a bold paragraph)
     **bold** *em*    run styles; ***both*** is both; __ and _ the same
     blank line       ends a paragraph; lines without one between them
                      are one paragraph, joined with a space, as a
                      Markdown reader would show them
     - item, 1. item  each its own paragraph, the marker kept as text
     > quote, ---     kept as text, each its own paragraph
     ``` fences       kept as text; the lines inside stay separate and
                      unstyled, and come back with a blank line between
                      them, since the model has no code block
     `code`           kept with its backticks, no emphasis inside
     \*               an escaped punctuation character is that character

   Export puts one blank line between paragraphs, escapes what would
   otherwise read as markup (\, *, _, `, and a leading #, >, -, + or
   "1."), and writes an image paragraph as ![image](its cache file), the
   one place a Markdown reader is told where the pixels went rather
   than given them. Underline and colour have no Markdown and are left
   out. See docs/formats/markdown.md. */

wp_status format_md_import(const u8 *bytes, u32 len, wp_document *doc);

/* Same two-call sizing as format_txt_export: *out_len is always the
   size needed; WP_RANGE and nothing written when out_cap is short. */
wp_status format_md_export(const wp_document *doc, u8 *out, u32 out_cap, u32 *out_len);

#endif /* WP_FORMAT_MD_H */
