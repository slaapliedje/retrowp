/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "help_view.h"
#include "mem_host.h"

/* Host's plat_font_open gives a deterministic "fake monospace" font
   (platform/host/metrics_host.c): plat_measure_text always reports
   codepoint_count * (size_twips / 2) twips, with no real glyph shapes
   involved -- exactly what lets wp_help_wrap_line's wrap points be
   hand-computed and asserted exactly, the same precedent
   test_layout_greedy_wrap_hand_computed_fixture already set for the
   main engine's own linebreak.c. A 240-twip (12pt) font here gives a
   clean 120-twips-per-codepoint cell. */
static void open_test_font(plat_font **out)
{
    ASSERT_TRUE(plat_font_open((const u8 *)"", 0, 240, 0, out) == WP_OK);
}

/* "one two three four" at 120 twips/char: one=360,sp=120,two=360,
   sp=120,three=600,sp=120,four=480. At max_width=850: "one two"
   (360+120+360=840<=850, +" three" would be 1560>850) / "three"
   (600, +" four" would be 1200>850) / "four". Three rows, hand-verified
   -- see this test's own comment for the arithmetic. */
void test_help_wrap_line_greedy_word_wrap_hand_computed(void)
{
    static const char text[] = "one two three four";
    plat_font *font;
    u32 starts[8], lens[8];
    u32 row_count;

    open_test_font(&font);
    row_count = wp_help_wrap_line(font, (const u8 *)text, (u32)strlen(text), 850,
                                   starts, lens, 8);

    ASSERT_EQ_U32(row_count, 3);
    ASSERT_EQ_U32(starts[0], 0); ASSERT_EQ_U32(lens[0], 7);  /* "one two" */
    ASSERT_EQ_U32(starts[1], 8); ASSERT_EQ_U32(lens[1], 5);  /* "three" */
    ASSERT_EQ_U32(starts[2], 14); ASSERT_EQ_U32(lens[2], 4); /* "four" */

    plat_font_close(font);
}

/* A single word wider than max_width_twips is still placed alone on
   its own row rather than producing zero rows or looping forever --
   the documented single-overlong-word allowance (help_view.c). */
void test_help_wrap_line_overlong_word_gets_its_own_row(void)
{
    static const char text[] = "hello";
    plat_font *font;
    u32 starts[4], lens[4];
    u32 row_count;

    open_test_font(&font);
    /* "hello" = 5 * 120 = 600 twips; ask for a much narrower column. */
    row_count = wp_help_wrap_line(font, (const u8 *)text, (u32)strlen(text), 100,
                                   starts, lens, 4);

    ASSERT_EQ_U32(row_count, 1);
    ASSERT_EQ_U32(starts[0], 0);
    ASSERT_EQ_U32(lens[0], 5);

    plat_font_close(font);
}

void test_help_wrap_line_empty_text_yields_zero_rows(void)
{
    plat_font *font;
    u32 starts[4], lens[4];
    u32 row_count;

    open_test_font(&font);
    row_count = wp_help_wrap_line(font, (const u8 *)"", 0, 1000, starts, lens, 4);
    ASSERT_EQ_U32(row_count, 0);

    plat_font_close(font);
}

void test_help_wrap_line_respects_max_rows_cap(void)
{
    static const char text[] = "a a a a a a a a";
    plat_font *font;
    u32 starts[2], lens[2];
    u32 row_count;

    open_test_font(&font);
    /* Each "a" alone fits, but no two fit together at this width, so
       every word would be its own row -- cap output at 2 regardless. */
    row_count = wp_help_wrap_line(font, (const u8 *)text, (u32)strlen(text), 60,
                                   starts, lens, 2);
    ASSERT_EQ_U32(row_count, 2);

    plat_font_close(font);
}

static void build_two_node_doc(wp_help_doc *doc)
{
    static const char src[] =
        "@node One \"First\"\n"
        "Intro text.\n"
        "{Go to Two|Two}\n"
        "{Go to Missing|NoSuchNode}\n"
        "@endnode\n"
        "@node Two \"Second\"\n"
        "Other text.\n"
        "@endnode\n";
    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), doc) == WP_OK);
}

static wp_key_event key_special(wp_special_key special)
{
    wp_key_event key;
    key.codepoint = 0;
    key.special = (u16)special;
    key.mods = 0;
    key._pad = 0;
    return key;
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

void test_help_view_init_clamps_start_node(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    build_two_node_doc(&doc);

    wp_help_view_init(&vs, &doc, 1);
    ASSERT_EQ_U32(vs.current_node, 1);

    wp_help_view_init(&vs, &doc, 99);
    ASSERT_EQ_U32(vs.current_node, 0);

    wp_help_view_init(&vs, NULL, 0);
    ASSERT_EQ_U32(vs.current_node, WP_HELP_NODE_NONE);

    wp_help_doc_free(&doc);
}

void test_help_view_escape_and_q_request_close(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0);

    ASSERT_TRUE(wp_help_view_feed_key(&vs, key_special(WP_KEY_ESCAPE)) == WP_TRUE);
    ASSERT_TRUE(wp_help_view_feed_key(&vs, key_char('q')) == WP_TRUE);
    ASSERT_TRUE(wp_help_view_feed_key(&vs, key_char('x')) == WP_FALSE);

    wp_help_doc_free(&doc);
}

