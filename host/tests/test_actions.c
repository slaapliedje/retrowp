#include <string.h>
#include "test.h"
#include "actions.h"

static void build_doc_with_text(wp_document *doc, const char *text)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    para = wp_section_paragraph_get(sec, para_idx);
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
}

void test_actions_find_by_id(void)
{
    const wp_action *a = wp_action_find(WP_ACT_FORMAT_BOLD);

    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(a->id == WP_ACT_FORMAT_BOLD);
    ASSERT_EQ_STR(a->label_utf8, "Bold");
    ASSERT_EQ_U32(a->shortcut_ascii, 0x02);

    ASSERT_TRUE(wp_action_find((wp_action_id)999) == NULL);
}

void test_actions_find_by_shortcut(void)
{
    const wp_action *a = wp_action_find_by_shortcut(0x02); /* Ctrl+B */

    ASSERT_TRUE(a != NULL);
    ASSERT_TRUE(a->id == WP_ACT_FORMAT_BOLD);

    ASSERT_TRUE(wp_action_find_by_shortcut(0) == NULL);   /* 0 = "no shortcut", never matches */
    ASSERT_TRUE(wp_action_find_by_shortcut(0x7F) == NULL); /* not bound to anything */
}

void test_actions_every_shortcut_is_unique(void)
{
    u32 i, j;

    for (i = 0; i < WP_ACT_COUNT; i++) {
        if (g_wp_actions[i].shortcut_ascii == 0) continue;
        for (j = i + 1; j < WP_ACT_COUNT; j++) {
            if (g_wp_actions[j].shortcut_ascii == 0) continue;
            ASSERT_TRUE(g_wp_actions[i].shortcut_ascii != g_wp_actions[j].shortcut_ascii);
        }
    }
}

void test_actions_format_bold_toggles_style_flags(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *a = wp_action_find(WP_ACT_FORMAT_BOLD);

    build_doc_with_text(&doc, "Hello");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, &doc, &journal);

    ASSERT_EQ_U32(ctx.style_flags & WP_STYLE_BOLD, 0);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_TRUE((ctx.style_flags & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(ctx.style_flags & WP_STYLE_BOLD, 0);

    wp_vimode_free(&ctx.vi);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_actions_file_quit_sets_flag(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *a = wp_action_find(WP_ACT_FILE_QUIT);

    build_doc_with_text(&doc, "");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, &doc, &journal);

    ASSERT_TRUE(!ctx.quit_requested);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_TRUE(ctx.quit_requested);

    wp_vimode_free(&ctx.vi);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_actions_edit_copy_sets_clipboard(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *a = wp_action_find(WP_ACT_EDIT_COPY);
    u8 buf[64];
    u32 len;

    build_doc_with_text(&doc, "Hello World");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, &doc, &journal);
    ctx.doc_loaded = WP_TRUE;

    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_TRUE(plat_clipboard_has_text());
    ASSERT_TRUE(plat_clipboard_get_text(buf, sizeof(buf), &len) == WP_OK);
    buf[len] = '\0';
    ASSERT_EQ_STR((const char *)buf, "Hello World\n"); /* format_txt_export terminates every paragraph with '\n' */

    wp_vimode_free(&ctx.vi);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_actions_format_size_uses_dialog_choice(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *a = wp_action_find(WP_ACT_FORMAT_SIZE);

    build_doc_with_text(&doc, "");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, &doc, &journal);
    ctx.current_size_twips = 240; /* 12pt -- matches the host stub's "always pick the default" behavior */

    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(ctx.current_size_twips, 240); /* host's plat_dialog_choice stub always keeps the default */

    wp_vimode_free(&ctx.vi);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}
