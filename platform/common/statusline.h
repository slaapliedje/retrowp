#ifndef WP_STATUSLINE_H
#define WP_STATUSLINE_H

#include "types.h"
#include "platform.h"

/* A vi-mode status/mode line (M8.5) -- a thin, reserved screen strip
   showing the current mode plus any pending count/command buffer
   (e.g. "NORMAL", "3d", "COMMAND :wq"). Needs NO new platform.h
   primitive: plat_draw_text/plat_fill_rect already take arbitrary
   (x, y, w, h) and are already how the document body itself gets
   drawn -- a status strip is just a fixed screen region outside engine
   layout/pagination, not a new kind of thing. See docs/vimode.md.

   This header intentionally has no dependency on vimode.h: it's a pure
   READ-side view of whatever produces the display strings (currently
   wp_vimode_get_status_text-equivalent data assembled by the caller),
   keeping platform/common/statusline.c usable by anything that wants a
   labeled status strip, not hard-wired to vi-mode specifically. */
typedef struct {
    const u8 *mode_label_utf8;   /* e.g. "NORMAL" / "INSERT" / "VISUAL" */
    u32       mode_label_len;
    u32       pending_count;     /* the "3" in "3dd"; 0 = none, not shown */
    const u8 *pending_cmd_utf8;  /* partial command buffer, e.g. ":wq", "d" */
    u32       pending_cmd_len;
} wp_vi_status_view;

/* Fills (x, y, w, h) with a background bar and draws view's fields as
   one left-aligned line of text -- "<mode> <count> <pending_cmd>",
   omitting any field that's empty/zero. x/y/w/h are plain pixels (the
   same unit plat_draw_text/plat_fill_rect already use), NOT twips --
   the caller (a backend's own app-shell code) is responsible for
   converting its own twips-based layout to a pixel rect first, the
   same way it already does for the document body, and for sizing `h`
   from plat_font_metrics_get's real ascent+descent+leading rather than
   a hardcoded pixel count (matters concretely given ST Low's own
   documented "notoriously tight" 40-column budget). gc/font are passed
   straight through to plat_draw_text/plat_fill_rect -- this function
   does no drawing of its own beyond those two calls. */
void wp_statusline_paint(plat_gc *gc, plat_font *font,
                          i32 x, i32 y, i32 w, i32 h,
                          const wp_vi_status_view *view);

#endif /* WP_STATUSLINE_H */
