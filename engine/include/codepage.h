/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_CODEPAGE_H
#define WP_CODEPAGE_H

#include "types.h"

/* Eight-bit code pages to and from Unicode, for importers that meet text
   in one: RTF's \ansicpgN, \'hh escapes and \mac/\pc/\pca, and
   anything vintage that stored text natively. Numbers are the Windows
   code page numbers RTF uses: 437 (IBM PC, RTF \pc), 850 (DOS Western,
   RTF \pca), 1250 (Central European), 1251 (Cyrillic), 1252 (Western,
   RTF's default) and 10000 (Mac Roman, RTF \mac). Tables are generated
   from Python's codecs (engine/src/codepage.c) -- 768 bytes a code page,
   constant data. */

/* WP_TRUE for the code pages above. */
wp_bool wp_codepage_known(u16 codepage);

/* The Unicode code point of `byte` in `codepage` (an unknown code page
   decodes as 1252); U+FFFD for a byte the code page leaves undefined. */
u32 wp_codepage_decode(u16 codepage, u8 byte);

/* The byte for `cp` in `codepage`, if it has one. */
wp_bool wp_codepage_encode(u16 codepage, u32 cp, u8 *out_byte);

#endif /* WP_CODEPAGE_H */
