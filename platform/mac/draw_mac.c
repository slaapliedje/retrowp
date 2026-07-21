#include <stddef.h>
#include <Quickdraw.h>
#include "platform.h"
#include "utf8.h"
#include "metrics_mac_internal.h"

/* plat_utf8_to_native/plat_native_to_utf8 were pulled forward to Phase 1
   because the clipboard and file seams needed them to link -- same
   reasoning draw_amiga.c gives for doing the identical pull-forward in
   M9 Phase 1.

   Unlike Amiga's ISO-8859-1 (a genuine 1:1 identity map for 0x80-0xFF,
   not real extra work), Mac OS Roman is its own idiosyncratic ordering
   with no simple arithmetic relationship to Unicode -- a real mapping
   table remains future work, not assumed here. This follows Atari's own
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


/* -- Glyph draw (Phase 3): real QuickDraw, drawing into whatever real
   window the caller's mac_gc names (app_shell.c's own g_win during a
   real updateEvt repaint). Only ever asked to draw black text on a
   white background so far -- every fixture ROM boots into a 1-bit
   QuickDraw port by default, and color isn't a real feature on any
   backend yet -- so r/g/b are accepted but unused, same simplification
   draw_atari.c's own rgb_to_vdi_color makes explicit for its target. */

wp_status plat_draw_text(plat_gc *gc, plat_font *f, i32 x, i32 y,
                          const u8 *utf8, u32 len, u8 r, u8 g, u8 b)
{
    mac_gc *mgc = (mac_gc *)gc;
    mac_font *mf = (mac_font *)f;
    char buf[256];
    u32 n, out_len, unmapped;
    wp_status st;

    WP_UNUSED(r);
    WP_UNUSED(g);
    WP_UNUSED(b);

    if (mgc == NULL || mf == NULL) return WP_ERR;

    n = (len < (u32)sizeof(buf)) ? len : (u32)sizeof(buf);
    st = plat_utf8_to_native(utf8, n, (u8 *)buf, (u32)sizeof(buf), &out_len, &unmapped);
    if (st != WP_OK) return st;

    SetPort(mgc->window);
    TextFont(mf->fontNum);
    TextSize(mf->size);
    TextFace((short)mf->style);
    MoveTo((short)x, (short)y);
    DrawText(buf, 0, (short)out_len);
    return WP_OK;
}

wp_status plat_fill_rect(plat_gc *gc, i32 x, i32 y, i32 w, i32 h,
                          u8 r, u8 g, u8 b)
{
    mac_gc *mgc = (mac_gc *)gc;
    Rect rect;

    WP_UNUSED(r);
    WP_UNUSED(g);
    WP_UNUSED(b);

    if (mgc == NULL) return WP_ERR;

    SetPort(mgc->window);
    SetRect(&rect, (short)x, (short)y, (short)(x + w), (short)(y + h));
    PaintRect(&rect);
    return WP_OK;
}

void plat_gc_flush(plat_gc *gc)
{
    WP_UNUSED(gc);
}
