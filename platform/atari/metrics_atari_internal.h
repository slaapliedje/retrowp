#ifndef WP_ATARI_METRICS_INTERNAL_H
#define WP_ATARI_METRICS_INTERNAL_H

#include <gem.h>
#include "types.h"

/* Private to platform/atari -- shared between metrics_atari.c (owns the
   twips<->pixel conversion and the lazily-opened metrics-only VDI
   workstation), draw_atari.c (the plat_gc implementation, drawing in
   pixels), and app_shell.c (which lays out text in twips via the engine
   and must convert to pixels with the SAME ratio before calling
   plat_draw_text/plat_fill_rect, so line breaks measured by the engine
   land exactly where they're drawn).

   Real device DPI is queried per-axis from v_opnvwk()'s own work_out[3]/
   [4] (pixel width/height in microns -- a standard VDI open-workstation
   output, NOT GDOS-specific; verified empirically via a real Hatari +
   EmuTOS boot with no GDOS driver attached at all, see
   docs/atari-platform.md). X and Y are queried and converted separately
   on purpose: low-res ST pixels are not square (confirmed empirically:
   75 DPI horizontal vs 68 DPI vertical at 320x200), so a single shared
   ratio would distort measurements relative to what's actually drawn.
   ATARI_ASSUMED_DPI is now only a defensive fallback (used per-axis if a
   driver ever reports a 0 or nonsensical pixel size, which shouldn't
   happen on real hardware but must never divide by zero) -- not the
   primary source of truth it was before. Because plat_measure_text and
   the pixel coordinates handed to plat_draw_text still go through the
   SAME per-axis conversion, wrap correctness (T4.2's AC) remains an
   internal-consistency property even though the ratio is now real. */
#define ATARI_ASSUMED_DPI 96

i32 atari_twips_to_px_x(i32 twips);
i32 atari_twips_to_px_y(i32 twips);
i32 atari_px_to_twips_x(i32 px);
i32 atari_px_to_twips_y(i32 px);

/* plat_gc for this backend: a real window's VDI handle plus the AES
   window handle it belongs to (needed for wind_get_grect-based clipping
   during redraw). Callers (app_shell.c) construct one directly -- there
   is no plat_window_create/plat_window_get_gc implementation for Atari
   yet, since app_shell.c already owns its window through direct AES
   calls (CLAUDE.md: "platform/ owns UI + rendering") and nothing in
   engine/ calls those platform.h functions. */
typedef struct {
    VdiHdl vdi;
    short  window_handle;
} atari_gc;

#endif /* WP_ATARI_METRICS_INTERNAL_H */
