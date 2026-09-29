/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_WS_H
#define WP_FORMAT_WS_H

#include "types.h"
#include "docmodel.h"

/* WordStar document-mode importer (T187 Phase 1).
 *
 * Targets the CLASSIC WordStar line: 3.x on CP/M through 7.0 on DOS
 * (1992, the last of that lineage). Those versions share the document
 * conventions this reader depends on, so one lenient parser serves the
 * family rather than one parser per version.
 *
 * EXPLICITLY NOT WordStar 2000. Despite the name it was a separate
 * product with an incompatible format, and a file from it will not parse
 * as anything sensible here. Said out loud because the naming invites
 * exactly that mistake.
 *
 * WordStar for Windows (1991-93) is likewise a different codebase and
 * out of scope.
 *
 * See docs/formats/wordstar.md for the conventions, including which of
 * them are confidently documented and which still want checking against
 * a real specimen file. */

/* Parses `bytes` into a fresh document (initialised internally, exactly
   like format_txt_import -- the caller owns it and must wp_document_free
   it). Lenient by design: anything unrecognised degrades to plain text
   rather than failing the import, because a manuscript that opens
   imperfectly is worth immeasurably more than one that will not open. */
wp_status format_ws_import(const u8 *bytes, u32 len, wp_document *doc);

/* Heuristic "does this look like WordStar?" for callers that must decide
   by content rather than by name.

   Needed because WordStar files carry no magic number and no reserved
   extension -- they are commonly .WS, .DOC (colliding with Microsoft
   Word), or nothing at all. Answers on the two markers that are both
   distinctive and common: a soft carriage return (0x8D) and a dot
   command at the start of a line.

   Deliberately conservative: a plain ASCII file must NEVER be claimed,
   since WordStar's own non-document mode produced exactly that and it is
   already handled correctly by the plain-text importer. */
wp_bool format_ws_looks_like(const u8 *bytes, u32 len);

#endif /* WP_FORMAT_WS_H */