void test_help_view_link_selection_clamped_at_bounds(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0); /* node "One" has 2 links */

    ASSERT_EQ_U32(vs.selected_link, 0);
    wp_help_view_feed_key(&vs, key_special(WP_KEY_UP)); /* already at 0 -- stays */
    ASSERT_EQ_U32(vs.selected_link, 0);

    wp_help_view_feed_key(&vs, key_char('j'));
    ASSERT_EQ_U32(vs.selected_link, 1);
    wp_help_view_feed_key(&vs, key_char('j')); /* already at last link -- stays */
    ASSERT_EQ_U32(vs.selected_link, 1);

    wp_help_view_feed_key(&vs, key_char('k'));
    ASSERT_EQ_U32(vs.selected_link, 0);

    wp_help_doc_free(&doc);
}

void test_help_view_enter_follows_link_and_backspace_goes_back(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0);

    wp_help_view_feed_key(&vs, key_special(WP_KEY_ENTER)); /* follows link 0: "Two" */
    ASSERT_EQ_U32(vs.current_node, 1);
    ASSERT_EQ_U32(vs.history_count, 1);
    ASSERT_EQ_U32(vs.selected_link, 0);

    wp_help_view_feed_key(&vs, key_special(WP_KEY_BACKSPACE));
    ASSERT_EQ_U32(vs.current_node, 0);
    ASSERT_EQ_U32(vs.history_count, 0);

    /* Backspace with no history left is a no-op, not a crash. */
    wp_help_view_feed_key(&vs, key_special(WP_KEY_BACKSPACE));
    ASSERT_EQ_U32(vs.current_node, 0);

    wp_help_doc_free(&doc);
}

void test_help_view_enter_on_broken_link_target_is_a_noop(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0);

    wp_help_view_feed_key(&vs, key_char('j')); /* select link 1: "NoSuchNode" */
    ASSERT_EQ_U32(vs.selected_link, 1);
    wp_help_view_feed_key(&vs, key_special(WP_KEY_ENTER));
    ASSERT_EQ_U32(vs.current_node, 0); /* unchanged -- broken target ignored */
    ASSERT_EQ_U32(vs.history_count, 0);

    wp_help_doc_free(&doc);
}

void test_help_view_page_scroll_clamped_at_zero(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0);

    wp_help_view_feed_key(&vs, key_special(WP_KEY_PAGE_UP)); /* already at 0 */
    ASSERT_EQ_U32(vs.scroll_row, 0);

    wp_help_view_feed_key(&vs, key_special(WP_KEY_PAGE_DOWN));
    ASSERT_TRUE(vs.scroll_row > 0);

    wp_help_view_feed_key(&vs, key_special(WP_KEY_PAGE_UP));
    ASSERT_EQ_U32(vs.scroll_row, 0);

    wp_help_doc_free(&doc);
}

/* draw_host.c's plat_draw_text/plat_fill_rect are no-ops (see this
   file's own header comment) -- this can only prove wp_help_view_paint
   runs without crashing across representative states (empty doc,
   scrolled, a selected link, a node with no links at all), the same
   boundary test_statusline_paint_does_not_crash_with_various_views
   already established. */
void test_help_view_paint_does_not_crash_with_various_states(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;
    plat_window *win;
    plat_gc *gc;
    plat_font *font;

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0);

    ASSERT_TRUE(plat_window_create((const u8 *)"t", 1, 640, 400, &win) == WP_OK);
    ASSERT_TRUE(plat_window_get_gc(win, &gc) == WP_OK);
    open_test_font(&font);

    wp_help_view_paint(gc, font, 0, 0, 640, 380, 6000, 20, 16, &vs);

    vs.scroll_row = 50; /* well past the real content -- must not crash */
    wp_help_view_paint(gc, font, 0, 0, 640, 380, 6000, 20, 16, &vs);

    wp_help_view_feed_key(&vs, key_char('j'));
    wp_help_view_paint(gc, font, 0, 0, 640, 380, 6000, 20, 16, &vs);

    wp_help_view_paint(gc, font, 0, 0, 640, 380, 6000, 20, 16, NULL);

    {
        wp_help_view_state empty_vs;
        wp_help_view_init(&empty_vs, NULL, 0);
        wp_help_view_paint(gc, font, 0, 0, 640, 380, 6000, 20, 16, &empty_vs);
    }

    plat_font_close(font);
    plat_window_destroy(win);
    wp_help_doc_free(&doc);
}

void test_help_view_no_leaks(void)
{
    wp_help_doc doc;
    wp_help_view_state vs;

    mem_host_reset_counters();

    build_two_node_doc(&doc);
    wp_help_view_init(&vs, &doc, 0);
    wp_help_view_feed_key(&vs, key_special(WP_KEY_ENTER));
    wp_help_view_feed_key(&vs, key_special(WP_KEY_BACKSPACE));
    wp_help_doc_free(&doc);

    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
