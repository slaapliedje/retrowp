/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "actions.h"
#include "rulerbar.h"
#include "format_txt.h"
#include "offload.h"
#include "gateway_harness.h"
#include "draw_host.h"

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

/* Cut and Copy act on the selection now: these put vi into Visual mode
   over the whole document, which is what the whole-document tests below
   mean by "cut". */
static void select_all_vi(wp_app_ctx *ctx)
{
    ctx->km.vi.mode = (u8)WP_VIMODE_VISUAL;
    ctx->km.vi.visual.anchor = wp_pos_doc_start(ctx->doc);
    ctx->km.vi.visual.caret = wp_pos_doc_end(ctx->doc);
    ctx->km.vi.cursor = ctx->km.vi.visual.caret;
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

/* wp_action_find() returns &g_wp_actions[id] on a bare bounds check --
   it trusts the table's row order to match the wp_action_id enum exactly,
   and never verifies the .id field it just indexed past. Every backend
   depends on that: Amiga bakes act->id into its MUI ReturnID and indexes
   back positionally on dispatch, Mac counts ordinal position within a
   .group, Atari switches on raw menu object indices. If a row were ever
   inserted or reordered without moving the enum in lockstep, all of them
   would silently fire the WRONG action -- no crash, no warning, just a
   menu item that does someone else's job. Nothing checked this until now. */
void test_actions_table_row_order_matches_enum(void)
{
    u32 i;

    for (i = 0; i < WP_ACT_COUNT; i++) {
        ASSERT_TRUE(g_wp_actions[i].id == (wp_action_id)i);
        ASSERT_TRUE(wp_action_find((wp_action_id)i) == &g_wp_actions[i]);
    }
}

/* Every action must be reachable by a human: it sits in a real native
   menu group, or has a keyboard shortcut, or is a ruler-bar button. An entry
   with neither is dead weight that no backend can ever invoke. */
void test_actions_every_action_is_reachable(void)
{
    u32 i;

    for (i = 0; i < WP_ACT_COUNT; i++) {
        ASSERT_TRUE(g_wp_actions[i].group != WP_MENU_NONE ||
                    g_wp_actions[i].shortcut_ascii != 0 ||
                    wp_rulerbar_has_action((wp_action_id)i));
    }
}

/* Every action must also carry a non-empty label: the Amiga and Mac
   backends both build their menus by folding this table and passing
   .label_utf8 straight to MenuitemObject/InsertMenuItem, neither of
   which tolerates a NULL. */
void test_actions_every_action_has_a_label(void)
{
    u32 i;

    for (i = 0; i < WP_ACT_COUNT; i++) {
        ASSERT_TRUE(g_wp_actions[i].label_utf8 != NULL);
        ASSERT_TRUE(g_wp_actions[i].label_utf8[0] != '\0');
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
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    ASSERT_EQ_U32(ctx.style_flags & WP_STYLE_BOLD, 0);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_TRUE((ctx.style_flags & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(ctx.style_flags & WP_STYLE_BOLD, 0);

    wp_keymap_free(&ctx);
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
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);

    ASSERT_TRUE(!ctx.quit_requested);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_TRUE(ctx.quit_requested);

    wp_keymap_free(&ctx);
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
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    ctx.doc_loaded = WP_TRUE;

    select_all_vi(&ctx);
    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_TRUE(plat_clipboard_has_text());
    ASSERT_TRUE(plat_clipboard_get_text(buf, sizeof(buf), &len) == WP_OK);
    buf[len] = '\0';
    ASSERT_EQ_STR((const char *)buf, "Hello World"); /* the selection, nothing after it */

    wp_keymap_free(&ctx);
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
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    ctx.current_size_twips = 240; /* 12pt -- matches the host stub's "always pick the default" behavior */

    ASSERT_TRUE(a->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(ctx.current_size_twips, 240); /* host's plat_dialog_choice stub always keeps the default */

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Bold with something selected -- the behaviour that did not exist
   before wp_edit_style_range: every scheme's Bold used to change only
   the style of text typed NEXT, so selecting a word and pressing Ctrl+B
   did nothing at all to it. */

static u16 flags_of_first_run(wp_document *doc)
{
    wp_section *sec = wp_document_section_get(doc, 0);
    wp_paragraph *para = wp_section_paragraph_get(sec, 0);
    const wp_char_style *cs = wp_document_cstyle_get(doc, para->runs[0].style);

    return cs ? cs->flags : 0;
}

static void select_vi_visual(wp_app_ctx *ctx, u32 from, u32 to)
{
    ctx->km.vi.mode = (u8)WP_VIMODE_VISUAL;
    ctx->km.vi.visual.anchor.section = 0;
    ctx->km.vi.visual.anchor.paragraph = 0;
    ctx->km.vi.visual.anchor.run = 0;
    ctx->km.vi.visual.anchor.offset = from;
    ctx->km.vi.visual.caret = ctx->km.vi.visual.anchor;
    ctx->km.vi.visual.caret.offset = to;
}

void test_actions_bold_styles_the_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *bold = wp_action_find(WP_ACT_FORMAT_BOLD);

    build_doc_with_text(&doc, "Hello World");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    select_vi_visual(&ctx, 0, 5);

    ASSERT_TRUE(bold->handler(&ctx) == WP_OK);

    /* The selected text itself is bold now ... */
    ASSERT_TRUE((flags_of_first_run(&doc) & WP_STYLE_BOLD) != 0);
    /* ... and it is one undoable step. */
    ASSERT_TRUE(wp_undo_can_undo(&journal));
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE((flags_of_first_run(&doc) & WP_STYLE_BOLD) == 0);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_actions_bold_toggles_input_style_with_no_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *bold = wp_action_find(WP_ACT_FORMAT_BOLD);

    build_doc_with_text(&doc, "Hello World");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    ctx.style_flags = 0;

    /* Nothing selected: the old behaviour, unchanged -- what gets typed
       next is bold, and the document is not touched. */
    ASSERT_TRUE(bold->handler(&ctx) == WP_OK);
    ASSERT_TRUE((ctx.style_flags & WP_STYLE_BOLD) != 0);
    ASSERT_TRUE((flags_of_first_run(&doc) & WP_STYLE_BOLD) == 0);
    ASSERT_TRUE(!wp_undo_can_undo(&journal));

    ASSERT_TRUE(bold->handler(&ctx) == WP_OK);
    ASSERT_TRUE((ctx.style_flags & WP_STYLE_BOLD) == 0);   /* a toggle, still */

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

void test_actions_bold_on_an_all_bold_selection_clears_it(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_action *bold = wp_action_find(WP_ACT_FORMAT_BOLD);

    build_doc_with_text(&doc, "Hello World");
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    select_vi_visual(&ctx, 0, 11);

    ASSERT_TRUE(bold->handler(&ctx) == WP_OK);
    ASSERT_TRUE((flags_of_first_run(&doc) & WP_STYLE_BOLD) != 0);

    /* Already bold end to end, so the second press turns it off. */
    ASSERT_TRUE(bold->handler(&ctx) == WP_OK);
    ASSERT_TRUE((flags_of_first_run(&doc) & WP_STYLE_BOLD) == 0);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* -- Data-loss guards: Cut, Paste, quitting, saving ------------------- */

static void doc_text(const wp_document *doc, char *out, u32 cap)
{
    u32 len = 0;

    ASSERT_TRUE(format_txt_export(doc, (u8 *)out, cap - 1, &len) == WP_OK);
    out[len] = '\0';
}

static void ctx_setup(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx,
                      const char *text)
{
    build_doc_with_text(doc, text);
    wp_undo_journal_init(journal);
    wp_app_ctx_init(ctx, WP_KEYMAP_VI, doc, journal);
    ctx->doc_loaded = WP_TRUE;
}

static void ctx_teardown(wp_document *doc, wp_undo_journal *journal, wp_app_ctx *ctx)
{
    wp_keymap_free(ctx);
    wp_undo_journal_free(journal);
    wp_document_free(doc);
}

void test_actions_cut_then_paste_gives_the_document_back(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Hello World");

    select_all_vi(&ctx);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "\n");
    /* A Cut is unsaved work, not a fresh load: quitting must still warn. */
    ASSERT_TRUE(wp_document_is_modified(&doc));

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_PASTE)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello World\n");   /* no extra empty paragraph */

    ctx_teardown(&doc, &journal, &ctx);
}

/* Like ctx_setup, but the text goes through the plain-text importer,
   so a '\n' makes a paragraph (ctx_setup keeps the whole string in one
   run). That is the shape a document has after File > Open. */
static void ctx_setup_imported(wp_document *doc, wp_undo_journal *journal,
                               wp_app_ctx *ctx, const char *text)
{
    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), doc) == WP_OK);
    wp_undo_journal_init(journal);
    wp_app_ctx_init(ctx, WP_KEYMAP_VI, doc, journal);
    ctx->doc_loaded = WP_TRUE;
}

/* Found on the Amiga, reading the saved file back: Cut of the sample
   document left its third and fifth paragraphs standing. Every blank
   line had come out of the importer as a paragraph holding one run of
   no text, and wp_edit_delete_selection stopped at the first of them. */
void test_actions_cut_takes_every_paragraph_of_an_opened_file(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[128];

    ctx_setup_imported(&doc, &journal, &ctx, "One two\n\nThree four\n\nFive six\n\nSeven\n");

    select_all_vi(&ctx);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "\n");

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_PASTE)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "One two\n\nThree four\n\nFive six\n\nSeven\n");

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "\n");
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "One two\n\nThree four\n\nFive six\n\nSeven\n");

    ctx_teardown(&doc, &journal, &ctx);
}

