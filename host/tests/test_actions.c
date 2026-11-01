/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "actions.h"
#include "format_txt.h"

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

/* Every action must be reachable by a human: it either sits in a real
   native menu group, or it has a keyboard shortcut, or both. An entry
   with neither is dead weight that no backend can ever invoke. */
void test_actions_every_action_is_reachable(void)
{
    u32 i;

    for (i = 0; i < WP_ACT_COUNT; i++) {
        ASSERT_TRUE(g_wp_actions[i].group != WP_MENU_NONE ||
                    g_wp_actions[i].shortcut_ascii != 0);
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

    /* A file that cannot be opened leaves document and name alone. */
    ASSERT_TRUE(wp_action_load_file(&ctx, missing, NULL) != WP_OK);
    doc_text(&doc, text, sizeof(text));
    ASSERT_EQ_STR(text, "Alpha\nBeta\n");
    ASSERT_EQ_STR(ctx.doc_path, rtf);

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
