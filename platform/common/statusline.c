#include <stddef.h>
#include "statusline.h"

#define WP_STATUSLINE_BUF_MAX 128

static u32 append_bytes(u8 *buf, u32 len, u32 cap, const u8 *src, u32 src_len)
{
    u32 i;

    for (i = 0; i < src_len && len < cap - 1; i++) buf[len++] = src[i];
    return len;
}

/* No sprintf/stdio -- matches this project's usual portable-code
   discipline (host/tools/atari_smoke.c's own nf_log_long has the same
   shape for the same reason). */
static u32 append_decimal(u8 *buf, u32 len, u32 cap, u32 n)
{
    u8 digits[10];
    u32 digit_count = 0;

    if (n == 0) {
        digits[digit_count++] = '0';
    } else {
        while (n > 0 && digit_count < (u32)sizeof(digits)) {
            digits[digit_count++] = (u8)('0' + (n % 10));
            n /= 10;
        }
    }
    while (digit_count > 0 && len < cap - 1) buf[len++] = digits[--digit_count];
    return len;
}

void wp_statusline_paint(plat_gc *gc, plat_font *font,
                          i32 x, i32 y, i32 w, i32 h,
                          const wp_vi_status_view *view)
{
    u8 buf[WP_STATUSLINE_BUF_MAX];
    u32 len = 0;

    /* Light gray bar, matching GEM's typical chrome tone -- this
       function doesn't try to match a specific platform's exact desktop
       color scheme, just a legible neutral background for black text. */
    plat_fill_rect(gc, x, y, w, h, 0xC0, 0xC0, 0xC0);

    if (view == NULL) return;

    len = append_bytes(buf, len, WP_STATUSLINE_BUF_MAX, view->mode_label_utf8, view->mode_label_len);

    if (view->pending_count > 0) {
        len = append_bytes(buf, len, WP_STATUSLINE_BUF_MAX, (const u8 *)" ", 1);
        len = append_decimal(buf, len, WP_STATUSLINE_BUF_MAX, view->pending_count);
    }

    if (view->pending_cmd_len > 0) {
        len = append_bytes(buf, len, WP_STATUSLINE_BUF_MAX, (const u8 *)" ", 1);
        len = append_bytes(buf, len, WP_STATUSLINE_BUF_MAX, view->pending_cmd_utf8, view->pending_cmd_len);
    }

    if (len > 0) plat_draw_text(gc, font, x + 4, y, buf, len, 0, 0, 0);
}
