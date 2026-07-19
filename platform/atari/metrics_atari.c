#include <stddef.h>
#include <string.h>
#include "platform.h"
#include "docmodel.h"
#include "metrics_atari_internal.h"

/* No SpeedoGDOS on this build (vq_gdos() would confirm, but there's
   nothing installed to load even if present -- see
   docs/atari-platform.md) -- text is always the built-in bitmap system
   font at whatever point size vst_point() can actually hit; requested
   sizes snap to the nearest size the system font supports. */

/* A lazily-opened, metrics-only virtual VDI workstation, independent of
   any window's own VDI handle (font metrics can be queried before a
   window exists, or shared across paragraphs/windows). VDI text
   attributes (point size, effects) are workstation-global state, not
   per-call, so every measurement re-applies them before querying --
   see measure_and_height()'s font cache in linebreak.c, which can hand
   us several different plat_font instances that all share this one
   handle. */
static VdiHdl g_metrics_vdi = 0;

static wp_status ensure_metrics_vdi(void)
{
    short work_in[11];
    short work_out[57];
    short handle, dummy;

    if (g_metrics_vdi != 0) return WP_OK;

    handle = graf_handle(&dummy, &dummy, &dummy, &dummy);
    for (dummy = 0; dummy < 10; dummy++) work_in[dummy] = 1;
    work_in[10] = 2; /* raster coordinates */
    v_opnvwk(work_in, &handle, work_out);
    if (handle == 0) return WP_ERR;

    g_metrics_vdi = handle;
    return WP_OK;
}

i32 atari_twips_to_px(i32 twips)
{
    return (twips * ATARI_ASSUMED_DPI) / 1440;
}

i32 atari_px_to_twips(i32 px)
{
    return (px * 1440) / ATARI_ASSUMED_DPI;
}

typedef struct {
    short point_size;
    u16   style_flags;
} atari_font;

static short map_effects(u16 style_flags)
{
    short effects = TXT_NORMAL;

    if (style_flags & WP_STYLE_BOLD)      effects |= TXT_THICKENED;
    if (style_flags & WP_STYLE_ITALIC)    effects |= TXT_SKEWED;
    if (style_flags & WP_STYLE_UNDERLINE) effects |= TXT_UNDERLINED;
    /* WP_STYLE_STRIKE/SUPER/SUB have no VDI text-effect equivalent --
       not mapped; documented gap in docs/atari-platform.md. */
    return effects;
}

/* Re-applies this font's point size and effects to the shared metrics
   VDI handle. Must run before any query that depends on current text
   attributes (vst_point, vqt_extent, vqt_fontinfo). */
static void select_font(const atari_font *f)
{
    short charw, charh, cellw, cellh;

    vst_point(g_metrics_vdi, f->point_size, &charw, &charh, &cellw, &cellh);
    vst_effects(g_metrics_vdi, map_effects(f->style_flags));
}

wp_status plat_font_open(const u8 *utf8_font_name, u32 name_len,
                          i32 size_twips, u16 style_flags, plat_font **out)
{
    atari_font *f;
    wp_status st;
    short pt;

    WP_UNUSED(utf8_font_name);
    WP_UNUSED(name_len);

    st = ensure_metrics_vdi();
    if (st != WP_OK) return st;

    f = (atari_font *)mem_alloc((u32)sizeof(atari_font));
    if (f == NULL) return WP_NOMEM;

    pt = (short)(size_twips / 20);
    if (pt < 1) pt = 1;
    f->point_size = pt;
    f->style_flags = style_flags;

    *out = (plat_font *)f;
    return WP_OK;
}

void plat_font_close(plat_font *f)
{
    mem_free(f);
}

wp_status plat_font_metrics_get(plat_font *f, plat_font_metrics *out)
{
    atari_font *af = (atari_font *)f;
    short charw, charh, cellw, cellh;

    if (af == NULL || out == NULL) return WP_ERR;

    /* vst_point's own charh/cellh outputs are the simplest available
       approximation for ascent/descent (no vqt_fontinfo distances[]
       breakdown attempted here): charh is treated as the ascent (cap
       height to baseline), and the extra room in cellh as descent plus
       any internal leading. Documented simplification, not exact
       typographic ascent/descent -- see docs/atari-platform.md. */
    vst_point(g_metrics_vdi, af->point_size, &charw, &charh, &cellw, &cellh);

    out->ascent = atari_px_to_twips((i32)charh);
    out->descent = atari_px_to_twips((i32)(cellh - charh));
    if (out->descent < 0) out->descent = 0;
    out->leading = 0;
    return WP_OK;
}

wp_status plat_measure_text(plat_font *f, const u8 *utf8, u32 len,
                             i32 *width_twips_out)
{
    atari_font *af = (atari_font *)f;
    char buf[256];
    u32 n, i;
    short extent[8];
    short min_x, max_x;
    wp_status st;

    if (af == NULL || width_twips_out == NULL) return WP_ERR;

    n = (len < sizeof(buf) - 1) ? len : (u32)(sizeof(buf) - 1);
    /* No real Atari-ST-charset mapping table yet (plat_utf8_to_native is
       ASCII-passthrough-plus-'?' -- see draw_atari.c and
       docs/atari-platform.md); reuse it here so measurement and drawing
       agree on what bytes are actually being rendered. */
    {
        u32 out_len = 0;
        u32 unmapped = 0;
        st = plat_utf8_to_native(utf8, n, (u8 *)buf, (u32)(sizeof(buf) - 1), &out_len, &unmapped);
        if (st != WP_OK) return st;
        n = out_len;
    }
    buf[n] = '\0';

    select_font(af);

    if (n == 0) {
        *width_twips_out = 0;
        return WP_OK;
    }

    vqt_extent(g_metrics_vdi, buf, extent);
    min_x = max_x = extent[0];
    for (i = 2; i <= 6; i += 2) {
        if (extent[i] < min_x) min_x = extent[i];
        if (extent[i] > max_x) max_x = extent[i];
    }

    *width_twips_out = atari_px_to_twips((i32)(max_x - min_x));
    return WP_OK;
}
