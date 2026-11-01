/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_TOOLS_H
#define WP_TOOLS_H

#include "types.h"

/* The Tools menu's four checks, portable: Thesaurus, Chat and Grammar
   Check through the offload gateway (Grammar falls back to the local
   grammar-lite service without one), Spell Check through the local
   dictionary service. One implementation for every backend, in the
   same spirit as wp_action_dictate (actions.h): the Mac and DOS shells
   had none of these -- the menu entries beeped -- and the Amiga carried
   its own copies of three. The ST keeps its own Grammar Check, whose
   interactive apply loop goes beyond the report these give.

   What a backend supplies is the presenter: a function that shows a
   title and a message with an OK button, in whatever its toolkit
   offers (an MUI requester, a form_alert, plat_dialog_message). It is a
   parameter rather than a call to the dialog seam because not every
   backend implements plat_dialog_message, and a backend that already
   has a message helper should use it. The offload link is the shell's
   too: it brings the link up its own way (each has its own endpoint
   string) before calling one of these; with no link, the three gateway
   tools say so.

   platform/common rules apply: engine headers, services and platform.h
   only, never a native header. */

struct wp_app_ctx;

typedef void (*wp_tools_show_fn)(struct wp_app_ctx *ctx, const char *title, const char *msg);

/* Synonyms for the word the caret follows. */
wp_status wp_tools_thesaurus(struct wp_app_ctx *ctx, wp_tools_show_fn show);

/* The gateway's reply to the caret's paragraph. */
wp_status wp_tools_chat(struct wp_app_ctx *ctx, wp_tools_show_fn show);

/* How many issues the caret's paragraph has and the first of them, from
   the gateway when connected, else from the local grammar-lite check. */
wp_status wp_tools_grammar_check(struct wp_app_ctx *ctx, wp_tools_show_fn show);

/* The first word the dictionary at dict_path does not know, with up to
   three suggestions. The dictionary stays open until wp_tools_shutdown;
   a missing file is reported, once per attempt. */
wp_status wp_tools_spell_check(struct wp_app_ctx *ctx, const char *dict_path, wp_tools_show_fn show);

/* Closes the dictionary, if one was opened. Safe without one. */
void wp_tools_shutdown(void);

#endif /* WP_TOOLS_H */
