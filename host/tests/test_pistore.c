/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The Pi's document store (M21 T21.6): File > Save to Pi, the copy every
   save makes, and File > Open from Pi, through the real file flow
   (actions.c) against a real odt_gateway.py --store. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "test.h"
#include "pistore.h"
#include "settings.h"
#include "app_ctx.h"
#include "actions.h"
#include "docmodel.h"
#include "format_txt.h"
#include "offload.h"
#include "platform.h"
#include "window_host.h"
#include "gateway_harness.h"

#define GATEWAY_BINARY "offload/gateway/odt_gateway.py"
#define PS_DIR    "build/pistore"
#define PS_STORE  PS_DIR "/store"
#define PS_CFG    PS_DIR "/RETROWP.CFG"
#define PS_DOC    PS_DIR "/NOTE.TXT"
#define PS_KEPT   PS_STORE "/ST/NOTE.TXT/"

static wp_document     g_doc;
static wp_undo_journal g_journal;
static wp_app_ctx      g_ctx;
static gateway_handle  g_gw;

static wp_bool there(const char *path)
{
    wp_bool t = WP_FALSE;
    return (wp_bool)(plat_file_exists(path, &t) == WP_OK && t);
}

static u32 read_all(const char *path, char *out, u32 cap)
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

static void write_all(const char *path, const char *text)
{
    FILE *fp = fopen(path, "wb");
    if (fp != NULL) {
        fputs(text, fp);
        fclose(fp);
    }
}

/* The document's text, as plain text. */
static const char *doc_text(void)
{
    static char buf[256];
    u32 n = 0;

    if (format_txt_export(g_ctx.doc, (u8 *)buf, sizeof(buf) - 1, &n) != WP_OK) n = 0;
    buf[n] = '\0';
    return buf;
}

/* A fresh store; the gateway started with `store` (or without one). */
static void setup(wp_bool store)
{
    static const char *const with[] = { "--store", PS_STORE, "--store-versions", "3", NULL };

    (void)mkdir(PS_DIR, 0777);
    (void)system("rm -rf " PS_STORE " " PS_DOC);
    ASSERT_TRUE(format_txt_import((const u8 *)"Hello\n", 6, &g_doc) == WP_OK);
    wp_undo_journal_init(&g_journal);
    wp_app_ctx_init(&g_ctx, WP_KEYMAP_VI, &g_doc, &g_journal);
    g_ctx.doc_loaded = WP_TRUE;
    g_ctx.config_path = PS_CFG;
    g_ctx.path_sep = '/';
    g_ctx.machine_default = "ST";
    ASSERT_TRUE(gateway_start_bin(GATEWAY_BINARY, store ? with : NULL, &g_gw) == WP_OK);
    ASSERT_TRUE(offload_init(g_gw.endpoint) == WP_OK);
}

static void teardown(void)
{
    offload_shutdown();
    gateway_stop(&g_gw);
    wp_keymap_free(&g_ctx);
    wp_undo_journal_free(&g_journal);
}

/* Save to Pi: saved here, then the same bytes kept on the Pi under this
   machine's name, and the Pi's answer told. */
void test_pistore_save_to_pi_keeps_the_file(void)
{
    char here[64], kept[64];

    setup(WP_TRUE);
    g_ctx.to_pi = WP_TRUE;
    ASSERT_TRUE(wp_action_file_save(&g_ctx, WP_FALSE, NULL, NULL) == WP_OK);  /* unnamed, no picker */
    ASSERT_TRUE(!g_ctx.to_pi);                 /* a cancelled save does not leave it set */
    ASSERT_TRUE(!there(PS_KEPT "000001"));

    g_ctx.to_pi = WP_TRUE;
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_EQ_STR("Kept NOTE.TXT on the Pi (1 version)", window_host_last_title());
    ASSERT_TRUE(read_all(PS_DOC, here, sizeof(here)) > 0);
    ASSERT_TRUE(read_all(PS_KEPT "000001", kept, sizeof(kept)) > 0);
    ASSERT_EQ_STR(here, kept);
    ASSERT_TRUE(!g_ctx.to_pi);

    /* A plain Save afterwards does not send it again. */
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_TRUE(!there(PS_KEPT "000002"));
    teardown();
}

/* Settings' copy on every save: each save is kept, and nothing is said
   when it works. */
void test_pistore_copy_on_every_save_is_quiet(void)
{
    setup(WP_TRUE);
    g_ctx.settings.pi_copy = 1;
    wp_action_tell(&g_ctx, "before");
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_EQ_STR("before", window_host_last_title());
    ASSERT_TRUE(there(PS_KEPT "000001"));
    ASSERT_TRUE(there(PS_KEPT "000002"));
    teardown();
}

/* The Pi keeps the last three: the fourth and fifth saves retire the
   first two. */
void test_pistore_keeps_the_last_versions(void)
{
    u32 i;

    setup(WP_TRUE);
    for (i = 0; i < 5; i++) {
        ASSERT_TRUE(wp_pistore_put(&g_ctx, PS_DOC, (const u8 *)"x\n", 2, WP_TRUE) == WP_OK);
    }
    ASSERT_TRUE(!there(PS_KEPT "000002"));
    ASSERT_TRUE(there(PS_KEPT "000003"));
    ASSERT_TRUE(there(PS_KEPT "000005"));
    teardown();
}