/* The importer no longer makes such a run, but other paths can leave a
   run with no text behind, so the delete must cope with one on its own. */
void test_actions_cut_copes_with_a_run_of_no_text(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_section *sec;
    wp_span span;
    u16 run_idx;
    char text[64];

    ctx_setup_imported(&doc, &journal, &ctx, "One\n\nTwo\n");
    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(sec->paras[1].run_count == 0);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"", 0, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(&sec->paras[1], span, doc.default_cstyle, &run_idx) == WP_OK);

    select_all_vi(&ctx);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "\n");

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "One\n\nTwo\n");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_cut_is_one_undo_step(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    select_all_vi(&ctx);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello World\n");

    ctx_teardown(&doc, &journal, &ctx);
}

/* The host clipboard refuses more than 4096 bytes. Cut used to empty the
   document anyway, and that is how a long document was lost on DOS. */
void test_actions_cut_keeps_a_document_the_clipboard_cannot_hold(void)
{
    static char big[5001];
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    ctx_setup(&doc, &journal, &ctx, big);

    select_all_vi(&ctx);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) != WP_OK);
    ASSERT_EQ_U32(wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0)->runs[0].text.length,
                  5000);
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_paste_inserts_at_the_cursor(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "AB");
    wp_keymap_cursor(&ctx)->offset = 1;
    ASSERT_TRUE(plat_clipboard_set_text((const u8 *)"x\r\ny", 4) == WP_OK);

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_PASTE)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Ax\nyB\n");   /* CRLF became one paragraph break */
    ASSERT_TRUE(wp_document_is_modified(&doc));

    /* One undo step takes the whole paste back out. */
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "AB\n");

    ctx_teardown(&doc, &journal, &ctx);
}

/* The host's plat_dialog_choice always takes the default, and Cancel is
   the default of the unsaved-changes question -- so on the host a
   modified document is a writer who said "keep it". */
