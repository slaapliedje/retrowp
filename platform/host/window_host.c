#include <stddef.h>
#include "platform.h"

/* No real GUI at this stage -- enough surface to link and let a test
   harness exercise the seam. plat_window/plat_gc are opaque handles per
   platform.h; this struct is private to this backend. */
typedef struct {
    i32 w;
    i32 h;
} host_window;

wp_status plat_window_create(const u8 *title_utf8, u32 title_len,
                              i32 w, i32 h, plat_window **out)
{
    host_window *win;

    WP_UNUSED(title_utf8);
    WP_UNUSED(title_len);

    win = (host_window *)mem_alloc((u32)sizeof(host_window));
    if (win == NULL) return WP_NOMEM;
    win->w = w;
    win->h = h;
    *out = (plat_window *)win;
    return WP_OK;
}

void plat_window_destroy(plat_window *w)
{
    mem_free(w);
}

wp_status plat_window_get_gc(plat_window *w, plat_gc **out)
{
    if (w == NULL || out == NULL) return WP_ERR;
    /* No real drawing surface: the window handle itself doubles as a
       non-null opaque gc handle for draw_host.c's no-op functions. */
    *out = (plat_gc *)w;
    return WP_OK;
}

wp_status plat_dialog_message(plat_window *parent, plat_dialog_kind kind,
                               const u8 *utf8_msg, u32 len,
                               plat_dialog_result *out)
{
    WP_UNUSED(parent);
    WP_UNUSED(kind);
    WP_UNUSED(utf8_msg);
    WP_UNUSED(len);
    if (out) *out = PLAT_DR_OK;
    return WP_OK;
}
