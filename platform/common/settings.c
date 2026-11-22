/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "settings.h"
#include "app_ctx.h"
#include "actions.h"
#include "autosave.h"
#include "config.h"
#include "recovery.h"
#include "offload.h"

void wp_settings_defaults(wp_settings *s)
{
    s->docdir[0] = '\0';
    s->datadir[0] = '\0';
    s->recovery[0] = '\0';
    s->offload[0] = '\0';
    s->machine[0] = '\0';
    s->autosave_minutes = WP_AUTOSAVE_DEFAULT_MINUTES;
    s->backup = 0;
    s->pi_copy = 0;
}

/* The clipart folder in the data folder (one program, one of these). */
static char g_clipart[WP_SETTINGS_TEXT + 16];

void wp_settings_apply(wp_app_ctx *ctx)
{
    ctx->recovery_path = ctx->settings.recovery[0] != '\0' ? ctx->settings.recovery
                                                           : ctx->recovery_default;
    ctx->autosave.minutes = ctx->settings.autosave_minutes;
    if (ctx->clipart_default != NULL) {
        char name[9];

        /* CLIPART and the machine's separator, in the data folder; with
           none set, the shell's own place for it. */
        memcpy(name, "CLIPART", 7);
        name[7] = ctx->path_sep;
        name[8] = '\0';
        ctx->clipart_dir = wp_settings_data_file(ctx, name, g_clipart, (u32)sizeof(g_clipart))
                               ? g_clipart : ctx->clipart_default;
    }
}

const char *wp_settings_offload(const wp_app_ctx *ctx)
{
    return ctx->settings.offload[0] != '\0' ? ctx->settings.offload : ctx->offload_default;
}

const char *wp_settings_machine(const wp_app_ctx *ctx)
{
    if (ctx->settings.machine[0] != '\0') return ctx->settings.machine;
    return ctx->machine_default != NULL ? ctx->machine_default : "RETROWP";
}

static wp_bool is_sep(char c)
{
    return (wp_bool)(c == '\\' || c == '/' || c == ':');
}

wp_bool wp_settings_data_file(const wp_app_ctx *ctx, const char *name, char *out, u32 cap)
{
    const char *dir = ctx->settings.datadir;
    u32 dl = (u32)strlen(dir), nl = (u32)strlen(name);
    wp_bool add = (wp_bool)(dl > 0 && !is_sep(dir[dl - 1]));

    if (dl == 0 || dl + (add ? 1u : 0u) + nl + 1u > cap) return WP_FALSE;
    memcpy(out, dir, dl);
    if (add) out[dl++] = ctx->path_sep;
    memcpy(out + dl, name, nl + 1);
    return WP_TRUE;
}

/* -- The dialog ------------------------------------------------------ */

static const u16 k_minutes[] = { 0, 1, 2, 5, 10, 15, 30, 60 };
#define K_MINUTES_N ((u32)(sizeof(k_minutes) / sizeof(k_minutes[0])))

static const char *k_autosave_labels[K_MINUTES_N + 1] = {
    "Off", "1 minute", "2 minutes", "5 minutes", "10 minutes",
    "15 minutes", "30 minutes", "60 minutes", NULL
};
static const char *const k_backup_labels[] = { "Off", "Keep NAME.BAK" };
static const char *const k_picopy_labels[] = { "Off", "On every save" };

/* "N minutes" for a value from the configuration file that is not one of
   the list's: shown as itself, last, so OK does not quietly change it. */
static char g_other_minutes[16];

static u32 put_u(char *out, u32 v)
{
    char d[12];
    u32 k = 0, i = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v > 0 && k < 11);
    while (k > 0) out[i++] = d[--k];
    out[i] = '\0';
    return i;
}

/* Spaces off both ends: a path typed with one is a different path. */
static void trim_text(char *s)
{
    u32 n = (u32)strlen(s), b = 0;
    while (n > 0 && s[n - 1] == ' ') s[--n] = '\0';
    while (s[b] == ' ') b++;
    if (b > 0) memmove(s, s + b, n - b + 1);
}

static wp_bool starts(const char *s, const char *p)
{
    return (wp_bool)(strncmp(s, p, strlen(p)) == 0);
}

/* An offload link the transport seam can read: empty (the machine's
   own), serial:..., tcp:..., or pipe:... (the host's). What follows the
   prefix is the backend's to check when it connects. */
static wp_bool offload_ok(const char *s)
{
    return (wp_bool)(s[0] == '\0' ||
                     (starts(s, "serial:") && s[7] != '\0') ||
                     (starts(s, "tcp:") && s[4] != '\0') ||
                     (starts(s, "pipe:") && s[5] != '\0'));
}

enum { F_DOCDIR, F_DATADIR, F_RECOVERY, F_AUTOSAVE, F_BACKUP, F_OFFLOAD, F_PICOPY, F_MACHINE, F_COUNT };

static void text_field(plat_form_field *f, const char *label, char *buf)
{
    f->kind = PLAT_FORM_TEXT;
    f->label = label;
    f->text = buf;
    f->text_cap = WP_SETTINGS_TEXT;
    f->options = NULL;
    f->option_count = 0;
    f->index = 0;
}

