/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_MERGE_H
#define WP_MERGE_H

#include "types.h"
#include "docmodel.h"

/* Mail merge (M18). A merge field is text in guillemets, «Name» -- the
   way Word shows a MERGEFIELD, and characters every target's set has
   (Atari, Mac Roman, Latin-1), so a field can be seen and typed
   anywhere. RTF writes one as a real MERGEFIELD (format_rtf_export.c)
   and reads Word's back as its shown «Name». The data is a text file:
   a first line of field names, then a record a line, the fields split
   by commas, semicolons or tabs (whichever the first line uses most),
   a field in double quotes where it holds one of those or a line
   break ("" inside is a quote). UTF-8, or else this machine's own
   character set. See docs/document-model.md, "Mail merge". */

#define WP_MERGE_OPEN  "\xc2\xab"   /* « */
#define WP_MERGE_CLOSE "\xc2\xbb"   /* » */
#define WP_MERGE_NAME_MAX 64

/* The next field in text[from..len): *start and *end bound the whole
   «Name», *name_at and *name_len the name. WP_FALSE when there is none.
   A name is 1..WP_MERGE_NAME_MAX bytes on one line. */
wp_bool wp_merge_next_field(const u8 *text, u32 len, u32 from, u32 *start, u32 *end,
                            u32 *name_at, u32 *name_len);

/* A data file, read: `fields` names (row 0) and `records` rows of that
   many cells each, every cell UTF-8 in `bytes`. */
typedef struct {
    u8  *bytes;
    u32 *cell_at, *cell_len;   /* (row * fields + column) */
    u32  fields, records;
} wp_merge_data;

/* WP_BADFMT for a file with no field names; a short row's missing
   cells are empty, a long row's extra ones dropped. */
wp_status wp_merge_data_read(const u8 *bytes, u32 len, wp_merge_data *out);
void      wp_merge_data_free(wp_merge_data *d);

/* The column named `name` (ASCII case ignored, spaces round it too);
   -1 for none. */
i32 wp_merge_data_column(const wp_merge_data *d, const u8 *name, u32 len);

/* The merge, in place: the document becomes one copy of itself a
   record, each copy starting a new page (its sections repeated), every
   field replaced by that record's value. A field the data has no
   column for stays as it is, to be seen. *copies_out gets the records
   merged. On a failure part way the document is whole -- some copies
   made -- and the status says so. */
wp_status wp_merge_document(wp_document *doc, const wp_merge_data *d, u32 *copies_out);

/* Labels: the first section's paragraphs are one label; the section
   becomes a table of `across` labels a row, each row exactly the text
   height over `down` tall and each label the column over `across` wide
   (the page's margins are the sheet's), a record a label, the last row
   filled out with empty labels. WP_UNSUPPORTED when the label holds a
   table or a frame of its own, WP_RANGE for across outside 1..16 or
   down outside 1..60. *copies_out as above. */
wp_status wp_merge_labels(wp_document *doc, const wp_merge_data *d, u32 across, u32 down,
                          u32 *copies_out);

#endif /* WP_MERGE_H */
