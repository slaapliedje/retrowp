/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Settings (settings.h, M21 T21.4): the dialog through the host's
   plat_dialog_form, what an answer changes, the configuration file it
   writes, and the backups it turns on. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "test.h"
#include "settings.h"
#include "config.h"
#include "recovery.h"
#include "autosave.h"
#include "app_ctx.h"
#include "actions.h"
#include "docmodel.h"
#include "format_txt.h"
#include "window_host.h"
#include "offload.h"
#include <stdlib.h>

#define ST_DIR  "build/settings"
#define ST_CFG  ST_DIR "/RETROWP.CFG"
#define ST_REC  ST_DIR "/RETROWP.REC"
#define ST_REC2 ST_DIR "/ELSE.REC"
#define ST_DOC  ST_DIR "/NOTE.TXT"
#define ST_BAK  ST_DIR "/NOTE.BAK"

/* The form's fields, in settings.c's order. */
enum { F_DOCDIR, F_DATADIR, F_RECOVERY, F_AUTOSAVE, F_BACKUP, F_PORT, F_SPEED, F_ADDRESS, F_PICOPY, F_MACHINE };

/* The Port list with no serial ports listed (the host's default):
   Off, Serial (the one AUX), Network. */
enum { P_OFF, P_SERIAL, P_NET };

static wp_bool there(const char *path)
{
    wp_bool t = WP_FALSE;
    return (wp_bool)(plat_file_exists(path, &t) == WP_OK && t);
}

static void gone(const char *path)
{
    if (there(path)) (void)plat_file_delete(path);
}

static void setup(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    (void)mkdir(ST_DIR, 0777);
    gone(ST_CFG); gone(ST_REC); gone(ST_REC2); gone(ST_DOC); gone(ST_BAK);
    ASSERT_TRUE(format_txt_import((const u8 *)"Hello\n", 6, doc) == WP_OK);
    wp_undo_journal_init(journal);
    wp_app_ctx_init(ctx, WP_KEYMAP_VI, doc, journal);
    ctx->doc_loaded = WP_TRUE;
    ctx->config_path = ST_CFG;
    ctx->recovery_default = ST_REC;
    ctx->offload_default = "serial:aux:9600";
    ctx->path_sep = '\\';
    wp_settings_apply(ctx);
}

static void teardown(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    offload_set_speed_limit(0);         /* wp_settings_apply set it for everyone */
    wp_keymap_free(ctx);
    wp_undo_journal_free(journal);
    wp_document_free(doc);
    gone(ST_CFG); gone(ST_REC); gone(ST_REC2); gone(ST_DOC); gone(ST_BAK);
}

static u32 read_file(const char *path, char *out, u32 cap)
{
    FILE *fp = fopen(path, "rb");
    u32 n = 0;
    if (fp != NULL) {
        n = (u32)fread(out, 1, cap - 1, fp);
        fclose(fp);
    }
    out[n] = '\0';
    return n;
}

/* OK with changes: they are in effect, and in the configuration file,
   from which they read back the same. */