static void choice_field(plat_form_field *f, const char *label,
                         const char *const *options, u32 n, u32 index)
{
    f->kind = PLAT_FORM_CHOICE;
    f->label = label;
    f->text = NULL;
    f->text_cap = 0;
    f->options = options;
    f->option_count = n;
    f->index = index;
}

/* The recovery file moves: the copy this document has is written at the
   new place before the old one goes, so there is never a moment with
   none. WP_FALSE (and the old place kept) if it cannot be written. */
static wp_bool move_recovery(wp_app_ctx *ctx, const char *old_path)
{
    if (!ctx->recovery_owned || old_path == NULL) return WP_TRUE;
    if (wp_recovery_write(ctx, NULL) != WP_OK) return WP_FALSE;
    if (ctx->recovery_path == NULL || strcmp(old_path, ctx->recovery_path) != 0) {
        (void)plat_file_delete(old_path);
    }
    return WP_TRUE;
}

wp_bool wp_action_settings(wp_app_ctx *ctx)
{
    /* Static: six fields and a working copy are a lot for a 4 KB Amiga
       stack (and the dialog backends need some of it too). */
    static wp_settings edit;
    static plat_form_field fields[F_COUNT];
    static char old_recovery[WP_APP_PATH_MAX];
    static char old_setting[WP_SETTINGS_TEXT];
    static const char title[] = "Settings";
    u32 i, mi, choices;
    wp_bool cancelled = WP_FALSE, offload_changed;
    const char *old_path;

    edit = ctx->settings;
    for (;;) {
        /* The autosave list: the eight standard choices, and the
           configuration's own value after them when it is none of those. */
        choices = K_MINUTES_N;
        mi = K_MINUTES_N;
        for (i = 0; i < K_MINUTES_N; i++) if (k_minutes[i] == edit.autosave_minutes) mi = i;
        if (mi == K_MINUTES_N) {
            u32 n = put_u(g_other_minutes, edit.autosave_minutes);
            memcpy(g_other_minutes + n, " minutes", 9);
            k_autosave_labels[K_MINUTES_N] = g_other_minutes;
            choices = K_MINUTES_N + 1;
        }
        text_field(&fields[F_DOCDIR], "Documents folder", edit.docdir);
        text_field(&fields[F_DATADIR], "Dictionary folder", edit.datadir);
        text_field(&fields[F_RECOVERY], "Recovery file", edit.recovery);
        choice_field(&fields[F_AUTOSAVE], "Autosave", (const char *const *)k_autosave_labels, choices, mi);
        choice_field(&fields[F_BACKUP], "Backups", k_backup_labels, 2u, edit.backup ? 1u : 0u);
        text_field(&fields[F_OFFLOAD], "Offload link", edit.offload);
        choice_field(&fields[F_PICOPY], "Copy to the Pi", k_picopy_labels, 2u, edit.pi_copy ? 1u : 0u);
        text_field(&fields[F_MACHINE], "Name on the Pi", edit.machine);

        if (plat_dialog_form(ctx->window, (const u8 *)title, (u32)(sizeof(title) - 1),
                             fields, (u32)F_COUNT, &cancelled) != WP_OK || cancelled) {
            return WP_FALSE;
        }
        trim_text(edit.docdir);
        trim_text(edit.datadir);
        trim_text(edit.recovery);
        trim_text(edit.offload);
        trim_text(edit.machine);
        edit.pi_copy = (u8)(fields[F_PICOPY].index == 1u ? 1 : 0);
        mi = fields[F_AUTOSAVE].index;
        if (mi < K_MINUTES_N) edit.autosave_minutes = k_minutes[mi];   /* else: as it was */
        edit.backup = (u8)(fields[F_BACKUP].index == 1u ? 1 : 0);
        if (offload_ok(edit.offload)) break;
        wp_action_tell(ctx, "Offload link: serial:PORT:BAUD or tcp:HOST:PORT");
    }

    offload_changed = (wp_bool)(strcmp(edit.offload, ctx->settings.offload) != 0);
    old_recovery[0] = '\0';
    if (ctx->recovery_path != NULL && strlen(ctx->recovery_path) < sizeof(old_recovery)) {
        strcpy(old_recovery, ctx->recovery_path);
    }
    strcpy(old_setting, ctx->settings.recovery);
    old_path = old_recovery[0] != '\0' ? old_recovery : NULL;
    ctx->settings = edit;
    wp_settings_apply(ctx);
    if (old_path != NULL && (ctx->recovery_path == NULL || strcmp(old_path, ctx->recovery_path) != 0) &&
        !move_recovery(ctx, old_path)) {
        /* The copy could not go there: keep it where it is. */
        strcpy(ctx->settings.recovery, old_setting);
        wp_settings_apply(ctx);
        wp_action_tell(ctx, "Couldn't write the recovery file there; kept the old one");
    }
    if (offload_changed) {
        offload_shutdown();
        ctx->offload_retry = WP_TRUE;
    }
    if (ctx->settings_changed != NULL) ctx->settings_changed(ctx);
    if (ctx->config_path != NULL &&
        wp_config_save_settings(ctx->config_path, &ctx->settings) != WP_OK) {
        wp_action_tell(ctx, "Settings used, but couldn't write the config file");
    }
    return WP_TRUE;
}
