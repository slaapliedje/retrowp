/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_STYLES_H
#define WP_STYLES_H

#include "types.h"
#include "docmodel.h"

/* Named paragraph styles (M19): "Heading 1", "Quotation" -- a name, the
   paragraph format a paragraph in it takes and the character format of
   its text. The document's stylesheet is doc->named (docmodel.h); a
   paragraph is in style N when its paragraph format's style_id is N.
   Formatting a styled paragraph directly (centring a heading) keeps it
   in its style; applying or redefining the style sets it afresh.

   RetroWP offers a few styles of its own (wp_style_builtin_*), copied
   into a document the first time one is used, so a document that uses
   none carries no stylesheet. See docs/document-model.md, "Styles". */

/* The built-in styles: count, and each one's name (ASCII). */
u32         wp_style_builtin_count(void);
const char *wp_style_builtin_name(u32 index);

/* The document's style named `name` (ASCII case ignored), or 0. */
u16 wp_style_find(const wp_document *doc, const u8 *name, u32 len);

/* The style named `name`, from the document's stylesheet or else a
   built-in copied into it; WP_RANGE for a name that is neither. */
wp_status wp_style_ensure(wp_document *doc, const u8 *name, u32 len, u16 *id_out);

/* Adds the style `name` with these formats, or, where the document has
   one by that name, gives it them -- and every paragraph in it takes
   them again (wp_style_restyle). */
wp_status wp_style_define(wp_document *doc, const u8 *name, u32 len, const wp_para_style *ps,
                          const wp_char_style *cs, u16 *id_out);

/* Paragraph `pi` of section `si` put in style `id`: its paragraph
   format the style's, every run's character format the style's. */
wp_status wp_style_apply(wp_document *doc, u32 si, u32 pi, u16 id);

/* Every paragraph in style `id` given its formats again. */
wp_status wp_style_restyle(wp_document *doc, u16 id);

/* The character format text typed into `para` takes when it has none
   to follow -- an empty paragraph: its named style's, else the
   document's default. */
wp_cstyle wp_style_text_cstyle(const wp_document *doc, const wp_paragraph *para);

/* What a paragraph split off the end of `para` (Enter at its end) is
   formatted as: Normal after a Title or a Heading, as a word processor
   does, else the same as `para` (*out = para's own format). */
wp_status wp_style_next_pstyle(wp_document *doc, const wp_paragraph *para, wp_pstyle *out);

/* The style's name (not NUL-terminated), *len its length; NULL for 0
   or past the stylesheet. */
const u8 *wp_style_name(const wp_document *doc, u16 id, u32 *len);

#endif /* WP_STYLES_H */