/* Open from Pi: the document, then which copy (newest first), written
   beside the configuration file and opened. */
void test_pistore_open_from_pi_restores_a_version(void)
{
    char here[64];

    setup(WP_TRUE);
    ASSERT_TRUE(wp_pistore_put(&g_ctx, PS_DOC, (const u8 *)"one\n", 4, WP_TRUE) == WP_OK);
    ASSERT_TRUE(wp_pistore_put(&g_ctx, PS_DOC, (const u8 *)"two\n", 4, WP_TRUE) == WP_OK);
    ASSERT_TRUE(wp_pistore_put(&g_ctx, PS_DOC, (const u8 *)"three\n", 6, WP_TRUE) == WP_OK);
    ASSERT_TRUE(!there(PS_DOC));

    g_ctx.from_pi = WP_TRUE;
    window_host_next_choice(0);                /* NOTE.TXT */
    window_host_next_choice(1);                /* the second newest */
    ASSERT_TRUE(wp_action_file_open(&g_ctx, NULL, NULL) == WP_OK);
    ASSERT_TRUE(!g_ctx.from_pi);
    ASSERT_EQ_STR("Which copy?", window_host_last_title());
    ASSERT_EQ_STR("two\n", doc_text());
    ASSERT_EQ_STR(PS_DOC, g_ctx.doc_path);
    ASSERT_TRUE(read_all(PS_DOC, here, sizeof(here)) > 0);
    ASSERT_EQ_STR("two\n", here);
    teardown();
}

/* A file of that name here is replaced only when the writer says so. */
void test_pistore_open_from_pi_asks_before_replacing(void)
{
    char here[64];

    setup(WP_TRUE);
    ASSERT_TRUE(wp_pistore_put(&g_ctx, PS_DOC, (const u8 *)"from the Pi\n", 12, WP_TRUE) == WP_OK);
    write_all(PS_DOC, "local\n");

    g_ctx.from_pi = WP_TRUE;
    window_host_next_choice(0);                /* NOTE.TXT (one version: no second question) */
    window_host_next_choice(1);                /* Cancel */
    ASSERT_TRUE(wp_action_file_open(&g_ctx, NULL, NULL) == WP_OK);
    ASSERT_EQ_STR("Replace NOTE.TXT here?", window_host_last_title());
    ASSERT_EQ_STR("Hello\n", doc_text());
    (void)read_all(PS_DOC, here, sizeof(here));
    ASSERT_EQ_STR("local\n", here);

    g_ctx.from_pi = WP_TRUE;
    window_host_next_choice(0);
    window_host_next_choice(0);                /* Replace */
    ASSERT_TRUE(wp_action_file_open(&g_ctx, NULL, NULL) == WP_OK);
    ASSERT_EQ_STR("from the Pi\n", doc_text());
    (void)read_all(PS_DOC, here, sizeof(here));
    ASSERT_EQ_STR("from the Pi\n", here);
    teardown();
}

/* Each machine sees its own: another name on the same store sees none. */
void test_pistore_machines_are_kept_apart(void)
{
    setup(WP_TRUE);
    ASSERT_TRUE(wp_pistore_put(&g_ctx, PS_DOC, (const u8 *)"st\n", 3, WP_TRUE) == WP_OK);
    strcpy(g_ctx.settings.machine, "AMIGA");
    g_ctx.from_pi = WP_TRUE;
    ASSERT_TRUE(wp_action_file_open(&g_ctx, NULL, NULL) == WP_OK);
    ASSERT_EQ_STR("The Pi keeps no documents for this machine", window_host_last_title());
    ASSERT_EQ_STR("Hello\n", doc_text());

    /* A name the store refuses is said so, and the save stands. */
    strcpy(g_ctx.settings.machine, "../ST");
    g_ctx.to_pi = WP_TRUE;
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_EQ_STR("The Pi refused that name", window_host_last_title());
    ASSERT_TRUE(there(PS_DOC));
    teardown();
}

/* A gateway that keeps no documents: Save to Pi says so; the copy every
   save makes says so once, and the saves themselves stand. */
void test_pistore_without_a_store(void)
{
    setup(WP_FALSE);
    ASSERT_TRUE(!wp_pistore_able(&g_ctx));
    g_ctx.to_pi = WP_TRUE;
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_EQ_STR("Not copied: no gateway that keeps documents", window_host_last_title());
    ASSERT_TRUE(there(PS_DOC));

    g_ctx.settings.pi_copy = 1;
    wp_action_tell(&g_ctx, "between");
    ASSERT_TRUE(wp_action_save_file(&g_ctx, PS_DOC, NULL) == WP_OK);
    ASSERT_EQ_STR("between", window_host_last_title());  /* told already, above */

    g_ctx.from_pi = WP_TRUE;
    ASSERT_TRUE(wp_action_file_open(&g_ctx, NULL, NULL) == WP_OK);
    ASSERT_EQ_STR("Open from Pi needs a gateway that keeps documents", window_host_last_title());
    teardown();
}
