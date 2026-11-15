/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Recovery (recovery.h): the unsaved document kept when the system ends
   RetroWP, and offered back at the next start. The host's
   plat_dialog_choice always takes the default, which for the offer is
   Recover. */
#include <string.h>
#include "test.h"
#include "recovery.h"
#include "app_ctx.h"
#include "actions.h"
#include "docmodel.h"
#include "format_txt.h"

#define REC_PATH "build/test_recovery.rec"

static wp_bool there(const char *path)
{
    wp_bool t = WP_FALSE;
    return (wp_bool)(plat_file_exists(path, &t) == WP_OK && t);
}

static void setup(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx, const char *text)
{
    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), doc) == WP_OK);
    wp_undo_journal_init(journal);
    wp_app_ctx_init(ctx, WP_KEYMAP_VI, doc, journal);
    ctx->doc_loaded = WP_TRUE;
    ctx->recovery_path = REC_PATH;
    if (there(REC_PATH)) (void)plat_file_delete(REC_PATH);
}

static void teardown(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    wp_keymap_free(ctx);
    wp_undo_journal_free(journal);
    wp_document_free(doc);
    if (there(REC_PATH)) (void)plat_file_delete(REC_PATH);
}

static void edit(wp_document *doc, wp_undo_journal *journal)
{
    ASSERT_TRUE(wp_edit_insert_text(doc, journal, wp_pos_doc_start(doc),
                                    (const u8 *)"Changed ", 8, NULL) == WP_OK);
}

static void text_of(const wp_document *doc, char *out, u32 cap)
{
    u32 len = 0;
    ASSERT_TRUE(format_txt_export(doc, (u8 *)out, cap - 1, &len) == WP_OK);
    out[len] = '\0';
}

static void write_raw(const char *bytes)
{
    plat_file *f;
    ASSERT_TRUE(plat_file_open(REC_PATH, PLAT_FILE_WRITE, &f) == WP_OK);
    ASSERT_TRUE(plat_file_write(f, (const u8 *)bytes, (u32)strlen(bytes)) == WP_OK);
    plat_file_close(f);
}

/* AP_TERM with nothing unsaved: leave at once, and leave nothing behind. */
void test_recovery_term_clean_leaves_without_a_copy(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "Hello\n");
    ASSERT_TRUE(wp_action_term_request(&ctx, NULL));
    ASSERT_TRUE(ctx.quit_requested);
    ASSERT_TRUE(!there(REC_PATH));
    teardown(&doc, &journal, &ctx);
}

/* AP_TERM with changes: they are written, unasked, and the program
   leaves; the document is NOT marked saved, because it was not. */
void test_recovery_term_modified_writes_the_copy_and_leaves(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char path[WP_APP_PATH_MAX];
    wp_bool rtf = WP_TRUE;

    setup(&doc, &journal, &ctx, "Hello\n");
    wp_action_remember_path(&ctx, "C:\\LETTERS\\MUM.TXT");
    ctx.doc_rtf = WP_FALSE;
    edit(&doc, &journal);
    ASSERT_TRUE(wp_action_term_request(&ctx, NULL));
    ASSERT_TRUE(ctx.quit_requested);
    ASSERT_TRUE(wp_document_is_modified(&doc));
    ASSERT_TRUE(ctx.recovery_owned);
    ASSERT_TRUE(wp_recovery_pending(&ctx, path, (u32)sizeof(path), &rtf));
    ASSERT_EQ_STR(path, "C:\\LETTERS\\MUM.TXT");
    ASSERT_TRUE(!rtf);
    teardown(&doc, &journal, &ctx);
}

/* No slot, or one that cannot be written: stay, so the shell refuses the
   shutdown rather than losing the work. */
void test_recovery_term_stays_when_the_copy_cannot_be_written(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "Hello\n");
    edit(&doc, &journal);
    ctx.recovery_path = NULL;
    ASSERT_TRUE(!wp_action_term_request(&ctx, NULL));
    ASSERT_TRUE(!ctx.quit_requested);

    ctx.recovery_path = "build/no-such-directory/x.rec";
    ASSERT_TRUE(!wp_action_term_request(&ctx, NULL));
    ASSERT_TRUE(!ctx.quit_requested);
    ASSERT_TRUE(!ctx.recovery_owned);
    ctx.recovery_path = REC_PATH;
    teardown(&doc, &journal, &ctx);
}

/* The next run: the offer's default recovers the changes, under the old
   name and format, still unsaved; the copy stays until they are. */