void test_actions_request_quit_asks_when_modified(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    ctx_setup(&doc, &journal, &ctx, "Hello");
    wp_action_request_quit(&ctx);
    ASSERT_TRUE(ctx.quit_requested);   /* clean: leaves without asking */

    ctx.quit_requested = WP_FALSE;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, wp_pos_doc_start(&doc),
                                    (const u8 *)"x", 1, NULL) == WP_OK);
    wp_action_request_quit(&ctx);
    ASSERT_TRUE(!ctx.quit_requested);  /* modified: the answer was Cancel */

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_save_quit_stays_when_the_save_did_not_land(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    ctx_setup(&doc, &journal, &ctx, "Hello");
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, wp_pos_doc_start(&doc),
                                    (const u8 *)"x", 1, NULL) == WP_OK);
    wp_action_request_quit_after_save(&ctx);
    ASSERT_TRUE(!ctx.quit_requested);

    wp_document_mark_saved(&doc);       /* what a save that landed does */
    wp_action_request_quit_after_save(&ctx);
    ASSERT_TRUE(ctx.quit_requested);

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_replace_document_resets_the_journal(void)
{
    wp_document doc, fresh;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Old");
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, wp_pos_doc_start(&doc),
                                    (const u8 *)"x", 1, NULL) == WP_OK);
    ASSERT_TRUE(wp_undo_can_undo(&journal));

    build_doc_with_text(&fresh, "New");
    wp_action_replace_document(&ctx, &fresh);

    /* An Undo here used to replay "insert x" against the freed document. */
    ASSERT_TRUE(!wp_undo_can_undo(&journal));
    ASSERT_TRUE(!wp_document_is_modified(&doc));
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "New\n");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_write_file_marks_saved_only_when_it_lands(void)
{
    static const char good[] = "build/test_actions_write_file.txt";
    static const char bad[] = "build/no-such-directory/x.txt";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_bool exists = WP_FALSE;

    ctx_setup(&doc, &journal, &ctx, "Hello");
    wp_document_mark_modified(&doc);

    ASSERT_TRUE(wp_action_write_file(&ctx, bad, (const u8 *)"Hello\n", 6, WP_TRUE) != WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc));

    ASSERT_TRUE(wp_action_write_file(&ctx, good, (const u8 *)"Hello\n", 6, WP_FALSE) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc));   /* an export, not a save */

    ASSERT_TRUE(wp_action_write_file(&ctx, good, (const u8 *)"Hello\n", 6, WP_TRUE) == WP_OK);
    ASSERT_TRUE(!wp_document_is_modified(&doc));
    ASSERT_TRUE(plat_file_exists(good, &exists) == WP_OK && exists);
    plat_file_delete(good);

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_align_marks_the_document_modified(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    ctx_setup(&doc, &journal, &ctx, "Hello");
    /* format_txt_import gives every paragraph the default style; the
       bare fixture leaves handle 0, which Align skips as unstyled. */
    wp_paragraph_pstyle_set(wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0),
                            doc.default_pstyle);

    /* The host dialog keeps the current choice: the same alignment
       again is not a change. */
    ASSERT_TRUE(wp_action_find(WP_ACT_FORMAT_ALIGN)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    /* The dialog defaults to ctx.align, so this picks Center. */
    ctx.align = WP_ALIGN_CENTER;
    ASSERT_TRUE(wp_action_find(WP_ACT_FORMAT_ALIGN)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(wp_document_is_modified(&doc));

    ctx_teardown(&doc, &journal, &ctx);
}

/* Cut deletes through wp_edit_delete_selection, which was written for
   text. A document with a picture in it must still come out of Cut
   empty, and one Undo must bring every paragraph back -- the picture
   included, since the clipboard only ever holds the text. */
void test_actions_cut_with_an_image_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_image_desc desc;
    wp_pos pos;
    char text[64];
    u32 before;

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    memset(&desc, 0, sizeof(desc));
    desc.width_px = 8;
    desc.height_px = 8;
    desc.display_width_twips = 120;
    desc.display_height_twips = 120;
    desc.palette_count = 2;
    desc.palette[1][0] = desc.palette[1][1] = desc.palette[1][2] = 255;
    pos.section = 0; pos.paragraph = 0; pos.run = 0; pos.offset = 5;
    ASSERT_TRUE(wp_edit_insert_image(&doc, &journal, pos, &desc) == WP_OK);
    before = wp_document_section_get(&doc, 0)->para_count;
    ASSERT_EQ_U32(before, 3);   /* "Hello", the picture, " World" */

    select_all_vi(&ctx);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(wp_document_section_get(&doc, 0)->para_count, 1);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "\n");

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(wp_document_section_get(&doc, 0)->para_count, before);
    ASSERT_TRUE(wp_section_paragraph_get(wp_document_section_get(&doc, 0), 1)->kind ==
                (u8)WP_PARA_IMAGE);

    ctx_teardown(&doc, &journal, &ctx);
}

/* ---- The clipboard works on the selection ------------------------ */

static void clipboard_text(char *out, u32 cap)
{
    u32 len = 0;

    ASSERT_TRUE(plat_clipboard_get_text((u8 *)out, cap - 1, &len) == WP_OK);
    out[len] = '\0';
}

void test_actions_copy_takes_only_the_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    select_vi_visual(&ctx, 0, 5);

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_COPY)->handler(&ctx) == WP_OK);
    clipboard_text(text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello");
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello World\n");
    ASSERT_TRUE(!wp_document_is_modified(&doc));
    /* Copy leaves the selection standing. */
    ASSERT_EQ_U32(ctx.km.vi.mode, (u32)WP_VIMODE_VISUAL);

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_cut_takes_only_the_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    select_vi_visual(&ctx, 6, 11);

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    clipboard_text(text, sizeof(text));
    ASSERT_EQ_STR(text, "World");
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello \n");
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 6);
    /* Cut consumed the selection: back in Normal mode, as after `d`. */
    ASSERT_EQ_U32(ctx.km.vi.mode, (u32)WP_VIMODE_NORMAL);

    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello World\n");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_paste_replaces_the_selection(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    ASSERT_TRUE(plat_clipboard_set_text((const u8 *)"Big", 3) == WP_OK);
    select_vi_visual(&ctx, 0, 5);

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_PASTE)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Big World\n");
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 3);
    ASSERT_EQ_U32(ctx.km.vi.mode, (u32)WP_VIMODE_NORMAL);

    /* The removal and the insertion are one undo step. */
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello World\n");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_copy_and_cut_with_nothing_selected_do_nothing(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Hello");
    ASSERT_TRUE(plat_clipboard_set_text((const u8 *)"keep", 4) == WP_OK);

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_COPY)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    clipboard_text(text, sizeof(text));
    ASSERT_EQ_STR(text, "keep");
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Hello\n");
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    ctx_teardown(&doc, &journal, &ctx);
}

/* Visual Line selects paragraphs with their breaks, so what is cut can
   be pasted back as the same lines: the '\n' rides along. */
