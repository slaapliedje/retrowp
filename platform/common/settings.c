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
    s->link_max = 0;
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
    offload_set_speed_limit(ctx->settings.link_max);
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

/* -- The link: Port, Speed, Network address (M21 T21.7) ---------------

   The offload= string keeps the shape it always had -- serial:<port>:
   <baud>[:hard], tcp:<host>:<port>, or off -- and the dialog builds it
   from choices filled from what the machine really has
   (plat_serial_ports). The <baud> is where the link starts, and stays
   what it was: the gateway listens there, and OFFLOAD_SET_SPEED raises
   it. Speed is how far up it may go (linkmax=), and whether RTS/CTS is
   on, which the fastest rates of some ports need.

   A form's choices cannot change while it is open, so Speed lists every
   rate any port reaches; a rate the chosen port does not reach is
   refused on OK, naming the port's own top speed. */

#define LINK_PORTS_MAX  8
#define LINK_SPEEDS_MAX 10

static plat_serial_port g_ports[LINK_PORTS_MAX];
static u32 g_port_n;
static const char *g_port_labels[LINK_PORTS_MAX + 4];
static u32 g_port_choices;
static u32 g_port_net;                 /* index of "Network" */
static u32 g_port_other;               /* index of the as-it-was entry, or ~0 */
static wp_bool g_net;                  /* plat_network_available(): Network offered */
static char g_other_label[WP_SETTINGS_TEXT];

static u32 g_speeds[LINK_SPEEDS_MAX];  /* index -> cap; 0 = fastest */
static u8  g_speed_hard[LINK_SPEEDS_MAX];
static const char *g_speed_labels[LINK_SPEEDS_MAX];
static char g_speed_text[LINK_SPEEDS_MAX][16];
static u32 g_speed_choices;

static char g_address[WP_SETTINGS_TEXT];

/* The parsed link as the dialog opened: its port index, its device and
   start speed, whether RTS/CTS was on. */
static u32  g_open_port;
static char g_open_dev[PLAT_PORT_NAME_MAX];
static char g_open_baud[12];
static wp_bool g_open_hard;

static char lower_c(char c) { return (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); }

static wp_bool same_nocase(const char *a, const char *b, u32 blen)
{
    u32 i;
    for (i = 0; i < blen; i++) if (a[i] == '\0' || lower_c(a[i]) != lower_c(b[i])) return WP_FALSE;
    return (wp_bool)(a[blen] == '\0');
}

/* A port's own top rate and the slowest needing RTS/CTS. */
static u32 port_rates(u32 port, u32 *rates, u32 cap, u32 *hard_from)
{
    const char *name = g_port_n > 0 ? g_ports[port].name : "aux";
    return plat_serial_port_speeds(name, rates, cap, hard_from);
}

static void add_speed(u32 cap, wp_bool hard, const char *label)
{
    u32 i;
    if (g_speed_choices >= LINK_SPEEDS_MAX) return;
    for (i = 0; i < g_speed_choices; i++) if (g_speeds[i] == cap && g_speed_hard[i] == (u8)hard) return;
    g_speeds[g_speed_choices] = cap;
    g_speed_hard[g_speed_choices] = (u8)hard;
    if (label != NULL) {
        g_speed_labels[g_speed_choices] = label;
    } else {
        u32 n;
        memcpy(g_speed_text[g_speed_choices], "Up to ", 6);
        n = put_u(g_speed_text[g_speed_choices] + 6, cap);
        WP_UNUSED(n);
        g_speed_labels[g_speed_choices] = g_speed_text[g_speed_choices];
    }
    g_speed_choices++;
}

/* Fills the Port and Speed lists, and reads `link` (the link in use)
   into them: *port_ix and *speed_ix answer where it stands. */
