#ifndef WP_FORMAT_RTF_INTERNAL_H
#define WP_FORMAT_RTF_INTERNAL_H

#include "types.h"

/* Private to engine/src -- shared between format_rtf_import.c and
   format_rtf_export.c, not installed under engine/include. Defined
   `static` (growth_internal.h's pattern): each including translation
   unit gets its own private copy, no separate .c file needed. */

#define RTF_MAX_COLORS 32

/* cp1252 0x80-0x9F -> Unicode codepoint (0x00-0x7F is ASCII passthrough;
   0xA0-0xFF matches Latin-1/Unicode directly and needs no table).
   Undefined cp1252 slots map to U+FFFD -- honest about "no defined
   character" rather than guessing. See docs/formats/rtf.md. */
static u32 rtf_cp1252_to_unicode(u8 b)
{
    static const u32 table[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
        0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178
    };
    if (b < 0x80) return (u32)b;
    if (b < 0xA0) return table[b - 0x80];
    return (u32)b;
}

#endif /* WP_FORMAT_RTF_INTERNAL_H */