void test_actions_cut_in_visual_line_takes_whole_paragraphs(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup_imported(&doc, &journal, &ctx, "One\nTwo\nThree\n");
    ctx.km.vi.mode = (u8)WP_VIMODE_VISUAL_LINE;
    ctx.km.vi.visual.anchor.section = 0;
    ctx.km.vi.visual.anchor.paragraph = 1;
    ctx.km.vi.visual.anchor.run = 0;
    ctx.km.vi.visual.anchor.offset = 1;
    ctx.km.vi.visual.caret = ctx.km.vi.visual.anchor;
    ctx.km.vi.cursor = ctx.km.vi.visual.caret;

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_CUT)->handler(&ctx) == WP_OK);
    clipboard_text(text, sizeof(text));
    ASSERT_EQ_STR(text, "Two\n");
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "One\nThree\n");
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 1);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 0);

    ASSERT_TRUE(wp_action_find(WP_ACT_EDIT_PASTE)->handler(&ctx) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "One\nTwo\nThree\n");

    ctx_teardown(&doc, &journal, &ctx);
}

/* ---- The shared file flow ----------------------------------------- */

static u32 g_pick_calls;
static const char *g_pick_path;

static wp_bool pick_gives(wp_app_ctx *ctx, wp_bool save, char *out, u32 cap)
{
    (void)ctx; (void)save;
    g_pick_calls++;
    if (strlen(g_pick_path) >= cap) return WP_FALSE;
    strcpy(out, g_pick_path);
    return WP_TRUE;
}

static wp_bool pick_cancels(wp_app_ctx *ctx, wp_bool save, char *out, u32 cap)
{
    (void)ctx; (void)save; (void)out; (void)cap;
    g_pick_calls++;
    return WP_FALSE;
}

static void read_whole_file(const char *path, char *out, u32 cap)
{
    plat_file *f;
    u32 n = 0;

    ASSERT_TRUE(plat_file_open(path, PLAT_FILE_READ, &f) == WP_OK);
    ASSERT_TRUE(plat_file_read(f, (u8 *)out, cap - 1, &n) == WP_OK);
    plat_file_close(f);
    out[n] = '\0';
}

