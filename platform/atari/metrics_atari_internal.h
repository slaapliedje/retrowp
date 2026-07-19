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

   There is no GDOS on this build (see docs/atari-platform.md), so there
   is no way to query a real physical screen DPI -- ATARI_ASSUMED_DPI is
   a fixed, documented simplification, not a calibrated value. Because
   plat_measure_text and the pixel coordinates handed to plat_draw_text
   both go through this SAME conversion, wrap correctness (T4.2's AC) is
   an internal-consistency property, not something that depends on the
   assumed DPI being physically accurate. */
#define ATARI_ASSUMED_DPI 96

i32 atari_twips_to_px(i32 twips);
i32 atari_px_to_twips(i32 px);

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
