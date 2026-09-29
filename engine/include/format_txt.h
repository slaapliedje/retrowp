/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_FORMAT_TXT_H
#define WP_FORMAT_TXT_H

#include "types.h"
#include "docmodel.h"

/* Plain UTF-8 text import/export. Policy is documented in full in
   docs/formats/txt.md -- summary: each newline-terminated line is exactly
   one paragraph, one section, default styles throughout. Import accepts
   LF or CRLF line endings and a final line that is not newline-
   terminated; export always emits LF-only with exactly one trailing
   newline per paragraph (including the last). Only input already in that
   canonical form round-trips byte-for-byte through
   export -> import -> export. */

/* Imports UTF-8 plaintext bytes into doc. doc must NOT already be
   initialized -- this function calls wp_document_init itself. On any
   failure, doc is left freed (safe to discard, not safe to use). */
wp_status format_txt_import(const u8 *bytes, u32 len, wp_document *doc);

/* Exports doc as UTF-8 plaintext into out (caller-owned, out_cap bytes).
   *out_len is always set to the exact number of bytes the export needs,
   whether or not it fit. Returns WP_RANGE (nothing written) if out_cap is
   too small; call once with out_cap == 0 to just measure. */
wp_status format_txt_export(const wp_document *doc, u8 *out, u32 out_cap,
                             u32 *out_len);

#endif /* WP_FORMAT_TXT_H */