void test_actions_save_file_picks_the_format_by_extension_and_remembers(void)
{
    static const char txt[] = "build/test_actions_doc.txt";
    static const char rtf[] = "build/test_actions_doc.rtf";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char back[256];

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "");

    ASSERT_TRUE(wp_action_save_file(&ctx, txt, NULL) == WP_OK);
    read_whole_file(txt, back, sizeof(back));
    ASSERT_EQ_STR(back, "Hello World\n");
    ASSERT_EQ_STR(ctx.doc_path, txt);
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "test_actions_doc.txt");
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    ASSERT_TRUE(wp_action_save_file(&ctx, rtf, NULL) == WP_OK);
    read_whole_file(rtf, back, sizeof(back));
    ASSERT_TRUE(strncmp(back, "{\\rtf", 5) == 0);
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "test_actions_doc.rtf");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_save_goes_to_the_remembered_name_without_asking(void)
{
    static const char first[] = "build/test_actions_pick1.txt";
    static const char second[] = "build/test_actions_pick2.txt";
    static const char named[] = "build/test_actions_pick3.txt";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_pos at;
    char back[64];

    ctx_setup(&doc, &journal, &ctx, "One");

    /* No name yet: Save asks. */
    g_pick_calls = 0;
    g_pick_path = first;
    ASSERT_TRUE(wp_action_file_save(&ctx, WP_FALSE, pick_gives, NULL) == WP_OK);
    ASSERT_EQ_U32(g_pick_calls, 1);
    ASSERT_EQ_STR(ctx.doc_path, first);

    /* With a name: Save writes there and asks nothing. */
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, *wp_keymap_cursor(&ctx), (const u8 *)"!", 1, &at) == WP_OK);
    ASSERT_TRUE(wp_action_file_save(&ctx, WP_FALSE, pick_cancels, NULL) == WP_OK);
    ASSERT_EQ_U32(g_pick_calls, 1);
    read_whole_file(first, back, sizeof(back));
    ASSERT_EQ_STR(back, "!One\n");
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    /* Save As always asks, and the answer becomes the name. */
    g_pick_path = second;
    ASSERT_TRUE(wp_action_file_save(&ctx, WP_TRUE, pick_gives, NULL) == WP_OK);
    ASSERT_EQ_U32(g_pick_calls, 2);
    ASSERT_EQ_STR(ctx.doc_path, second);

    /* A cancelled Save As changes nothing. */
    ASSERT_TRUE(wp_action_file_save(&ctx, WP_TRUE, pick_cancels, NULL) == WP_OK);
    ASSERT_EQ_STR(ctx.doc_path, second);

    /* ":w name" names the file itself, and that name sticks. */
    wp_keymap_set_pending_path((const u8 *)named, (u32)strlen(named));
    ASSERT_TRUE(wp_action_file_save(&ctx, WP_FALSE, pick_cancels, NULL) == WP_OK);
    ASSERT_EQ_STR(ctx.doc_path, named);
    read_whole_file(named, back, sizeof(back));
    ASSERT_EQ_STR(back, "!One\n");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_load_file_reads_text_and_rtf_and_keeps_the_name(void)
{
    static const char txt[] = "build/test_actions_load.txt";
    static const char rtf[] = "build/test_actions_load.rtf";
    static const char md[] = "build/test_actions_load.md";
    static const char missing[] = "build/no-such-directory/x.txt";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    /* Through the importer, so "Alpha" and "Beta" are two paragraphs
       (ctx_setup would keep the '\n' inside one run). */
    ctx_setup_imported(&doc, &journal, &ctx, "Alpha\nBeta\n");
    ASSERT_TRUE(wp_action_save_file(&ctx, txt, NULL) == WP_OK);
    ASSERT_TRUE(wp_action_save_file(&ctx, rtf, NULL) == WP_OK);
    ctx_teardown(&doc, &journal, &ctx);

    ctx_setup(&doc, &journal, &ctx, "Something else");
    ASSERT_TRUE(wp_action_load_file(&ctx, txt, NULL) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");
    ASSERT_EQ_STR(ctx.doc_path, txt);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 0);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->offset, 0);
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    /* RTF by name: the exporter's own output reads back as the text. */
    ASSERT_TRUE(wp_action_load_file(&ctx, rtf, NULL) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");
    ASSERT_EQ_STR(ctx.doc_path, rtf);

    /* Markdown by name: paragraphs go out a blank line apart and come
       back as paragraphs. */
    ASSERT_TRUE(wp_action_save_file(&ctx, md, NULL) == WP_OK);
    read_whole_file(md, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\n\nBeta\n");
    ASSERT_TRUE(wp_action_load_file(&ctx, md, NULL) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");
    ASSERT_EQ_STR(ctx.doc_path, md);

    /* A file that cannot be opened leaves document and name alone. */
    ASSERT_TRUE(wp_action_load_file(&ctx, missing, NULL) != WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");
    ASSERT_EQ_STR(ctx.doc_path, md);

    ctx_teardown(&doc, &journal, &ctx);
}

static wp_bool no_link(void) { return WP_FALSE; }

/* ODT and PDF through the real ODT gateway (the one test_convert.c
   drives), by way of the shared file flow this time: the extension
   alone chooses the route, the link is required and its absence is
   told, ODT is a save and PDF an export. */
void test_actions_odt_and_pdf_go_through_the_gateway(void)
{
    static const char odt[] = "build/test_actions_conv.odt";
    static const char pdf[] = "build/test_actions_conv.pdf";
    gateway_handle gw;
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_pos at;
    char text[64];

    ctx_setup_imported(&doc, &journal, &ctx, "Alpha\nBeta\n");

    /* No link: told, nothing remembered. */
    ctx.ensure_offload = no_link;
    ASSERT_TRUE(wp_action_save_file(&ctx, odt, NULL) != WP_OK);
    ASSERT_EQ_STR(ctx.doc_path, "");

    ASSERT_TRUE(gateway_start_bin("offload/gateway/odt_gateway.py", NULL, &gw) == WP_OK);
    ASSERT_TRUE(offload_init(gw.endpoint) == WP_OK);
    ctx.ensure_offload = NULL;   /* the link as it stands */

    /* ODT: a save. The file is a ZIP container, the name sticks, the
       document is clean. */
    ASSERT_TRUE(wp_action_save_file(&ctx, odt, NULL) == WP_OK);
    read_whole_file(odt, text, sizeof(text));
    ASSERT_TRUE(text[0] == 'P' && text[1] == 'K');
    ASSERT_EQ_STR(ctx.doc_path, odt);
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    /* PDF: an export. The document stays modified and keeps its name. */
    ASSERT_TRUE(wp_edit_insert_text(&doc, &journal, *wp_keymap_cursor(&ctx), (const u8 *)"!", 1, &at) == WP_OK);
    ASSERT_TRUE(wp_action_save_file(&ctx, pdf, NULL) == WP_OK);
    read_whole_file(pdf, text, sizeof(text));
    ASSERT_TRUE(strncmp(text, "%PDF", 4) == 0);
    ASSERT_EQ_STR(ctx.doc_path, odt);
    ASSERT_TRUE(wp_document_is_modified(&doc));

    /* ODT back in: the text the gateway was sent. */
    ASSERT_TRUE(wp_action_load_file(&ctx, odt, NULL) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    offload_shutdown();
    gateway_stop(&gw);

    /* Link gone: an ODT open is refused and the document is untouched. */
    ASSERT_TRUE(wp_action_load_file(&ctx, odt, NULL) != WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_file_open_asks_picks_and_a_new_document_forgets_the_name(void)
{
    static const char txt[] = "build/test_actions_open.txt";
    wp_document doc, fresh;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup(&doc, &journal, &ctx, "Saved");
    ASSERT_TRUE(wp_action_save_file(&ctx, txt, NULL) == WP_OK);
    ctx_teardown(&doc, &journal, &ctx);

    ctx_setup(&doc, &journal, &ctx, "Current");
    g_pick_calls = 0;
    g_pick_path = txt;

    /* Cancelled at the picker: nothing changes. */
    ASSERT_TRUE(wp_action_file_open(&ctx, pick_cancels, NULL) == WP_OK);
    ASSERT_EQ_U32(g_pick_calls, 1);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Current\n");
    ASSERT_EQ_STR(ctx.doc_path, "");

    ASSERT_TRUE(wp_action_file_open(&ctx, pick_gives, NULL) == WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Saved\n");
    ASSERT_EQ_STR(ctx.doc_path, txt);

    /* New: no file, so the next Save must ask rather than overwrite. */
    ASSERT_TRUE(format_txt_import((const u8 *)"", 0, &fresh) == WP_OK);
    wp_action_replace_document(&ctx, &fresh);
    ASSERT_EQ_STR(ctx.doc_path, "");
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "");

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_document_name_is_the_last_path_component(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    ctx_setup(&doc, &journal, &ctx, "x");
    wp_action_remember_path(&ctx, "C:\\DOCS\\LETTER.TXT");
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "LETTER.TXT");
    wp_action_remember_path(&ctx, "Work:Docs/letter.rtf");
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "letter.rtf");
    wp_action_remember_path(&ctx, "Macintosh HD:letter");
    ASSERT_EQ_STR(wp_action_document_name(&ctx), "letter");
    ASSERT_TRUE(wp_path_ends_with_ci("notes.rtf", ".RTF"));
    ASSERT_TRUE(wp_path_ends_with_ci("NOTES.RTF", ".rtf"));
    ASSERT_TRUE(!wp_path_ends_with_ci("notes.txt", ".RTF"));
    ASSERT_TRUE(!wp_path_ends_with_ci("rtf", ".RTF"));
    ctx_teardown(&doc, &journal, &ctx);
}

/* ---- Paragraph formatting and the ruler bar -------------------------- */

static const wp_para_style *caret_pstyle(wp_app_ctx *ctx, u32 pi)
{
    wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(ctx->doc, 0), pi);
    return wp_document_pstyle_get(ctx->doc, p->style);
}

void test_actions_paragraph_buttons_change_the_caret_paragraph(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;

    ctx_setup_imported(&doc, &journal, &ctx, "One\nTwo\n");
    wp_keymap_cursor(&ctx)->paragraph = 1;
    wp_keymap_cursor(&ctx)->offset = 0;

    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_JUSTIFY)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(caret_pstyle(&ctx, 1)->align, (u32)WP_ALIGN_JUSTIFY);
    ASSERT_EQ_U32(caret_pstyle(&ctx, 0)->align, (u32)WP_ALIGN_LEFT);   /* only the caret's */
    ASSERT_TRUE(wp_document_is_modified(&doc));

    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_DOUBLE)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 1)->line_spacing, 480);
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_SINGLE)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 1)->line_spacing, 0);

    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_INDENT_MORE)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_INDENT_MORE)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 1)->indent_left, 1440);
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_INDENT_LESS)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_INDENT_LESS)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_INDENT_LESS)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 1)->indent_left, 0);           /* never past the margin */

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_toggle_tab_adds_snaps_and_removes(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    const wp_para_style *ps;

    ctx_setup_imported(&doc, &journal, &ctx, "x\n");
    ASSERT_TRUE(wp_action_toggle_tab(&ctx, 2950) == WP_OK);        /* snaps to 2880 */
    ASSERT_TRUE(wp_action_toggle_tab(&ctx, 1000) == WP_OK);        /* snaps to 1080, sorted first */
    ps = caret_pstyle(&ctx, 0);
    ASSERT_EQ_U32(ps->tab_count, 2);
    ASSERT_EQ_U32((u32)ps->tab_pos[0], 1080);
    ASSERT_EQ_U32((u32)ps->tab_pos[1], 2880);
    ASSERT_TRUE(wp_action_toggle_tab(&ctx, 2900) == WP_OK);        /* near 2880: removes it */
    ps = caret_pstyle(&ctx, 0);
    ASSERT_EQ_U32(ps->tab_count, 1);
    ASSERT_EQ_U32((u32)ps->tab_pos[0], 1080);

    ctx_teardown(&doc, &journal, &ctx);
}