void test_settings_ok_applies_and_writes_the_config(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_config cfg;
    char text[512];
    u32 n;

    setup(&doc, &journal, &ctx);
    window_host_form_edit(F_DOCDIR, "C:\\LETTERS", -1);
    window_host_form_edit(F_DATADIR, "  C:\\DATA  ", -1);
    window_host_form_edit(F_AUTOSAVE, NULL, 4);          /* 10 minutes */
    window_host_form_edit(F_BACKUP, NULL, 1);
    window_host_form_edit(F_PORT, NULL, P_NET);
    window_host_form_edit(F_ADDRESS, " pi:6809 ", -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("Settings", window_host_last_title());
    ASSERT_EQ_STR("C:\\LETTERS", ctx.settings.docdir);
    ASSERT_EQ_STR("C:\\DATA", ctx.settings.datadir);     /* trimmed */
    ASSERT_EQ_U32(10, ctx.settings.autosave_minutes);
    ASSERT_EQ_U32(10, ctx.autosave.minutes);
    ASSERT_EQ_U32(1, ctx.settings.backup);
    ASSERT_EQ_STR("tcp:pi:6809", wp_settings_offload(&ctx));
    ASSERT_TRUE(ctx.offload_retry);

    n = read_file(ST_CFG, text, (u32)sizeof(text));
    ASSERT_TRUE(n > 0);
    ASSERT_TRUE(strstr(text, "docdir=C:\\LETTERS\n") != NULL);
    ASSERT_TRUE(strstr(text, "autosave=10\n") != NULL);
    ASSERT_TRUE(strstr(text, "backup=on\n") != NULL);
    ASSERT_TRUE(wp_config_parse((const u8 *)text, n, &cfg) == WP_OK);
    ASSERT_EQ_STR("C:\\LETTERS", cfg.settings.docdir);
    ASSERT_EQ_STR("C:\\DATA", cfg.settings.datadir);
    ASSERT_EQ_STR("", cfg.settings.recovery);
    ASSERT_EQ_STR("tcp:pi:6809", cfg.settings.offload);
    ASSERT_EQ_U32(10, cfg.settings.autosave_minutes);
    ASSERT_EQ_U32(1, cfg.settings.backup);
    teardown(&doc, &journal, &ctx);
}

/* Cancel changes nothing and writes nothing. */
void test_settings_cancel_changes_nothing(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    window_host_form_edit(F_DOCDIR, "C:\\LETTERS", -1);
    window_host_form_cancel();
    ASSERT_TRUE(!wp_action_settings(&ctx));
    ASSERT_EQ_STR("", ctx.settings.docdir);
    ASSERT_TRUE(!there(ST_CFG));
    ASSERT_TRUE(!ctx.offload_retry);
    teardown(&doc, &journal, &ctx);
}

/* Network with no address: said so, and the dialog opens again with
   what was chosen, to be put right there. */
void test_settings_bad_offload_link_asks_again(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    u32 forms;

    setup(&doc, &journal, &ctx);
    forms = window_host_form_count();
    window_host_form_edit(F_PORT, NULL, P_NET);
    window_host_form_edit(F_DOCDIR, "A:\\", -1);
    window_host_form_next();
    window_host_form_edit(F_ADDRESS, "pi.local:6809", -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_U32(forms + 2, window_host_form_count());
    ASSERT_EQ_STR("A:\\", ctx.settings.docdir);          /* kept from the first */
    ASSERT_EQ_STR("tcp:pi.local:6809", ctx.settings.offload);
    teardown(&doc, &journal, &ctx);
}

/* T21.7: a machine with several ports. /dev/ttyA is the one "aux" means
   now, /dev/ttyB has rates that need RTS/CTS, /dev/ttyC is held by the
   system. Port list: Off, ttyA, ttyB, ttyC, Network; Speed: Fastest,
   Fastest RTS/CTS, then Up to 57600 ... 9600. */
enum { PP_A = 1, PP_B, PP_C, PP_NET };
enum { S_FASTEST, S_FASTEST_HARD, S_57600, S_38400, S_19200, S_9600 };
#define THREE_PORTS "/dev/ttyA=c,/dev/ttyB=h,/dev/ttyC=b"

/* OK without touching the link keeps it "" -- the machine's own -- and
   writes no limit. */
void test_settings_link_unchanged_stays_the_default(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[512];

    setenv("RP_HOST_SERIAL_PORTS", THREE_PORTS, 1);
    setup(&doc, &journal, &ctx);
    window_host_form_edit(F_DOCDIR, "C:\\", -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("", ctx.settings.offload);
    ASSERT_EQ_U32(0, ctx.settings.link_max);
    ASSERT_TRUE(!ctx.offload_retry);
    (void)read_file(ST_CFG, text, (u32)sizeof(text));
    ASSERT_TRUE(strstr(text, "offload=\n") != NULL);
    ASSERT_TRUE(strstr(text, "linkmax=\n") != NULL);
    unsetenv("RP_HOST_SERIAL_PORTS");
    teardown(&doc, &journal, &ctx);
}

/* Another port, its fastest with RTS/CTS: the port's own name, the start
   speed kept, ":hard". */
void test_settings_link_port_and_rts_cts(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setenv("RP_HOST_SERIAL_PORTS", THREE_PORTS, 1);
    setup(&doc, &journal, &ctx);
    window_host_form_edit(F_PORT, NULL, PP_B);
    window_host_form_edit(F_SPEED, NULL, S_FASTEST_HARD);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("serial:/dev/ttyB:9600:hard", ctx.settings.offload);
    ASSERT_EQ_U32(0, ctx.settings.link_max);
    ASSERT_TRUE(ctx.offload_retry);
    unsetenv("RP_HOST_SERIAL_PORTS");
    teardown(&doc, &journal, &ctx);
}

/* A limit below the rates that need RTS/CTS: no ":hard", linkmax= in
   the configuration file, read back. Reopened, the dialog shows it. */
void test_settings_link_speed_limit(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_config cfg;
    char text[512];
    u32 n;

    setenv("RP_HOST_SERIAL_PORTS", THREE_PORTS, 1);
    setup(&doc, &journal, &ctx);
    window_host_form_edit(F_PORT, NULL, PP_B);
    window_host_form_edit(F_SPEED, NULL, S_57600);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("serial:/dev/ttyB:9600", ctx.settings.offload);
    ASSERT_EQ_U32(57600, ctx.settings.link_max);
    n = read_file(ST_CFG, text, (u32)sizeof(text));
    ASSERT_TRUE(strstr(text, "linkmax=57600\n") != NULL);
    ASSERT_TRUE(wp_config_parse((const u8 *)text, n, &cfg) == WP_OK);
    ASSERT_EQ_U32(57600, cfg.settings.link_max);
    /* Opened again and OK'd untouched: the same link and limit. */
    ctx.offload_retry = WP_FALSE;
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("serial:/dev/ttyB:9600", ctx.settings.offload);
    ASSERT_EQ_U32(57600, ctx.settings.link_max);
    ASSERT_TRUE(!ctx.offload_retry);
    unsetenv("RP_HOST_SERIAL_PORTS");
    teardown(&doc, &journal, &ctx);
}

/* A port the system holds is refused, named, and the dialog opens again;
   Off is "off". */
void test_settings_link_busy_port_and_off(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    u32 forms;

    setenv("RP_HOST_SERIAL_PORTS", THREE_PORTS, 1);
    setup(&doc, &journal, &ctx);
    forms = window_host_form_count();
    window_host_form_edit(F_PORT, NULL, PP_C);
    window_host_form_next();
    window_host_form_edit(F_PORT, NULL, 0);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_U32(forms + 2, window_host_form_count());
    ASSERT_EQ_STR("off", ctx.settings.offload);
    unsetenv("RP_HOST_SERIAL_PORTS");
    teardown(&doc, &journal, &ctx);
}

/* A link the lists cannot show (the host's pipe:) is offered as itself
   and kept through OK. */
void test_settings_link_unlisted_survives_ok(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    strcpy(ctx.settings.offload, "pipe:build/settings/x");
    window_host_form_edit(F_DOCDIR, "C:\\", -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("pipe:build/settings/x", ctx.settings.offload);
    ASSERT_TRUE(!ctx.offload_retry);
    teardown(&doc, &journal, &ctx);
}

/* A machine with no TCP: no Network in the Port list and no address
   field (nine fields); a tcp: link from the file is offered as itself
   and kept, and a port can still be picked. */
void test_settings_link_without_network(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const plat_form_field *f;
    u32 n = 0, i;

    setenv("RP_HOST_SERIAL_PORTS", THREE_PORTS, 1);
    setenv("RP_HOST_NO_NETWORK", "1", 1);
    setup(&doc, &journal, &ctx);
    strcpy(ctx.settings.offload, "tcp:pi:6809");
    ASSERT_TRUE(wp_action_settings(&ctx));
    f = window_host_form_last(&n);
    ASSERT_TRUE(f != NULL);
    ASSERT_EQ_U32(9, n);
    for (i = 0; i < n; i++) ASSERT_TRUE(strcmp(f[i].label, "Network address") != 0);
    ASSERT_EQ_STR("Name on the Pi", f[8].label);
    ASSERT_EQ_U32(5, f[F_PORT].option_count);        /* Off, A, B, C, the tcp: link */
    for (i = 0; i < f[F_PORT].option_count; i++) ASSERT_TRUE(strcmp(f[F_PORT].options[i], "Network") != 0);
    ASSERT_EQ_STR("tcp:pi:6809", f[F_PORT].options[4]);
    ASSERT_EQ_U32(4, f[F_PORT].index);
    ASSERT_EQ_STR("tcp:pi:6809", ctx.settings.offload);

    window_host_form_edit(F_PORT, NULL, PP_B);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR("serial:/dev/ttyB:9600", ctx.settings.offload);
    unsetenv("RP_HOST_NO_NETWORK");
    unsetenv("RP_HOST_SERIAL_PORTS");
    teardown(&doc, &journal, &ctx);
}

/* A value from the configuration file the list does not have stays
   itself through OK. */
void test_settings_unlisted_autosave_survives_ok(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    ctx.settings.autosave_minutes = 7;
    wp_settings_apply(&ctx);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_U32(7, ctx.settings.autosave_minutes);
    window_host_form_edit(F_AUTOSAVE, NULL, 0);           /* Off */
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_U32(0, ctx.autosave.minutes);
    teardown(&doc, &journal, &ctx);
}

/* The recovery file moves with its copy: written at the new place
   before the old one goes. */
void test_settings_recovery_file_moves_with_its_copy(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, wp_pos_doc_start(&doc),
                                    (const u8 *)"Work ", 5, NULL) == WP_OK);
    ASSERT_TRUE(wp_recovery_write(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(there(ST_REC));
    window_host_form_edit(F_RECOVERY, ST_REC2, -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR(ST_REC2, ctx.recovery_path);
    ASSERT_TRUE(there(ST_REC2));
    ASSERT_TRUE(!there(ST_REC));

    /* Back to the default: empty means the shell's own. */
    window_host_form_edit(F_RECOVERY, "", -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_STR(ST_REC, ctx.recovery_path);
    ASSERT_TRUE(there(ST_REC));
    ASSERT_TRUE(!there(ST_REC2));
    teardown(&doc, &journal, &ctx);
}

/* The data folder: a file in it, joined by the machine's separator;
   none set, and the shell keeps looking where it always has. */
void test_settings_data_file_joins_the_folder(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char out[64];

    setup(&doc, &journal, &ctx);
    strcpy(out, "untouched");
    ASSERT_TRUE(!wp_settings_data_file(&ctx, "WPSPELL.DIC", out, (u32)sizeof(out)));
    ASSERT_EQ_STR("untouched", out);
    strcpy(ctx.settings.datadir, "C:\\DATA");
    ASSERT_TRUE(wp_settings_data_file(&ctx, "WPSPELL.DIC", out, (u32)sizeof(out)));
    ASSERT_EQ_STR("C:\\DATA\\WPSPELL.DIC", out);
    strcpy(ctx.settings.datadir, "Work:");
    ctx.path_sep = '/';
    ASSERT_TRUE(wp_settings_data_file(&ctx, "WPSPELL.DIC", out, (u32)sizeof(out)));
    ASSERT_EQ_STR("Work:WPSPELL.DIC", out);
    ASSERT_TRUE(!wp_settings_data_file(&ctx, "WPSPELL.DIC", out, 8u));
    teardown(&doc, &journal, &ctx);
}

/* Backups on: a save keeps the copy it replaces as NAME.BAK. Off: no
   .BAK appears. */
void test_settings_backup_keeps_the_previous_copy(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[32];

    setup(&doc, &journal, &ctx);
    ASSERT_TRUE(wp_action_write_file(&ctx, ST_DOC, (const u8 *)"one\n", 4, WP_TRUE) == WP_OK);
    ASSERT_TRUE(wp_action_write_file(&ctx, ST_DOC, (const u8 *)"two\n", 4, WP_TRUE) == WP_OK);
    ASSERT_TRUE(!there(ST_BAK));
    ctx.settings.backup = 1;
    ASSERT_TRUE(wp_action_write_file(&ctx, ST_DOC, (const u8 *)"three\n", 6, WP_TRUE) == WP_OK);
    (void)read_file(ST_BAK, text, (u32)sizeof(text));
    ASSERT_EQ_STR("two\n", text);
    ASSERT_TRUE(wp_action_write_file(&ctx, ST_DOC, (const u8 *)"four\n", 5, WP_TRUE) == WP_OK);
    (void)read_file(ST_BAK, text, (u32)sizeof(text));
    ASSERT_EQ_STR("three\n", text);
    (void)read_file(ST_DOC, text, (u32)sizeof(text));
    ASSERT_EQ_STR("four\n", text);
    ASSERT_TRUE(!there(ST_DIR "/RETROWP.OLD"));
    /* The recovery file never has one. */
    ASSERT_TRUE(wp_recovery_write(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(wp_recovery_write(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(!there(ST_DIR "/RETROWP.BAK"));
    teardown(&doc, &journal, &ctx);
}

/* The configuration file's own spelling: off/on, and a text too long to
   be a whole path is ignored rather than cut short. */
void test_settings_config_keys(void)
{
    wp_config cfg;
    static const char a[] =
        "backup=on\r\nrecovery=D:\\R.REC\r\n"
        "docdir=C:\\AN\\EXTREMELY\\LONG\\FOLDER\\NAME\\THAT\\DOES\\NOT\\FIT\r\n";

    ASSERT_TRUE(wp_config_parse((const u8 *)"", 0, &cfg) == WP_OK);
    ASSERT_EQ_U32(0, cfg.settings.backup);
    ASSERT_EQ_STR("", cfg.settings.docdir);
    ASSERT_TRUE(wp_config_parse((const u8 *)a, (u32)strlen(a), &cfg) == WP_OK);
    ASSERT_EQ_U32(1, cfg.settings.backup);
    ASSERT_EQ_STR("D:\\R.REC", cfg.settings.recovery);
    ASSERT_EQ_STR("", cfg.settings.docdir);
}

/* The Pi's two (T21.6): Copy to the Pi and the machine's name, set in
   the dialog, written to the configuration file and read back; an empty
   name is the port's own. */
void test_settings_pi_copy_and_machine_name(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_config cfg;
    char text[512];
    u32 n;

    setup(&doc, &journal, &ctx);
    ctx.machine_default = "ST";
    ASSERT_EQ_STR("ST", wp_settings_machine(&ctx));
    window_host_form_edit(F_PICOPY, NULL, 1);
    window_host_form_edit(F_MACHINE, " STUDY ", -1);
    ASSERT_TRUE(wp_action_settings(&ctx));
    ASSERT_EQ_U32(1, ctx.settings.pi_copy);
    ASSERT_EQ_STR("STUDY", wp_settings_machine(&ctx));      /* trimmed */
    n = read_file(ST_CFG, text, (u32)sizeof(text));
    ASSERT_TRUE(strstr(text, "picopy=on\n") != NULL);
    ASSERT_TRUE(strstr(text, "machine=STUDY\n") != NULL);
    ASSERT_TRUE(wp_config_parse((const u8 *)text, n, &cfg) == WP_OK);
    ASSERT_EQ_U32(1, cfg.settings.pi_copy);
    ASSERT_EQ_STR("STUDY", cfg.settings.machine);
    ASSERT_TRUE(wp_config_parse((const u8 *)"picopy=off\nmachine=\n", 20, &cfg) == WP_OK);
    ASSERT_EQ_U32(0, cfg.settings.pi_copy);
    ASSERT_EQ_STR("", cfg.settings.machine);
    teardown(&doc, &journal, &ctx);
}
