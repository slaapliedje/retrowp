#include <stddef.h>
#include "platform.h"
#include "utf8.h"

/* T9.1a only implements the encoding edge (plat_utf8_to_native/
   plat_native_to_utf8) -- clipboard_amiga.c needs it to link at all, so
   it can't wait for T9.2's real glyph-draw work. plat_draw_text/
   plat_fill_rect/plat_gc_flush (RastPort-based rendering) are added in
   T9.2, per the M9 plan's architecture table.

   Unlike the Atari backend's ASCII-only '?' fallback for every codepoint
   above 0x7F (a documented, deliberate gap -- see docs/atari-platform.md),
   Amiga's native charset genuinely is ISO-8859-1 (Latin-1) from
   Kickstart 2.0's topaz.font onward -- this project's Amiga floor is
   already 2.04+ for MUI (see docs/amiga-platform.md), so codepoints
   0x80-0xFF map identically to native bytes 0x80-0xFF, no lookup table
   needed. Only codepoints above 0xFF (genuinely outside Latin-1) fall
   back to '?'. */

wp_status plat_utf8_to_native(const u8 *utf8, u32 len,
                               u8 *out, u32 out_cap, u32 *out_len,
                               u32 *unmapped_count)
{
    u32 pos = 0;
    u32 written = 0;
    u32 unmapped = 0;

    if (utf8 == NULL || out == NULL) return WP_ERR;

    while (pos < len && written < out_cap) {
        u32 cp;
        u32 n = utf8_decode(utf8 + pos, len - pos, &cp);

        if (n == 0) {
            out[written++] = '?';
            unmapped++;
            pos++;
            continue;
        }

        if (cp <= 0xFF) {
            out[written++] = (u8)cp;
        } else {
            out[written++] = '?';
            unmapped++;
        }
        pos += n;
    }

    if (out_len) *out_len = written;
    if (unmapped_count) *unmapped_count = unmapped;
    return WP_OK;
}

wp_status plat_native_to_utf8(const u8 *native, u32 len,
                               u8 *out, u32 out_cap, u32 *out_len)
{
    u32 i;
    u32 written = 0;

    if (native == NULL || out == NULL) return WP_ERR;

    for (i = 0; i < len; i++) {
        u8 tmp[4];
        u32 n = utf8_encode((u32)native[i], tmp);
        u32 j;

        if (written + n > out_cap) break;
        for (j = 0; j < n; j++) out[written++] = tmp[j];
    }

    if (out_len) *out_len = written;
    return WP_OK;
}