static i32 half(i32 v) { return v / 2; }       /* 1 px = 2 twips, for the bar tests */
static i32 twice(i32 v) { return v * 2; }

void test_rulerbar_hit_test_finds_buttons_gaps_and_the_ruler(void)
{
    wp_rulerbar rb;
    wp_rulerbar_hit hit;

    rb.x = 10; rb.y = 20; rb.w = 600;
    rb.button_w = 20; rb.button_h = 18; rb.icon_rows = 16; rb.scale_h = 12;
    rb.origin_x = 16;
    rb.twips_to_x = half;
    rb.x_to_twips = twice;
    rb.colors = 2;
    ASSERT_EQ_U32((u32)wp_rulerbar_height(&rb), 18 + 2 + 12);

    ASSERT_TRUE(wp_rulerbar_hit_test(&rb, 10, 25, &hit));               /* first button: New */
    ASSERT_TRUE(hit.kind == WP_RULERBAR_BUTTON && hit.action == WP_ACT_FILE_NEW);
    ASSERT_TRUE(!wp_rulerbar_hit_test(&rb, 72, 25, &hit));              /* the gap after Save (70..80) */
    ASSERT_TRUE(wp_rulerbar_hit_test(&rb, 85, 25, &hit));               /* Cut, after the gap */
    ASSERT_TRUE(hit.kind == WP_RULERBAR_BUTTON && hit.action == WP_ACT_EDIT_CUT);
    ASSERT_TRUE(!wp_rulerbar_hit_test(&rb, 72, 5, &hit));               /* above the bar */
    ASSERT_TRUE(wp_rulerbar_hit_test(&rb, 116, 45, &hit));              /* the ruler, 100 px in */
    ASSERT_TRUE(hit.kind == WP_RULERBAR_RULER);
    ASSERT_EQ_U32((u32)hit.twips, 200);

    ASSERT_TRUE(wp_rulerbar_has_action(WP_ACT_PARA_JUSTIFY));
    ASSERT_TRUE(!wp_rulerbar_has_action(WP_ACT_TOOLS_CHAT));
}

void test_rulerbar_signature_follows_what_the_bar_shows(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_rulerbar rb;
    u32 a, b;

    ctx_setup_imported(&doc, &journal, &ctx, "x\n");
    rb.x = 0; rb.y = 0; rb.w = 600; rb.button_w = 20; rb.button_h = 18;
    rb.icon_rows = 16; rb.scale_h = 12; rb.origin_x = 0;
    rb.twips_to_x = half; rb.x_to_twips = twice; rb.colors = 2;

    a = wp_rulerbar_signature(&rb, &ctx);
    ASSERT_EQ_U32(wp_rulerbar_signature(&rb, &ctx), a);                 /* stable */
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_CENTER)->handler(&ctx) == WP_OK);
    b = wp_rulerbar_signature(&rb, &ctx);
    ASSERT_TRUE(b != a);                                                /* alignment shows */
    ASSERT_TRUE(wp_action_toggle_tab(&ctx, 1440) == WP_OK);
    ASSERT_TRUE(wp_rulerbar_signature(&rb, &ctx) != b);                 /* so do tabs */
    ctx.style_flags = (u16)WP_STYLE_BOLD;
    ASSERT_TRUE(wp_rulerbar_signature(&rb, &ctx) != b);                 /* and bold */

    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_page_break_splits_and_undo_merges(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char text[64];

    ctx_setup_imported(&doc, &journal, &ctx, "Chapter one ends. Chapter two\n");
    wp_keymap_cursor(&ctx)->paragraph = 0;
    wp_keymap_cursor(&ctx)->run = 0;
    wp_keymap_cursor(&ctx)->offset = 18;   /* before "Chapter two" */

    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_PAGE_BREAK)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 2);
    ASSERT_TRUE(!caret_pstyle(&ctx, 0)->page_break_before);
    ASSERT_TRUE(caret_pstyle(&ctx, 1)->page_break_before);
    ASSERT_EQ_U32(wp_keymap_cursor(&ctx)->paragraph, 1);

    /* Enter at the end of the breaking paragraph: the new one does not break. */
    ASSERT_TRUE(wp_edit_split_paragraph(&doc, &journal, *wp_keymap_cursor(&ctx), NULL) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 3);
    ASSERT_TRUE(caret_pstyle(&ctx, 1)->page_break_before);
    ASSERT_TRUE(!caret_pstyle(&ctx, 2)->page_break_before);

    /* At a paragraph's start the button toggles that paragraph's break. */
    wp_keymap_cursor(&ctx)->paragraph = 1;
    wp_keymap_cursor(&ctx)->run = 0;
    wp_keymap_cursor(&ctx)->offset = 0;
    ASSERT_TRUE(wp_action_find(WP_ACT_PARA_PAGE_BREAK)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 3);
    ASSERT_TRUE(!caret_pstyle(&ctx, 1)->page_break_before);

    /* Undo the splits: one paragraph again, the text whole. */
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 1);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Chapter one ends. Chapter two\n");

    ctx_teardown(&doc, &journal, &ctx);
}

