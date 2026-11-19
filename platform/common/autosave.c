/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "autosave.h"
#include "app_ctx.h"
#include "actions.h"
#include "recovery.h"
#include "docmodel.h"

/* How long until `ms` have passed since `start`; 0 if they have. */
static u32 left(u32 start, u32 ms)
{
    u32 gone = PLAT_TICKS_SINCE(start);
    return gone >= ms ? 0u : ms - gone;
}

void wp_autosave_caught_up(wp_app_ctx *ctx)
{
    if (ctx == NULL || ctx->doc == NULL) return;
    ctx->autosave.written = ctx->doc->edits;
    ctx->autosave.armed = WP_FALSE;
}

u32 wp_autosave_poll(wp_app_ctx *ctx, wp_image_source *img_src)
{
    u32 now, interval, wait, pause;

    if (ctx == NULL || ctx->doc == NULL) return 0;
    if (!ctx->doc_loaded || !wp_document_is_modified(ctx->doc)) {
        /* Nothing unsaved: the file on disk is the work. Whatever is
           typed next is the first change the copy lacks. */
        wp_autosave_caught_up(ctx);
        return 0;
    }
    if (ctx->recovery_path == NULL || ctx->recovery_path[0] == '\0' ||
        ctx->autosave.minutes == 0) {
        /* Off, but the changes stay owed: turned back on, it writes
           them. */
        ctx->autosave.armed = WP_FALSE;
        return 0;
    }
    if (ctx->doc->edits == ctx->autosave.written) return 0;

    now = plat_ticks_ms();
    if (!ctx->autosave.armed) {
        ctx->autosave.armed = WP_TRUE;
        ctx->autosave.due_from = now;
        ctx->autosave.seen = ctx->doc->edits;
        ctx->autosave.changed_at = now;
    } else if (ctx->doc->edits != ctx->autosave.seen) {
        ctx->autosave.seen = ctx->doc->edits;
        ctx->autosave.changed_at = now;
    }

    interval = (u32)ctx->autosave.minutes * 60000uL;
    wait = left(ctx->autosave.due_from, interval);
    pause = left(ctx->autosave.changed_at, WP_AUTOSAVE_PAUSE_MS);
    if (wait > 0 || pause > 0) return wait > pause ? wait : pause;

    if (wp_recovery_write(ctx, img_src) == WP_OK) {
        ctx->autosave.told = WP_FALSE;      /* wp_recovery_write caught up */
        return 0;
    }
    /* The last good copy is still there. Try again after another
       interval, and say so once -- not at every attempt, which would
       be a dialog every few minutes while the disk stays full. */
    ctx->autosave.due_from = plat_ticks_ms();
    if (!ctx->autosave.told) {
        ctx->autosave.told = WP_TRUE;
        wp_action_tell(ctx, WP_AUTOSAVE_FAILED_MSG);
    }
    return interval;
}
