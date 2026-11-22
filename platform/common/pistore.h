/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PISTORE_H
#define WP_PISTORE_H

/* The Pi's document store (M21 T21.6): a copy of a saved file kept on
 * the gateway, each machine's apart and each document's last versions,
 * and a document fetched back from there. The wire side is OFFLOAD_STORE,
 * OFFLOAD_LIST and OFFLOAD_FETCH (docs/formats/offload-protocol.md's
 * "Document store"); the gateway's is offload/gateway/doc_store.py.
 *
 * What reaches the menus:
 *   File > Save to Pi   the document saved as Save would, then that
 *                       file's bytes sent (ctx->to_pi, actions.c)
 *   File > Open from Pi the documents kept for this machine, a version
 *                       of the one chosen, written beside the documents
 *                       and opened (ctx->from_pi)
 *   Settings' "Copy to the Pi": every save also sends the copy.
 *
 * The machine's name on the store is wp_settings_machine(). */

#include "types.h"

struct wp_app_ctx;

/* The link is up (brought up the shell's way) and its gateway keeps
   documents. Says nothing. */
wp_bool wp_pistore_able(struct wp_app_ctx *ctx);

/* Sends the file just written at native_path -- its bytes, `len` of
   them -- to be kept under that file's own name. `quiet`: say nothing
   when it worked, and a failure only once until one works again (the
   copy every save makes); otherwise the Pi's answer is told either way.
   WP_OK once the gateway has said the copy is safe. */
wp_status wp_pistore_put(struct wp_app_ctx *ctx, const char *native_path,
                         const u8 *bytes, u32 len, wp_bool quiet);

/* Open from Pi's picker, in the shape of a shell's pick_path: the
   documents kept for this machine, newest first and eight to a page;
   then, when there is more than one, which version; then the file
   fetched and written in the documents folder (Settings' docdir, or the
   program's own) under its name -- asking first before it replaces a
   file there. WP_TRUE with that file's path in `out`. */
wp_bool wp_pistore_pick(struct wp_app_ctx *ctx, char *out, u32 cap);

#endif
