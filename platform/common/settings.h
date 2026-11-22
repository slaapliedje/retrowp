/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_SETTINGS_H
#define WP_SETTINGS_H

/* Settings (M21 T21.4): one model, one dialog, every port.
 *
 * What the writer can change without editing the configuration file by
 * hand, kept in it as key=value lines beside the rest (config.h):
 *
 *     docdir=     where Open and Save start for a document that has no
 *                 name yet ("" = the program's own folder)
 *     datadir=    where the dictionary (WPSPELL.DIC) and the clipart
 *                 folder are loaded from ("" = beside the program). No
 *                 port loads fonts from a folder of its own -- they are
 *                 the system's (GDOS's ASSIGN.SYS, the Amiga's FONTS:,
 *                 the Mac's System file) -- so this is where "where
 *                 fonts and the dictionary are loaded from" lands.
 *     recovery=   the recovery file (recovery.h; "" = the machine's
 *                 own: RETROWP.REC beside the program on the ST, ...)
 *     autosave=   minutes, or off (autosave.h)
 *     backup=     on: a save keeps the copy it replaces as NAME.BAK,
 *                 the way WordStar and 1st Word did; off (the default)
 *     offload=    the offload link ("" = the machine's own, such as
 *                 serial:aux:9600; serial:<port>:<baud>[:hard] or
 *                 tcp:<host>:<port>, platform.h's transport seam)
 *     picopy=     on: every save also keeps a copy on the Pi's document
 *                 store (T21.6, pistore.h); off (the default)
 *     machine=    the name this machine's documents are kept under on
 *                 the Pi ("" = the port's own: ST, AMIGA, DOS, MAC)
 *
 * The dialog is plat_dialog_form, which each backend draws its own way;
 * this file decides what is in it and what an answer does.
 *
 * Every text is at most PLAT_FORM_TEXT_MAX characters, the widest edit
 * field every machine can show whole -- a vintage path is short. */

#include "types.h"
#include "platform.h"

struct wp_app_ctx;

#define WP_SETTINGS_TEXT (PLAT_FORM_TEXT_MAX + 1)

typedef struct {
    char docdir[WP_SETTINGS_TEXT];
    char datadir[WP_SETTINGS_TEXT];
    char recovery[WP_SETTINGS_TEXT];
    char offload[WP_SETTINGS_TEXT];
    char machine[WP_SETTINGS_TEXT]; /* this machine's name on the Pi */
    u16  autosave_minutes;          /* 0 = off */
    u8   backup;                    /* 1 = keep NAME.BAK */
    u8   pi_copy;                   /* 1 = every save also goes to the Pi */
} wp_settings;

/* Everything at its default: autosave 5 minutes, the rest empty/off. */
void wp_settings_defaults(wp_settings *s);

/* Puts ctx->settings into effect: the recovery file it names (or the
   shell's recovery_default), the autosave interval. A shell calls it
   once at startup, after copying the configuration's settings in. */
void wp_settings_apply(struct wp_app_ctx *ctx);

/* The offload link to use: the setting, or the shell's offload_default. */
const char *wp_settings_offload(const struct wp_app_ctx *ctx);

/* This machine's name on the Pi: the setting, or the shell's
   machine_default. */
const char *wp_settings_machine(const struct wp_app_ctx *ctx);

/* A file in the data folder: datadir joined with `name` by the shell's
   path_sep (no separator added after one already there). WP_FALSE --
   and `out` untouched -- when the data folder is the default, so the
   shell looks where it always has, or when it does not fit. */
wp_bool wp_settings_data_file(const struct wp_app_ctx *ctx, const char *name,
                              char *out, u32 cap);

/* File > Settings (the menu's place is each port's own): the dialog,
   then on OK the answer checked, put into effect and written to
   ctx->config_path. A malformed offload link says what it should look
   like and opens the dialog again with what was typed. WP_TRUE when
   the settings were changed. */
wp_bool wp_action_settings(struct wp_app_ctx *ctx);

#endif