static wp_key_event key_char(u32 cp)
{
    wp_key_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.codepoint = cp;
    return ev;
}

static wp_key_event key_special(wp_special_key k)
{
    wp_key_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.special = (u16)k;
    return ev;
}

void test_actions_header_prompt_types_edits_and_sets(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char status[96];
    wp_vi_status_view view;
    const char *typed = "Title|#x";
    u32 i;

    ctx_setup_imported(&doc, &journal, &ctx, "Body\n");
    ASSERT_TRUE(wp_action_find(WP_ACT_PAGE_HEADER)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(ctx.prompt.active);

    /* Every key goes to the prompt, none to the document. */
    for (i = 0; typed[i] != '\0'; i++) (void)wp_keymap_feed_key(&ctx, key_char((u8)typed[i]), 0);
    (void)wp_keymap_feed_key(&ctx, key_special(WP_KEY_BACKSPACE), 0);
    wp_keymap_get_status_view(&ctx, status, sizeof(status), &view);
    ASSERT_TRUE(view.mode_label_len > 0);
    ASSERT_TRUE(strstr(status, "Title|#_") != NULL);
    ASSERT_TRUE(!wp_document_is_modified(&doc));

    (void)wp_keymap_feed_key(&ctx, key_special(WP_KEY_ENTER), 0);
    ASSERT_TRUE(!ctx.prompt.active);
    ASSERT_EQ_U32(doc.sections[0].header.length, 7);
    ASSERT_TRUE(memcmp(wp_strpool_get(&doc.strings, doc.sections[0].header), "Title|#", 7) == 0);
    ASSERT_TRUE(wp_document_is_modified(&doc));

    /* Escape leaves it as it was; the prompt starts from the current text. */
    ASSERT_TRUE(wp_action_find(WP_ACT_PAGE_HEADER)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(ctx.prompt.len, 7);
    (void)wp_keymap_feed_key(&ctx, key_char('Z'), 0);
    (void)wp_keymap_feed_key(&ctx, key_special(WP_KEY_ESCAPE), 0);
    ASSERT_EQ_U32(doc.sections[0].header.length, 7);

    ctx_teardown(&doc, &journal, &ctx);
}

/* -- Typefaces and the input style ------------------------------------ */

static void type_key(wp_app_ctx *ctx, wp_key_event ev)
{
    wp_pos before = *wp_keymap_cursor(ctx);
    wp_keymap_outcome o = wp_keymap_feed_key(ctx, ev, 0);
    wp_action_after_key(ctx, before, o.inserted_char);
}

static const wp_char_style *run_style(wp_document *doc, u16 ri)
{
    wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(doc, 0), 0);
    return (ri < p->run_count) ? wp_document_cstyle_get(doc, p->runs[ri].style) : NULL;
}

void test_actions_font_restyles_the_selection_as_one_step(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_font times;

    ctx_setup(&doc, &journal, &ctx, "Hello World");
    /* The host's faces are Helvetica, Times and Courier, and its dialog
       picks the default -- which is the current font, found among them
       whatever its case. */
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"times", 5, WP_FAMILY_ROMAN, &times) == WP_OK);
    ctx.current_font = times;
    select_vi_visual(&ctx, 0, 5);

    ASSERT_TRUE(wp_action_find(WP_ACT_FORMAT_FONT)->handler(&ctx) == WP_OK);
    ASSERT_EQ_U32(run_style(&doc, 0)->font, times);
    ASSERT_EQ_U32(run_style(&doc, 1)->font, 0);          /* " World": not selected */
    ASSERT_TRUE(wp_edit_undo(&doc, &journal) == WP_OK);
    ASSERT_EQ_U32(run_style(&doc, 0)->font, 0);
    ctx_teardown(&doc, &journal, &ctx);
}

void test_actions_typed_text_keeps_its_neighbours_style_unless_armed(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_font courier;

    ctx_setup(&doc, &journal, &ctx, "Hello");
    ASSERT_TRUE(wp_document_font_find_or_add(&doc, (const u8 *)"Courier", 7, WP_FAMILY_MODERN, &courier) == WP_OK);

    /* Not armed: an input style left over from somewhere else does not
       restamp what is typed -- it takes the text's own style. */
    ctx.current_font = courier;
    ctx.style_flags = WP_STYLE_BOLD;
    type_key(&ctx, key_char('i'));                       /* vi: Insert */
    type_key(&ctx, key_char('a'));
    ASSERT_EQ_U32(run_style(&doc, 0)->font, 0);
    ASSERT_EQ_U32(run_style(&doc, 0)->flags, 0);

    /* Bold with nothing selected arms it at the caret: what is typed
       there is bold (and in the input face), and stays so while typing
       goes on from the same place. */
    ctx.current_font = courier;
    ctx.style_flags = 0;
    ASSERT_TRUE(wp_action_find(WP_ACT_FORMAT_BOLD)->handler(&ctx) == WP_OK);
    ASSERT_TRUE(ctx.input_armed);
    type_key(&ctx, key_char('b'));
    type_key(&ctx, key_char('c'));
    {
        wp_pos at = *wp_keymap_cursor(&ctx);
        const wp_char_style *cs = run_style(&doc, (u16)at.run);
        ASSERT_TRUE(cs != NULL && (cs->flags & WP_STYLE_BOLD) != 0);
        ASSERT_EQ_U32(cs->font, courier);
    }

    /* Moving the caret disarms it and reads the style at the new place
       back: the start of the document is plain, default-face text. */
    type_key(&ctx, key_special(WP_KEY_ESCAPE));
    type_key(&ctx, key_char('0'));
    ASSERT_TRUE(!ctx.input_armed);
    ASSERT_EQ_U32(ctx.style_flags, 0);
    ASSERT_EQ_U32(ctx.current_font, 0);
    ctx_teardown(&doc, &journal, &ctx);
}

