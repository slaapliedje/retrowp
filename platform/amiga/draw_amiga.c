#include <stddef.h>

#include <graphics/rastport.h>
#include <graphics/text.h>
#include <intuition/screens.h>
#include <proto/graphics.h>

#include "platform.h"
#include "utf8.h"
#include "metrics_amiga_internal.h"

/* T9.1a implemented only the encoding edge (plat_utf8_to_native/
   plat_native_to_utf8) -- clipboard_amiga.c needed it to link at all, so
   it couldn't wait for T9.2's real glyph-draw work. T9.2 (this revisit)
   adds plat_draw_text/plat_fill_rect/plat_gc_flush below, RastPort-based
   exactly like draw_atari.c is VDI-based, completing the pair per the
   M9 plan's architecture table.

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

/* r,g,b -> a real MUI pen index. Pens are screen-relative palette slots,
   not direct RGB, so (like Atari's rgb_to_vdi_color choosing between the
   low-res palette's two fixed G_BLACK/G_WHITE entries) this maps to
   whichever of the window's own DrawInfo TEXTPEN/BACKGROUNDPEN pens is
   closer by the same luminance threshold -- callers (app_shell.c) have
   only ever asked for black text on the theme's normal background so
   far, same scope note draw_atari.c's own version carries. */
static UBYTE rgb_to_pen(struct DrawInfo *dri, u8 r, u8 g, u8 b)
{
    u32 luminance = (u32)r + (u32)g + (u32)b;
    return (UBYTE)dri->dri_Pens[(luminance < 384) ? TEXTPEN : BACKGROUNDPEN];
}

wp_status plat_draw_text(plat_gc *gc, plat_font *f, i32 x, i32 y,
                          const u8 *utf8, u32 len, u8 r, u8 g, u8 b)
{
    amiga_gc *agc = (amiga_gc *)gc;
    amiga_font *af = (amiga_font *)f;
    char buf[256];
    u32 n, out_len, unmapped;
    wp_status st;

    if (agc == NULL || af == NULL || af->tf == NULL) return WP_ERR;

    n = (len < sizeof(buf)) ? len : (u32)sizeof(buf);
    st = plat_utf8_to_native(utf8, n, (u8 *)buf, (u32)sizeof(buf), &out_len, &unmapped);
    if (st != WP_OK) return st;
    if (out_len == 0) return WP_OK;

    /* Font attributes are re-selected here (unlike draw_atari.c, which
       leaves this to the caller's own VDI-attribute state) because a
       RastPort's current font is per-RastPort state that app_shell.c's
       caller doesn't otherwise track between lines of potentially
       different styles -- see paint_document's cached_style pattern,
       mirrored by app_shell.c's own painting loop for this backend. */
    SetFont(agc->rp, af->tf);
    SetAPen(agc->rp, rgb_to_pen(agc->dri, r, g, b));
    SetBPen(agc->rp, (UBYTE)agc->dri->dri_Pens[BACKGROUNDPEN]);
    SetDrMd(agc->rp, JAM2);
    Move(agc->rp, (LONG)x, (LONG)y);
    Text(agc->rp, (STRPTR)buf, (ULONG)out_len);
    return WP_OK;
}

wp_status plat_fill_rect(plat_gc *gc, i32 x, i32 y, i32 w, i32 h,
                          u8 r, u8 g, u8 b)
{
    amiga_gc *agc = (amiga_gc *)gc;

    if (agc == NULL) return WP_ERR;

    SetAPen(agc->rp, rgb_to_pen(agc->dri, r, g, b));
    RectFill(agc->rp, (LONG)x, (LONG)y, (LONG)(x + w - 1), (LONG)(y + h - 1));
    return WP_OK;
}

void plat_gc_flush(plat_gc *gc)
{
    WP_UNUSED(gc);
}
