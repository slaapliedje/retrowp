/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_CISUMMARY_H
#define WP_CISUMMARY_H

#include "types.h"
#include "docmodel.h"

/* What a document holds, one line, for the CI gates' alive markers -- so
   a gate on a real machine can tell the whole document came in, not only
   that something drew (host/tools/ci_charsheet.sh):

       doc paras=N pictures=N drawings=N rows=N frames=N\n

   pictures are bitmap picture paragraphs, drawings vector ones, rows the
   table rows, frames the frames used. No sprintf: every backend's marker
   writer avoids stdio. Returns the bytes written (at most cap). */
u32 wp_ci_doc_summary(const wp_document *doc, char *out, u32 cap);

#endif /* WP_CISUMMARY_H */
