/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "tools.h"
#include "app_ctx.h"
#include "keymap.h"
#include "format_txt.h"
#include "editing.h"

/* The presenter under test: keeps the last title and message shown. */
static char g_title[64];
static char g_msg[300];
static u32  g_shown;

static void capture(wp_app_ctx *ctx, const char *title, const char *msg)
{
    (void)ctx;
    strncpy(g_title, title, sizeof(g_title) - 1); g_title[sizeof(g_title) - 1] = '\0';
    strncpy(g_msg, msg, sizeof(g_msg) - 1);       g_msg[sizeof(g_msg) - 1] = '\0';
    g_shown++;
}

static void setup(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx, const char *text)
{
    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), doc) == WP_OK);
    wp_undo_journal_init(journal);
    wp_app_ctx_init(ctx, WP_KEYMAP_VI, doc, journal);
    ctx->doc_loaded = WP_TRUE;
    g_title[0] = '\0';
    g_msg[0] = '\0';
    g_shown = 0;
}

static void teardown(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    wp_keymap_free(ctx);
    wp_undo_journal_free(journal);
    wp_document_free(doc);
    wp_tools_shutdown();
}

/* No gateway on the host: the local grammar-lite check speaks instead. */
void test_tools_grammar_check_reports_locally_without_a_gateway(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "The the cat sat.\n");
    ASSERT_TRUE(wp_tools_grammar_check(&ctx, capture) == WP_OK);
    ASSERT_EQ_U32(g_shown, 1);
    ASSERT_EQ_STR(g_title, "Grammar Check");
    ASSERT_TRUE(strstr(g_msg, "local check found 1 issue") != NULL);
    ASSERT_TRUE(strstr(g_msg, "First: ") != NULL);
    teardown(&doc, &journal, &ctx);

    setup(&doc, &journal, &ctx, "A clean sentence.\n");
    ASSERT_TRUE(wp_tools_grammar_check(&ctx, capture) == WP_OK);
    ASSERT_EQ_STR(g_msg, "No grammar issues found.");
    teardown(&doc, &journal, &ctx);
}

void test_tools_grammar_and_chat_want_a_paragraph_with_text(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "\n");
    ASSERT_TRUE(wp_tools_grammar_check(&ctx, capture) == WP_OK);
    ASSERT_TRUE(strstr(g_msg, "Place the cursor in a paragraph") != NULL);
    ASSERT_TRUE(wp_tools_chat(&ctx, capture) == WP_OK);
    ASSERT_EQ_STR(g_title, "Chat");
    ASSERT_TRUE(strstr(g_msg, "Place the cursor in a paragraph") != NULL);
    teardown(&doc, &journal, &ctx);
}

void test_tools_gateway_tools_say_when_there_is_no_device(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "hello world\n");
    wp_keymap_cursor(&ctx)->offset = 5;   /* right after "hello" */
    ASSERT_TRUE(wp_tools_thesaurus(&ctx, capture) == WP_ERR);
    ASSERT_EQ_STR(g_title, "Thesaurus");
    ASSERT_EQ_STR(g_msg, "No offload device connected.");
    ASSERT_TRUE(wp_tools_chat(&ctx, capture) == WP_ERR);
    ASSERT_EQ_STR(g_msg, "No offload device connected.");

    /* At the start there is no word behind the caret to look up. */
    wp_keymap_cursor(&ctx)->offset = 0;
    ASSERT_TRUE(wp_tools_thesaurus(&ctx, capture) == WP_OK);
    ASSERT_TRUE(strstr(g_msg, "Place the cursor right after a word") != NULL);
    teardown(&doc, &journal, &ctx);
}

void test_tools_spell_check_names_the_first_unknown_word(void)
{
    static const char dict[] = "host/tests/fixtures/spell_test.dict";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    setup(&doc, &journal, &ctx, "hello world\nthe quick dgo ran\n");
    ASSERT_TRUE(wp_tools_spell_check(&ctx, dict, capture) == WP_OK);
    ASSERT_EQ_STR(g_title, "Spell Check");
    /* "the" is not in the small dictionary either, so it comes first. */
    ASSERT_TRUE(strncmp(g_msg, "Unknown word: the", 17) == 0);
    teardown(&doc, &journal, &ctx);

    setup(&doc, &journal, &ctx, "hello world\n");
    ASSERT_TRUE(wp_tools_spell_check(&ctx, dict, capture) == WP_OK);
    ASSERT_EQ_STR(g_msg, "No misspellings found.");
    teardown(&doc, &journal, &ctx);

    setup(&doc, &journal, &ctx, "hello dgo\n");
    ASSERT_TRUE(wp_tools_spell_check(&ctx, dict, capture) == WP_OK);
    ASSERT_TRUE(strncmp(g_msg, "Unknown word: dgo", 17) == 0);
    ASSERT_TRUE(strstr(g_msg, "Suggestions: ") != NULL);
    ASSERT_TRUE(strstr(g_msg, "dog") != NULL);
    teardown(&doc, &journal, &ctx);

    setup(&doc, &journal, &ctx, "hello\n");
    ASSERT_TRUE(wp_tools_spell_check(&ctx, "host/tests/fixtures/no-such.dict", capture) == WP_ERR);
    ASSERT_TRUE(strstr(g_msg, "dictionary not found") != NULL);
    teardown(&doc, &journal, &ctx);
}