static void link_lists(const char *link, u32 link_max, u32 *port_ix, u32 *speed_ix)
{
    u32 rates[LINK_SPEEDS_MAX], all[LINK_SPEEDS_MAX * 2];
    u32 i, j, k, n, all_n = 0, hard_from, any_hard = 0, ports;

    g_port_n = plat_serial_ports(g_ports, LINK_PORTS_MAX);
    ports = g_port_n > 0 ? g_port_n : 1u;
    g_port_labels[0] = "Off";
    for (i = 0; i < ports; i++) g_port_labels[1 + i] = g_port_n > 0 ? g_ports[i].label : "Serial";
    g_net = plat_network_available();
    if (g_net) {
        g_port_net = 1 + ports;
        g_port_labels[g_port_net] = "Network";
        g_port_choices = g_port_net + 1;
    } else {                               /* no TCP here: no Network, no address */
        g_port_net = ~(u32)0;
        g_port_choices = 1 + ports;
    }
    g_port_other = ~(u32)0;

    /* Every rate any port reaches, fastest first, without repeats. */
    for (i = 0; i < ports; i++) {
        n = port_rates(i, rates, LINK_SPEEDS_MAX, &hard_from);
        if (hard_from != 0) any_hard = 1;
        for (j = 0; j < n; j++) {
            for (k = 0; k < all_n && all[k] != rates[j]; k++) { }
            if (k == all_n && all_n < LINK_SPEEDS_MAX * 2) all[all_n++] = rates[j];
        }
    }
    for (i = 1; i < all_n; i++) {                   /* a few entries: insertion sort */
        u32 v = all[i];
        for (j = i; j > 0 && all[j - 1] < v; j--) all[j] = all[j - 1];
        all[j] = v;
    }
    g_speed_choices = 0;
    add_speed(0, WP_FALSE, "Fastest");
    if (any_hard) add_speed(0, WP_TRUE, "Fastest, RTS/CTS");
    for (i = 1; i < all_n; i++) add_speed(all[i], WP_FALSE, NULL);   /* the top is "Fastest" */

    /* Where the link in use stands. */
    *port_ix = 0;
    g_open_dev[0] = '\0';
    strcpy(g_open_baud, "9600");
    g_open_hard = WP_FALSE;
    g_address[0] = '\0';
    if (link[0] == '\0' || strcmp(link, "off") == 0) {
        *port_ix = 0;
    } else if (g_net && starts(link, "tcp:") && link[4] != '\0') {
        *port_ix = g_port_net;
        if (strlen(link + 4) < sizeof(g_address)) strcpy(g_address, link + 4);
    } else if (starts(link, "serial:")) {
        const char *rest = link + 7, *end = rest + strlen(rest), *c;
        u32 dl;

        if (end - rest > 5 && strcmp(end - 5, ":hard") == 0) { g_open_hard = WP_TRUE; end -= 5; }
        else if (end - rest > 5 && strcmp(end - 5, ":none") == 0) end -= 5;
        for (c = end; c > rest && c[-1] != ':'; c--) { }
        dl = c > rest ? (u32)(c - 1 - rest) : 0u;
        if (dl > 0 && dl < sizeof(g_open_dev) && (u32)(end - c) < sizeof(g_open_baud) && end > c) {
            memcpy(g_open_dev, rest, dl);
            g_open_dev[dl] = '\0';
            memcpy(g_open_baud, c, (u32)(end - c));
            g_open_baud[end - c] = '\0';
            if (g_port_n == 0) {
                *port_ix = 1;
            } else {
                for (i = 0; i < g_port_n; i++) {
                    u32 nl = (u32)strlen(g_ports[i].name);
                    if (same_nocase(g_ports[i].name, g_open_dev, dl) ||
                        (same_nocase("aux", g_open_dev, dl) && (g_ports[i].flags & PLAT_PORT_CURRENT)) ||
                        /* DOS's older spelling: 0-3 for COM1-COM4. */
                        (dl == 1 && g_open_dev[0] >= '0' && g_open_dev[0] <= '8' && nl > 0 &&
                         g_ports[i].name[nl - 1] == g_open_dev[0] + 1))
                        *port_ix = 1 + i;
                }
                if (*port_ix == 0 && same_nocase("aux", g_open_dev, dl)) *port_ix = 1;
            }
        }
    }
    if (*port_ix == 0 && link[0] != '\0' && strcmp(link, "off") != 0) {
        /* Something the lists cannot show (a MiNT device path, the host's
           pipe:): offered as itself, so OK keeps it. */
        g_port_other = g_port_choices;
        strncpy(g_other_label, link, sizeof(g_other_label) - 1);
        g_other_label[sizeof(g_other_label) - 1] = '\0';
        g_port_labels[g_port_other] = g_other_label;
        g_port_choices++;
        *port_ix = g_port_other;
    }

    if (link_max != 0) add_speed(link_max, WP_FALSE, NULL);
    *speed_ix = 0;
    for (i = 0; i < g_speed_choices; i++) {
        if (g_speeds[i] == link_max && (link_max != 0 || g_speed_hard[i] == (u8)g_open_hard)) {
            *speed_ix = i;
            break;
        }
    }
}

/* The link the answer names, into `out`; NULL if it is fine, else what
   to tell the writer. `cap_out` gets linkmax=. */
