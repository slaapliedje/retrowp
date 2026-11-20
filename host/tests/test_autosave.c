/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Autosave (autosave.h, M21 T21.3): unsaved work copied to the recovery
   file once the interval has run and typing has paused -- never to the
   document. The clock is moved with the host timer's bias hook. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "test.h"
#include "autosave.h"
#include "recovery.h"
#include "config.h"
#include "app_ctx.h"
#include "actions.h"
#include "docmodel.h"
#include "format_txt.h"
#include "timer_host.h"
#include "window_host.h"

#define AS_DIR  "build/autosave"
#define AS_REC  AS_DIR "/RETROWP.REC"
#define AS_DOC  AS_DIR "/NOTE.TXT"
#define MINUTE  60000uL

static u32 g_bias;

static void clock_on(u32 ms)
{
    g_bias += ms;
    timer_host_set_bias(g_bias);
}

static wp_bool there(const char *path)
{
    wp_bool t = WP_FALSE;
    return (wp_bool)(plat_file_exists(path, &t) == WP_OK && t);
}

static u32 size_of(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? (u32)sb.st_size : 0u;
}

static void setup(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    (void)mkdir(AS_DIR, 0777);
    /* What an aborted run of the failure test leaves behind. */
    (void)remove(AS_DIR "/RETROWP.$$$/KEEP");
    (void)remove(AS_DIR "/RETROWP.$$$");
    if (there(AS_REC)) (void)plat_file_delete(AS_REC);
    ASSERT_TRUE(format_txt_import((const u8 *)"Hello\n", 6, doc) == WP_OK);
    wp_undo_journal_init(journal);
    wp_app_ctx_init(ctx, WP_KEYMAP_VI, doc, journal);
    ctx->doc_loaded = WP_TRUE;
    ctx->recovery_path = AS_REC;
    g_bias = 0;
    timer_host_set_bias(0);
}

static void teardown(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    wp_keymap_free(ctx);
    wp_undo_journal_free(journal);
    wp_document_free(doc);
    if (there(AS_REC)) (void)plat_file_delete(AS_REC);
    timer_host_set_bias(0);
}

static void type(wp_document *doc, wp_undo_journal *journal, const char *s)
{
    ASSERT_TRUE(wp_edit_insert_text(doc, journal, wp_pos_doc_start(doc),
                                    (const u8 *)s, (u32)strlen(s), NULL) == WP_OK);
}

