/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "prompt.h"
#include "app_ctx.h"
#include "utf8.h"

void wp_prompt_begin(wp_app_ctx *ctx, const char *label,
                     const u8 *initial, u32 initial_len, wp_prompt_done_fn done)
{
    wp_prompt *p = &ctx->prompt;

    p->active = WP_TRUE;
    p->label = label;
    p->done = done;
    p->len = 0;
    if (initial != NULL) {
        if (initial_len > WP_PROMPT_MAX) initial_len = WP_PROMPT_MAX;
        /* Never keep half a character at the cut. */
        while (initial_len > 0 && initial_len < WP_PROMPT_MAX &&
               (initial[initial_len] & 0xC0) == 0x80) initial_len--;
        memcpy(p->text, initial, initial_len);
        p->len = initial_len;
    }
}

wp_bool wp_prompt_feed(wp_app_ctx *ctx, wp_key_event ev)
{
    wp_prompt *p = &ctx->prompt;

    if (!p->active) return WP_FALSE;

    if (ev.special == (u16)WP_KEY_ESCAPE) {
        p->active = WP_FALSE;
        return WP_TRUE;
    }
    if (ev.special == (u16)WP_KEY_ENTER) {
        p->active = WP_FALSE;
        if (p->done != NULL) p->done(ctx, p->text, p->len);
        return WP_TRUE;
    }
    if (ev.special == (u16)WP_KEY_BACKSPACE) {
        if (p->len > 0) p->len = utf8_prev(p->text, p->len, p->len);
        return WP_TRUE;
    }
    if (ev.special == (u16)WP_KEY_NONE && ev.codepoint >= 0x20 && ev.codepoint != 0x7F &&
        (ev.mods & (WP_MOD_CTRL | WP_MOD_ALT)) == 0) {
        u8 enc[4];
        u32 n = utf8_encode(ev.codepoint, enc);

        if (n > 0 && p->len + n <= WP_PROMPT_MAX) {
            memcpy(p->text + p->len, enc, n);
            p->len += n;
        }
    }
    return WP_TRUE;   /* every other key is swallowed while it is open */
}

u32 wp_prompt_status(const wp_prompt *p, char *buf, u32 cap)
{
    u32 n = 0, i;
    const char *l = (p->label != NULL) ? p->label : "";

    if (cap == 0) return 0;
    for (i = 0; l[i] != '\0' && n + 1 < cap; i++) buf[n++] = l[i];
    if (n + 1 < cap) buf[n++] = ' ';
    for (i = 0; i < p->len && n + 1 < cap; i++) buf[n++] = (char)p->text[i];
    if (n + 1 < cap) buf[n++] = '_';
    buf[n] = '\0';
    return n;
}