void test_rulerbar_has_font_and_size(void)
{
    ASSERT_TRUE(wp_rulerbar_has_action(WP_ACT_FORMAT_FONT));
    ASSERT_TRUE(wp_rulerbar_has_action(WP_ACT_FORMAT_SIZE));
}

/* From 16 colours the bar is a grey panel of raised buttons, with a
   pressed one sunk; below, the flat black-on-white it always was -- no
   grey face at all, since four colours have none to give. */
void test_rulerbar_panel_look_follows_the_colour_count(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_rulerbar rb;
    u32 sig2;

    ctx_setup_imported(&doc, &journal, &ctx, "x\n");
    rb.x = 0; rb.y = 0; rb.w = 640; rb.button_w = 22; rb.button_h = 20;
    rb.icon_rows = 16; rb.scale_h = 12; rb.origin_x = 0;
    rb.twips_to_x = half; rb.x_to_twips = twice;

    rb.colors = 4;
    draw_host_reset_fills();
    wp_rulerbar_paint(NULL, NULL, 0, &rb, &ctx);
    ASSERT_EQ_U32(draw_host_grey_fills(0xC0), 0);
    ASSERT_EQ_U32(draw_host_grey_fills(0xE0), 0);
    sig2 = wp_rulerbar_signature(&rb, &ctx);

    rb.colors = 256;
    draw_host_reset_fills();
    wp_rulerbar_paint(NULL, NULL, 0, &rb, &ctx);
    ASSERT_EQ_U32(draw_host_grey_fills(0xC0), 1);       /* the panel, once */
    ASSERT_TRUE(draw_host_grey_fills(0x80) > 20);       /* every button's shadow */
    ASSERT_EQ_U32(draw_host_grey_fills(0xE0), 2);       /* the two pressed: Align Left, Single */
    ASSERT_EQ_U32(draw_host_mono_calls(), 24);          /* one blit per icon, not a fill per run */
    ASSERT_TRUE(wp_rulerbar_signature(&rb, &ctx) != sig2);   /* a repaint when it changes */

    ctx_teardown(&doc, &journal, &ctx);
}

/* Dragging the ruler's markers. The bar here is 1 px = 2 twips, the
   strip from y 20 to 31 (upper half the first-line marker's), and the
   column 9360 twips wide, so the right indent marker starts at x 4680. */
static void drag_bar(wp_rulerbar *rb)
{
    rb->x = 0; rb->y = 0; rb->w = 640; rb->button_w = 20; rb->button_h = 18;
    rb->icon_rows = 16; rb->scale_h = 12; rb->origin_x = 0;
    rb->twips_to_x = half; rb->x_to_twips = twice; rb->colors = 2;
}

static void drag(wp_app_ctx *ctx, const wp_rulerbar *rb, i32 x0, i32 y0, i32 x1, i32 y1)
{
    wp_rulerbar_drag d;
    ASSERT_TRUE(wp_rulerbar_drag_begin(rb, ctx, x0, y0, &d));
    (void)wp_rulerbar_drag_move(rb, &d, (x0 + x1) / 2, (y0 + y1) / 2);
    (void)wp_rulerbar_drag_move(rb, &d, x1, y1);
    ASSERT_TRUE(wp_rulerbar_drag_end(ctx, &d) == WP_OK);
}

void test_rulerbar_dragging_indents(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_rulerbar rb;
    wp_rulerbar_drag d;

    ctx_setup_imported(&doc, &journal, &ctx, "x\n");
    drag_bar(&rb);

    /* The left indent (lower half) to an inch: the first line stays at
       the margin, so it hangs. Snapped: 724 px is 1448 twips, an inch. */
    drag(&ctx, &rb, 0, 29, 724, 29);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 0)->indent_left, 1440);
    ASSERT_TRUE(caret_pstyle(&ctx, 0)->indent_first == -1440);

    /* The first-line marker (upper half, still at the margin) to 1.5 in. */
    drag(&ctx, &rb, 0, 21, 1080, 21);
    ASSERT_TRUE(caret_pstyle(&ctx, 0)->indent_first == 720);

    /* The right indent in by an inch. */
    drag(&ctx, &rb, 4680, 29, 3960, 29);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 0)->indent_right, 1440);

    /* Pulled off the ruler, an indent stays on it. */
    drag(&ctx, &rb, 3960, 29, 3240, 200);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 0)->indent_right, 2880);

    /* Off the strip is no drag at all. */
    ASSERT_TRUE(!wp_rulerbar_drag_begin(&rb, &ctx, 100, 5, &d));
    ctx_teardown(&doc, &journal, &ctx);
}

void test_rulerbar_dragging_tabs(void)
{
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_rulerbar rb;
    wp_rulerbar_drag d;

    ctx_setup_imported(&doc, &journal, &ctx, "x\n");
    drag_bar(&rb);

    /* A click with no movement sets a stop, as a click always did. */
    ASSERT_TRUE(wp_rulerbar_drag_begin(&rb, &ctx, 720, 29, &d));
    ASSERT_TRUE(!wp_rulerbar_drag_move(&rb, &d, 721, 29));          /* a wobble */
    ASSERT_TRUE(wp_rulerbar_drag_end(&ctx, &d) == WP_OK);
    ASSERT_EQ_U32(caret_pstyle(&ctx, 0)->tab_count, 1);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 0)->tab_pos[0], 1440);

    /* Dragged along, it moves; its kind goes with it. */
    drag(&ctx, &rb, 720, 29, 1440, 29);
    ASSERT_EQ_U32(caret_pstyle(&ctx, 0)->tab_count, 1);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 0)->tab_pos[0], 2880);

    /* Dragged off the ruler, it goes. */
    drag(&ctx, &rb, 1440, 29, 1500, 120);
    ASSERT_EQ_U32(caret_pstyle(&ctx, 0)->tab_count, 0);

    /* A drag on bare ruler sets a stop where it is let go. */
    drag(&ctx, &rb, 300, 29, 900, 29);
    ASSERT_EQ_U32(caret_pstyle(&ctx, 0)->tab_count, 1);
    ASSERT_EQ_U32((u32)caret_pstyle(&ctx, 0)->tab_pos[0], 1800);
    ctx_teardown(&doc, &journal, &ctx);
}
