/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PROMPT_H
#define WP_PROMPT_H

#include "types.h"
#include "keyevent.h"

/* A one-line text prompt in the status line, for the few things that
   need a word or two typed -- a header, a footer -- on every target
   without a native text-entry dialog (platform.h has none, and four
   would be four ways for it to behave). The status line shows
   "<label> <text>_"; while the prompt is open it has every key, before
   any keymap scheme sees one: printable characters are typed, Backspace
   takes one back, Enter accepts (the done callback gets the text) and
   Escape cancels (it is not called).

   One lives in wp_app_ctx. platform/common rules apply. */

#define WP_PROMPT_MAX 120

struct wp_app_ctx;
typedef void (*wp_prompt_done_fn)(struct wp_app_ctx *ctx, const u8 *text, u32 len);

typedef struct {
    wp_bool           active;
    const char       *label;          /* static text, e.g. "Header:" */
    u8                text[WP_PROMPT_MAX];
    u32               len;
    wp_prompt_done_fn done;
} wp_prompt;

/* Opens the prompt with `initial` (may be NULL) already typed. */
void wp_prompt_begin(struct wp_app_ctx *ctx, const char *label,
                     const u8 *initial, u32 initial_len, wp_prompt_done_fn done);

/* One key while the prompt is open; WP_TRUE when it was the prompt's
   (always, while open). */
wp_bool wp_prompt_feed(struct wp_app_ctx *ctx, wp_key_event ev);

/* "<label> <text>_" into buf (NUL-terminated); the length. */
u32 wp_prompt_status(const wp_prompt *p, char *buf, u32 cap);

#endif /* WP_PROMPT_H */