void test_recovery_offer_restores_the_changes(void)
{
    wp_document doc, doc2;
    wp_undo_journal journal, journal2;
    wp_app_ctx ctx, ctx2;
    char text[64];

    setup(&doc, &journal, &ctx, "Hello\n");
    wp_action_remember_path(&ctx, "C:\\LETTERS\\MUM.RTF");
    ctx.doc_rtf = WP_TRUE;
    edit(&doc, &journal);
    ASSERT_TRUE(wp_action_term_request(&ctx, NULL));
    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);

    /* a fresh run, with its start-up sample */
    ASSERT_TRUE(format_txt_import((const u8 *)"Sample\n", 7, &doc2) == WP_OK);
    wp_undo_journal_init(&journal2);
    wp_app_ctx_init(&ctx2, WP_KEYMAP_VI, &doc2, &journal2);
    ctx2.doc_loaded = WP_TRUE;
    ctx2.recovery_path = REC_PATH;
    ASSERT_TRUE(!ctx2.recovery_owned);
    ASSERT_EQ_U32((u32)wp_recovery_offer(&ctx2, NULL, NULL, 0), (u32)WP_RECOVERY_RECOVERED);
    text_of(ctx2.doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Changed Hello\n");
    ASSERT_EQ_STR(ctx2.doc_path, "C:\\LETTERS\\MUM.RTF");
    ASSERT_TRUE(ctx2.doc_rtf);
    ASSERT_TRUE(wp_document_is_modified(ctx2.doc));
    ASSERT_TRUE(ctx2.recovery_owned);
    ASSERT_TRUE(there(REC_PATH));
    teardown(ctx2.doc, &journal2, &ctx2);
}

/* An untitled document comes back untitled. */
void test_recovery_untitled_comes_back_untitled(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char path[WP_APP_PATH_MAX];

    setup(&doc, &journal, &ctx, "Hello\n");
    edit(&doc, &journal);
    ASSERT_TRUE(wp_recovery_write(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(wp_recovery_pending(&ctx, path, (u32)sizeof(path), NULL));
    ASSERT_EQ_STR(path, "");
    wp_action_remember_path(&ctx, "C:\\OTHER.TXT");
    ASSERT_TRUE(wp_recovery_restore(&ctx, NULL) == WP_OK);
    ASSERT_EQ_STR(ctx.doc_path, "");
    teardown(ctx.doc, &journal, &ctx);
}

/* A save that lands settles the document: its copy goes. */
void test_recovery_save_removes_the_documents_copy(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "Hello\n");
    edit(&doc, &journal);
    ASSERT_TRUE(wp_recovery_write(&ctx, NULL) == WP_OK);
    ASSERT_TRUE(there(REC_PATH));
    ASSERT_TRUE(wp_action_write_file(&ctx, "build/test_recovery_saved.txt",
                                     (const u8 *)"x", 1, WP_TRUE) == WP_OK);
    ASSERT_TRUE(!there(REC_PATH));
    ASSERT_TRUE(!ctx.recovery_owned);
    (void)plat_file_delete("build/test_recovery_saved.txt");
    teardown(&doc, &journal, &ctx);
}

/* A copy this run neither wrote nor recovered is not the document's: a
   save of some other work must not delete it. */
void test_recovery_save_keeps_a_copy_it_does_not_own(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "Hello\n");
    write_raw(WP_RECOVERY_MAGIC "path=A:\\OLD.RTF\nrtf=1\n\n{\\rtf1 Old}");
    ASSERT_TRUE(!ctx.recovery_owned);
    ASSERT_TRUE(wp_action_write_file(&ctx, "build/test_recovery_saved.txt",
                                     (const u8 *)"x", 1, WP_TRUE) == WP_OK);
    ASSERT_TRUE(there(REC_PATH));
    wp_recovery_settled(&ctx);
    ASSERT_TRUE(there(REC_PATH));
    (void)plat_file_delete("build/test_recovery_saved.txt");
    teardown(&doc, &journal, &ctx);
}

/* "Use saved": the copy goes, and the shell is given the name to open. */
void test_recovery_drop_and_the_saved_name(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char path[WP_APP_PATH_MAX];

    setup(&doc, &journal, &ctx, "Hello\n");
    write_raw(WP_RECOVERY_MAGIC "path=A:\\OLD.RTF\nrtf=1\nlater=ignored\n\n{\\rtf1 Old}");
    ASSERT_TRUE(wp_recovery_pending(&ctx, path, (u32)sizeof(path), NULL));
    ASSERT_EQ_STR(path, "A:\\OLD.RTF");
    wp_recovery_drop(&ctx);
    ASSERT_TRUE(!there(REC_PATH));
    ASSERT_EQ_U32((u32)wp_recovery_offer(&ctx, NULL, path, (u32)sizeof(path)), (u32)WP_RECOVERY_NONE);
    teardown(&doc, &journal, &ctx);
}

/* Not one of ours, or cut short: never offered, never loaded. */
void test_recovery_ignores_a_file_that_is_not_one(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "Hello\n");
    write_raw("{\\rtf1 just a document}");
    ASSERT_TRUE(!wp_recovery_pending(&ctx, NULL, 0, NULL));
    ASSERT_EQ_U32((u32)wp_recovery_offer(&ctx, NULL, NULL, 0), (u32)WP_RECOVERY_NONE);
    write_raw(WP_RECOVERY_MAGIC "path=A:\\CUT.RTF\nrtf=1");
    ASSERT_TRUE(!wp_recovery_pending(&ctx, NULL, 0, NULL));
    ASSERT_TRUE(wp_recovery_restore(&ctx, NULL) != WP_OK);
    teardown(&doc, &journal, &ctx);
}
