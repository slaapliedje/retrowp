#include <stddef.h>
#include "platform.h"
#include "utf8.h"

/* Only plat_utf8_to_native/plat_native_to_utf8 for now, pulled forward
   from Phase 2 (QuickDraw metrics/render) because the clipboard and
   file seams need them to link -- same reasoning draw_amiga.c gives for
   doing the identical pull-forward in M9 Phase 1.

   Unlike Amiga's ISO-8859-1 (a genuine 1:1 identity map for 0x80-0xFF,
   not real extra work), Mac OS Roman is its own idiosyncratic ordering
   with no simple arithmetic relationship to Unicode -- a real mapping
   table is Phase 2/3 work, not assumed here. This follows Atari's own
   precedent instead: ASCII (0x00-0x7F) passes through unchanged, and
   every other codepoint becomes a single '?' fallback byte, counted in
   *unmapped_count. A documented gap, not a silent one. */

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

        if (cp <= 0x7F) {
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

    for (i = 0; i < len && written < out_cap; i++) {
        out[written++] = (native[i] <= 0x7F) ? native[i] : (u8)'?';
    }

    if (out_len) *out_len = written;
    return WP_OK;
}