static const char *link_answer(u32 port_ix, u32 speed_ix, const char *address,
                               char *out, u32 out_cap, u32 *cap_out)
{
    static char why[64];
    u32 rates[LINK_SPEEDS_MAX], n, hard_from, cap, k;
    wp_bool hard;
    const char *dev;

    cap = speed_ix < g_speed_choices ? g_speeds[speed_ix] : 0;
    *cap_out = cap;
    if (port_ix == 0) { strcpy(out, "off"); return NULL; }
    if (port_ix == g_port_other) { return NULL; }           /* kept as it was */
    if (port_ix == g_port_net) {
        if (address[0] == '\0' || strchr(address, ':') == NULL)
            return "Network address: HOST:PORT";
        if (4u + (u32)strlen(address) + 1u > out_cap) return "Network address: too long";
        strcpy(out, "tcp:");
        strcat(out, address);
        return NULL;
    }
    k = port_ix - 1;
    if (g_port_n > 0 && (g_ports[k].flags & PLAT_PORT_BUSY)) {
        strcpy(why, g_ports[k].label);
        strcat(why, " is in use by the system");
        return why;
    }
    n = port_rates(k, rates, LINK_SPEEDS_MAX, &hard_from);
    if (n > 0 && cap > rates[0]) {
        u32 l;
        strcpy(why, g_port_labels[port_ix]);
        strcat(why, " goes up to ");
        l = (u32)strlen(why);
        (void)put_u(why + l, rates[0]);
        return why;
    }
    hard = (wp_bool)(hard_from != 0 &&
                     ((cap == 0 && speed_ix < g_speed_choices && g_speed_hard[speed_ix]) ||
                      (cap != 0 && cap >= hard_from)));
    /* The device as it was written when the port is unchanged ("aux"
       stays "aux"), else the port's own name. */
    dev = (port_ix == g_open_port && g_open_dev[0] != '\0') ? g_open_dev
          : (g_port_n > 0 ? g_ports[k].name : "aux");
    if (7u + (u32)strlen(dev) + 1u + (u32)strlen(g_open_baud) + 6u > out_cap) return "Port name too long";
    strcpy(out, "serial:");
    strcat(out, dev);
    strcat(out, ":");
    strcat(out, g_open_baud);
    if (hard) strcat(out, ":hard");
    return NULL;
}

enum { F_DOCDIR, F_DATADIR, F_RECOVERY, F_AUTOSAVE, F_BACKUP, F_PORT, F_SPEED, F_ADDRESS, F_PICOPY, F_MACHINE, F_COUNT };

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
    /* Static: ten fields and a working copy are a lot for a 4 KB Amiga
       stack (and the dialog backends need some of it too). */
    static wp_settings edit;
    static plat_form_field fields[F_COUNT];
    static char old_recovery[WP_APP_PATH_MAX];
    static char old_setting[WP_SETTINGS_TEXT];
    static char link[WP_SETTINGS_TEXT];
    static const char title[] = "Settings";
    u32 i, mi, choices, port_ix, speed_ix, cap, nf;
    wp_bool cancelled = WP_FALSE, offload_changed;
    const char *old_path, *why, *cur;

    edit = ctx->settings;
    cur = wp_settings_offload(ctx);
    if (cur == NULL || strlen(cur) >= sizeof(link)) cur = "";
    link_lists(cur, edit.link_max, &port_ix, &speed_ix);
    g_open_port = port_ix;
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
        choice_field(&fields[F_PORT], "Pi link", (const char *const *)g_port_labels, g_port_choices, port_ix);
        choice_field(&fields[F_SPEED], "Link speed", (const char *const *)g_speed_labels, g_speed_choices, speed_ix);
        text_field(&fields[F_ADDRESS], "Network address", g_address);
        choice_field(&fields[F_PICOPY], "Copy to the Pi", k_picopy_labels, 2u, edit.pi_copy ? 1u : 0u);
        text_field(&fields[F_MACHINE], "Name on the Pi", edit.machine);

        /* Without TCP the address field is left out: the fields after
           it move up one for the form, and back for reading. */
        nf = (u32)F_COUNT;
        if (!g_net) {
            for (i = F_ADDRESS; i + 1 < (u32)F_COUNT; i++) fields[i] = fields[i + 1];
            nf--;
        }
        if (plat_dialog_form(ctx->window, (const u8 *)title, (u32)(sizeof(title) - 1),
                             fields, nf, &cancelled) != WP_OK || cancelled) {
            return WP_FALSE;
        }
        if (!g_net)
            for (i = (u32)F_COUNT - 1; i > F_ADDRESS; i--) fields[i] = fields[i - 1];
        trim_text(edit.docdir);
        trim_text(edit.datadir);
        trim_text(edit.recovery);
        trim_text(g_address);
        trim_text(edit.machine);
        edit.pi_copy = (u8)(fields[F_PICOPY].index == 1u ? 1 : 0);
        mi = fields[F_AUTOSAVE].index;
        if (mi < K_MINUTES_N) edit.autosave_minutes = k_minutes[mi];   /* else: as it was */
        edit.backup = (u8)(fields[F_BACKUP].index == 1u ? 1 : 0);
        port_ix = fields[F_PORT].index;
        speed_ix = fields[F_SPEED].index;
        strcpy(link, cur);
        why = link_answer(port_ix, speed_ix, g_address, link, (u32)sizeof(link), &cap);
        if (why == NULL) {
            /* The machine's own link, chosen again, stays "" -- so a later
               build's own default still applies to it. */
            if (ctx->offload_default != NULL && strcmp(link, ctx->offload_default) == 0) link[0] = '\0';
            strcpy(edit.offload, link);
            edit.link_max = cap;
            break;
        }
        wp_action_tell(ctx, why);
    }

    offload_changed = (wp_bool)(strcmp(edit.offload, ctx->settings.offload) != 0 ||
                                edit.link_max != ctx->settings.link_max);
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
