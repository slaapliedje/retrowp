#ifndef WP_FORMAT_RTF_H
#define WP_FORMAT_RTF_H

#include "types.h"
#include "docmodel.h"

/* Classic RTF 1.x subset import/export. Supported control words, scope,
   and documented simplifications are in docs/formats/rtf.md -- read it
   before changing this file. */

/* Imports RTF bytes into doc. doc must NOT already be initialized --
   this function calls wp_document_init itself. On any failure, doc is
   left freed (safe to discard, not safe to use).
   WP_BADFMT if the input doesn't start "{\rtf1", or a hex/brace token is
   malformed.
   WP_UNSUPPORTED if the character set isn't \ansi/\ansicpg1252.
   WP_RANGE if group nesting or the color table exceeds this parser's
   fixed bounds. */
wp_status format_rtf_import(const u8 *bytes, u32 len, wp_document *doc);

/* Exports doc as RTF bytes into out (caller-owned, out_cap bytes).
   *out_len is always set to the exact number of bytes the export needs,
   whether or not it fit. Returns WP_RANGE (nothing written) if out_cap
   is too small; call once with out_cap == 0 to just measure. */
wp_status format_rtf_export(const wp_document *doc, u8 *out, u32 out_cap,
                             u32 *out_len);

#endif /* WP_FORMAT_RTF_H */
