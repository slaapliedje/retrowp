/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
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
    ctx->current_font = 0;
    ctx->input_armed = WP_FALSE;
    memset(&ctx->input_at, 0, sizeof(ctx->input_at));

    wp_viewport_init(&ctx->view, NULL);
    ctx->doc_path[0] = '\0';
    ctx->ensure_offload = NULL;
    ctx->printer_path = NULL;
    ctx->printer_codepage = 0;
    ctx->printer_ready = NULL;
    ctx->printer_driver = 0;
    ctx->printer_sides = 0;
    ctx->printer_choosable = WP_FALSE;
    ctx->config_path = NULL;
    ctx->print_pages = NULL;
    ctx->print_pages_name = NULL;
    memset(&ctx->prompt, 0, sizeof(ctx->prompt));

    ctx->doc_loaded = WP_FALSE;
    ctx->quit_requested = WP_FALSE;
}
