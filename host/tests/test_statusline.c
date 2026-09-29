/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "statusline.h"

/* draw_host.c's plat_draw_text/plat_fill_rect are no-ops (no real
   drawing surface on host, see platform/host/draw_host.c's own
   comment) -- these tests can only prove wp_statusline_paint runs
   without crashing across representative wp_vi_status_view shapes
   using the real host plat_window/plat_font seam, the same boundary
   every other host test involving drawing already respects
   (test_fake_monospace_metrics is the precedent). Real visual
   confirmation happens once this is wired into a live backend's paint
   path (see docs/atari-platform.md). */
void test_statusline_paint_does_not_crash_with_various_views(void)
{
    plat_window *win;
    plat_gc *gc;
    plat_font *font;
    wp_vi_status_view view;

    ASSERT_TRUE(plat_window_create((const u8 *)"t", 1, 640, 200, &win) == WP_OK);
    ASSERT_TRUE(plat_window_get_gc(win, &gc) == WP_OK);
    ASSERT_TRUE(plat_font_open((const u8 *)"mono", 4, 240, 0, &font) == WP_OK);

    /* mode only */
    view.mode_label_utf8 = (const u8 *)"NORMAL";
    view.mode_label_len = 6;
    view.pending_count = 0;
    view.pending_cmd_utf8 = NULL;
    view.pending_cmd_len = 0;
    wp_statusline_paint(gc, font, 0, 180, 640, 20, &view);

    /* mode + pending count (e.g. "3" before "3dd") */
    view.mode_label_utf8 = (const u8 *)"NORMAL";
    view.mode_label_len = 6;
    view.pending_count = 3;
    wp_statusline_paint(gc, font, 0, 180, 640, 20, &view);

    /* command-line mode with a partial buffer */
    view.mode_label_utf8 = (const u8 *)"COMMAND";
    view.mode_label_len = 7;
    view.pending_count = 0;
    view.pending_cmd_utf8 = (const u8 *)":wq";
    view.pending_cmd_len = 3;
    wp_statusline_paint(gc, font, 0, 180, 640, 20, &view);

    /* NULL view -- still just paints the background, doesn't crash */
    wp_statusline_paint(gc, font, 0, 180, 640, 20, NULL);

    plat_font_close(font);
    plat_window_destroy(win);
}
