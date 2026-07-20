#include <string.h>
#include "test.h"
#include "app_ctx.h"

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

static wp_key_event key_char(u32 codepoint)
{
    wp_key_event key;
    key.codepoint = codepoint;
    key.special = (u16)WP_KEY_NONE;
    key.mods = 0;
    key._pad = 0;
    return key;
}

/* The concrete, mechanical guard the M10 plan calls for: this union's
   size must stay dominated by whichever single scheme is largest, not
   grow with the sum of every scheme ever added -- see keymap.h's own
   comment on wp_keymap_state. 2KB is generous headroom over
   wp_vimode_state's own two 128-byte cmdline/search buffers; a future
   scheme that embeds a large fixed array instead of mem_alloc'ing it
   (wp_keymap_state's own documented discipline) fails this test
   immediately instead of silently growing every wp_app_ctx instance. */
void test_keymap_app_ctx_size_stays_under_ceiling(void)
{
    ASSERT_TRUE(sizeof(wp_app_ctx) < 2048u);
}

void test_keymap_init_selects_vi_and_seeds_cursor_at_doc_start(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    build_doc_with_text(&doc, "Hello");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    ASSERT_EQ_U32((u32)ctx.keymap_id, (u32)WP_KEYMAP_VI);
    ASSERT_TRUE(wp_keymap_cursor(&ctx) != NULL);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->section, 0);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 0);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 0);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_keymap_feed_key_moves_cursor_through_vi_arm(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_keymap_outcome outcome;

    build_doc_with_text(&doc, "Hello");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    outcome = wp_keymap_feed_key(&ctx, key_char('l'), 0); /* vi Normal-mode: move right */
    ASSERT_EQ_U32(outcome.kind, (u32)WP_KEYMAP_RESULT_NONE);
    ASSERT_TRUE(outcome.inserted_char == WP_FALSE);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 1);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_keymap_feed_key_insert_reports_inserted_char(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_keymap_outcome outcome;

    build_doc_with_text(&doc, "Hello");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    wp_keymap_feed_key(&ctx, key_char('i'), 0); /* enter Insert mode */
    outcome = wp_keymap_feed_key(&ctx, key_char('X'), 0); /* types a char */
    ASSERT_TRUE(outcome.inserted_char == WP_TRUE);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_keymap_feed_key_quit_reports_quit_kind(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_keymap_outcome outcome;

    build_doc_with_text(&doc, "Hello");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    outcome = wp_keymap_feed_key(&ctx, key_char(':'), 0);
    ASSERT_EQ_U32(outcome.kind, (u32)WP_KEYMAP_RESULT_NONE);
    outcome = wp_keymap_feed_key(&ctx, key_char('q'), 0);
    ASSERT_EQ_U32(outcome.kind, (u32)WP_KEYMAP_RESULT_NONE);
    {
        wp_key_event enter;
        enter.codepoint = 0;
        enter.special = (u16)WP_KEY_ENTER;
        enter.mods = 0;
        enter._pad = 0;
        outcome = wp_keymap_feed_key(&ctx, enter, 0);
    }
    ASSERT_EQ_U32(outcome.kind, (u32)WP_KEYMAP_RESULT_QUIT);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_keymap_get_status_view_reports_vi_mode_label(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char buf[40];
    wp_vi_status_view view;

    build_doc_with_text(&doc, "Hello");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    wp_keymap_get_status_view(&ctx, buf, (u32)sizeof(buf), &view);
    ASSERT_TRUE(view.mode_label_utf8 != NULL);
    ASSERT_TRUE(view.mode_label_len > 0);
    ASSERT_TRUE(strstr((const char *)view.mode_label_utf8, "NORMAL") != NULL);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}
