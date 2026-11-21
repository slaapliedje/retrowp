/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "app_ctx.h"
#include "autosave.h"

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
    ctx->doc_rtf = WP_FALSE;
    ctx->ensure_offload = NULL;
    ctx->repaint = NULL;
    ctx->printer_path = NULL;
    ctx->printer_codepage = 0;
    ctx->printer_ready = NULL;
    ctx->printer_driver = 0;
    ctx->printer_sides = 0;
    ctx->printer_choosable = WP_FALSE;
    ctx->config_path = NULL;
    ctx->recovery_path = NULL;
    ctx->recovery_owned = WP_FALSE;
    ctx->autosave.minutes = WP_AUTOSAVE_DEFAULT_MINUTES;
    wp_settings_defaults(&ctx->settings);
    ctx->recovery_default = NULL;
    ctx->offload_default = NULL;
    ctx->clipart_default = NULL;
    ctx->settings_changed = NULL;
    ctx->path_sep = '\\';
    ctx->offload_retry = WP_FALSE;
    ctx->autosave.armed = WP_FALSE;
    ctx->autosave.told = WP_FALSE;
    ctx->autosave.written = 0;
    ctx->autosave.seen = 0;
    ctx->autosave.due_from = 0;
    ctx->autosave.changed_at = 0;
    ctx->print_pages = NULL;
    ctx->print_pages_name = NULL;
    memset(&ctx->prompt, 0, sizeof(ctx->prompt));
    ctx->page_view_able = WP_FALSE;
    ctx->page_view = WP_FALSE;
    wp_pagecanvas_init(&ctx->canvas, NULL, NULL, NULL, NULL);

    ctx->doc_loaded = WP_FALSE;
    ctx->quit_requested = WP_FALSE;
}