/* Nothing unsaved: nothing is written and nothing is waited for. */
void test_autosave_nothing_unsaved_writes_nothing(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    clock_on(60 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(!there(AS_REC));
    teardown(&doc, &journal, &ctx);
}

/* A change waits out the interval, then goes to the recovery file -- the
   document stays unsaved, and an unchanged document is not written
   again. */
void test_autosave_writes_the_copy_after_the_interval(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    u32 wait;
    char path[64];

    setup(&doc, &journal, &ctx);
    wp_action_remember_path(&ctx, AS_DOC);
    type(&doc, &journal, "Changed ");
    wait = wp_autosave_poll(&ctx, NULL);
    ASSERT_TRUE(wait > 4 * MINUTE && wait <= 5 * MINUTE);
    ASSERT_TRUE(!there(AS_REC));
    clock_on(4 * MINUTE);
    ASSERT_TRUE(wp_autosave_poll(&ctx, NULL) > 0);
    ASSERT_TRUE(!there(AS_REC));
    clock_on(MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(there(AS_REC));
    ASSERT_TRUE(!there(AS_DOC));                     /* never the document */
    ASSERT_TRUE(wp_document_is_modified(&doc));      /* still unsaved */
    ASSERT_TRUE(wp_recovery_pending(&ctx, path, (u32)sizeof(path), NULL));
    ASSERT_TRUE(strcmp(path, AS_DOC) == 0);
    ASSERT_TRUE(!there(AS_DIR "/RETROWP.$$$"));

    /* Caught up: deleting the copy behind its back shows it is not
       rewritten until something changes again. */
    (void)plat_file_delete(AS_REC);
    clock_on(10 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(!there(AS_REC));
    type(&doc, &journal, "Again ");
    ASSERT_TRUE(wp_autosave_poll(&ctx, NULL) > 0);
    clock_on(5 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(there(AS_REC));
    teardown(&doc, &journal, &ctx);
}

/* Due, but the writer is still typing: it waits for the pause. */
void test_autosave_waits_for_typing_to_pause(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    u32 wait;

    setup(&doc, &journal, &ctx);
    type(&doc, &journal, "One ");
    (void)wp_autosave_poll(&ctx, NULL);
    clock_on(5 * MINUTE);
    type(&doc, &journal, "two ");
    wait = wp_autosave_poll(&ctx, NULL);
    ASSERT_TRUE(wait > 0 && wait <= WP_AUTOSAVE_PAUSE_MS);
    ASSERT_TRUE(!there(AS_REC));
    clock_on(WP_AUTOSAVE_PAUSE_MS);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(there(AS_REC));
    teardown(&doc, &journal, &ctx);
}

/* Off (autosave=off) and no recovery file: never. A save settles it. */
void test_autosave_off_and_after_a_save(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    ctx.autosave.minutes = 0;
    type(&doc, &journal, "x");
    clock_on(60 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(!there(AS_REC));

    ctx.autosave.minutes = 5;
    ctx.recovery_path = NULL;
    clock_on(60 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ctx.recovery_path = AS_REC;

    /* Written, then saved: the copy goes, and nothing more is written. */
    (void)wp_autosave_poll(&ctx, NULL);
    clock_on(5 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(there(AS_REC));
    ASSERT_TRUE(wp_action_write_file(&ctx, AS_DOC, (const u8 *)"x\n", 2, WP_TRUE) == WP_OK);
    ASSERT_TRUE(!there(AS_REC));
    clock_on(60 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(!there(AS_REC));
    (void)plat_file_delete(AS_DOC);
    teardown(&doc, &journal, &ctx);
}

/* A copy that cannot be written keeps the last good one, says so once,
   and is tried again an interval later. */
void test_autosave_failure_keeps_the_last_copy(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    u32 good;
    FILE *fp;

    setup(&doc, &journal, &ctx);
    type(&doc, &journal, "First ");
    (void)wp_autosave_poll(&ctx, NULL);
    clock_on(5 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    good = size_of(AS_REC);
    ASSERT_TRUE(good > 0);

    /* A non-empty directory where the temporary file must go. */
    (void)mkdir(AS_DIR "/RETROWP.$$$", 0777);
    fp = fopen(AS_DIR "/RETROWP.$$$/KEEP", "w");
    ASSERT_TRUE(fp != NULL);
    if (fp != NULL) fclose(fp);

    type(&doc, &journal, "Second, much longer than the first ");
    (void)wp_autosave_poll(&ctx, NULL);
    clock_on(5 * MINUTE);
    ASSERT_EQ_U32(5 * MINUTE, wp_autosave_poll(&ctx, NULL));
    ASSERT_EQ_U32(good, size_of(AS_REC));            /* the old copy, whole */
    ASSERT_TRUE(strcmp(window_host_last_title(), WP_AUTOSAVE_FAILED_MSG) == 0);

    wp_action_tell(&ctx, "something else");
    clock_on(5 * MINUTE);
    ASSERT_EQ_U32(5 * MINUTE, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(strcmp(window_host_last_title(), "something else") == 0);  /* once */

    (void)remove(AS_DIR "/RETROWP.$$$/KEEP");
    (void)remove(AS_DIR "/RETROWP.$$$");
    clock_on(5 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(size_of(AS_REC) > good);
    teardown(&doc, &journal, &ctx);
}

/* A recovered document is what the copy holds: nothing to write until
   it changes. */
void test_autosave_recovered_document_is_caught_up(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx);
    type(&doc, &journal, "Work ");
    ASSERT_TRUE(wp_recovery_write(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(wp_recovery_restore(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(ctx.doc));
    (void)plat_file_delete(AS_REC);
    clock_on(60 * MINUTE);
    ASSERT_EQ_U32(0, wp_autosave_poll(&ctx, NULL));
    ASSERT_TRUE(!there(AS_REC));
    teardown(&doc, &journal, &ctx);
}

/* "autosave=" in the config file. */
void test_autosave_config_setting(void)
{
    wp_config cfg;
    static const char a[] = "autosave=off\n";
    static const char b[] = "autosave=10\n";
    static const char c[] = "autosave=9999\n";
    static const char d[] = "autosave=soon\n";

    ASSERT_TRUE(wp_config_parse((const u8 *)"", 0, &cfg) == WP_OK);
    ASSERT_EQ_U32(WP_AUTOSAVE_DEFAULT_MINUTES, cfg.settings.autosave_minutes);
    ASSERT_TRUE(wp_config_parse((const u8 *)a, (u32)strlen(a), &cfg) == WP_OK);
    ASSERT_EQ_U32(0, cfg.settings.autosave_minutes);
    ASSERT_TRUE(wp_config_parse((const u8 *)b, (u32)strlen(b), &cfg) == WP_OK);
    ASSERT_EQ_U32(10, cfg.settings.autosave_minutes);
    ASSERT_TRUE(wp_config_parse((const u8 *)c, (u32)strlen(c), &cfg) == WP_OK);
    ASSERT_EQ_U32(WP_AUTOSAVE_MAX_MINUTES, cfg.settings.autosave_minutes);
    ASSERT_TRUE(wp_config_parse((const u8 *)d, (u32)strlen(d), &cfg) == WP_OK);
    ASSERT_EQ_U32(WP_AUTOSAVE_DEFAULT_MINUTES, cfg.settings.autosave_minutes);
}
