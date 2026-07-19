#include <stddef.h>
#include "platform.h"
#include "utf8.h"
#include "metrics_atari_internal.h"

/* No real Atari-ST-high-ASCII mapping table yet -- ASCII (0x00-0x7F)
   passes through unchanged (already charset-compatible), and every
   other codepoint (regardless of its UTF-8 byte length) becomes exactly
   one '?' fallback byte, counted in *unmapped_count. Sufficient and
   correct for this milestone's ASCII sample document; a real high-ASCII
   table is a documented gap, not a silent one -- see
   docs/atari-platform.md. */
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

/* Inverse direction: native Atari charset input (keyboard/clipboard/file)
   to UTF-8. Same ASCII-only scope for now -- bytes >= 0x80 aren't a
   valid single-byte-to-codepoint mapping yet, so they're dropped in
   favor of a literal '?' rather than emitting a wrong codepoint. */
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

/* r,g,b -> nearest ST palette index. Only ever asked to draw black text
   on a white background at this milestone, so a simple luminance
   threshold against the two fixed low-res-palette entries (G_WHITE=0,
   G_BLACK=1) is enough; a real nearest-color search over the full
   16-entry palette is unneeded until color text is a real feature. */
static short rgb_to_vdi_color(u8 r, u8 g, u8 b)
{
    u32 luminance = (u32)r + (u32)g + (u32)b;
    return (luminance < 384) ? G_BLACK : G_WHITE;
}

wp_status plat_draw_text(plat_gc *gc, plat_font *f, i32 x, i32 y,
                          const u8 *utf8, u32 len, u8 r, u8 g, u8 b)
{
    atari_gc *agc = (atari_gc *)gc;
    char buf[256];
    u32 n, out_len, unmapped;
    wp_status st;

    WP_UNUSED(f); /* font attributes are already selected by the caller's
                     layout pass via plat_measure_text/plat_font_metrics_get;
                     v_gtext draws with the VDI handle's CURRENT text
                     attributes, which app_shell.c re-selects per line
                     via the same font before calling this. */

    if (agc == NULL) return WP_ERR;

    n = (len < sizeof(buf) - 1) ? len : (u32)(sizeof(buf) - 1);
    st = plat_utf8_to_native(utf8, n, (u8 *)buf, (u32)(sizeof(buf) - 1), &out_len, &unmapped);
    if (st != WP_OK) return st;
    buf[out_len] = '\0';

    vst_color(agc->vdi, rgb_to_vdi_color(r, g, b));
    v_gtext(agc->vdi, (short)x, (short)y, buf);
    return WP_OK;
}

wp_status plat_fill_rect(plat_gc *gc, i32 x, i32 y, i32 w, i32 h,
                          u8 r, u8 g, u8 b)
{
    atari_gc *agc = (atari_gc *)gc;
    short pxy[4];

    if (agc == NULL) return WP_ERR;

    pxy[0] = (short)x;
    pxy[1] = (short)y;
    pxy[2] = (short)(x + w - 1);
    pxy[3] = (short)(y + h - 1);

    vsf_interior(agc->vdi, FIS_SOLID);
    vsf_color(agc->vdi, rgb_to_vdi_color(r, g, b));
    v_bar(agc->vdi, pxy);
    return WP_OK;
}

void plat_gc_flush(plat_gc *gc)
{
    WP_UNUSED(gc);
}
