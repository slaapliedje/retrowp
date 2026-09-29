/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "app_ctx.h"

void wp_app_ctx_init(wp_app_ctx *ctx, wp_keymap_id keymap_id,
                      wp_document *doc, wp_undo_journal *journal)
{
    ctx->doc = doc;
    ctx->journal = journal;
    wp_keymap_init(ctx, keymap_id, doc, journal);

    ctx->window = NULL;
    ctx->gc = NULL;
    ctx->font = NULL;

    ctx->current_size_twips = 0;
    ctx->align = 0;
    ctx->style_flags = 0;

    ctx->doc_loaded = WP_FALSE;
    ctx->quit_requested = WP_FALSE;
}
