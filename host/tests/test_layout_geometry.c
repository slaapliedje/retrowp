/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "layout.h"
#include "format_txt.h"

/* Line geometry from the layout: indent, alignment, spacing and tabs.
   Host stub metrics (see test_layout.c): 120 twips a character and a
   300-twip line at the default 12 pt. Every number below is worked out
   from those two. */

static wp_paragraph *one_para(wp_document *doc, const char *text)
{
    /* No ASSERT here: it returns from a void function. A failed import
       leaves NULL, and the caller's first use of it fails the test. */
    if (format_txt_import((const u8 *)text, (u32)strlen(text), doc) != WP_OK) return NULL;
    return wp_section_paragraph_get(wp_document_section_get(doc, 0), 0);
}

static void set_style(wp_document *doc, wp_paragraph *para, const wp_para_style *ps)
{
    wp_pstyle h;
    ASSERT_TRUE(wp_document_pstyle_find_or_add(doc, ps, &h) == WP_OK);
    wp_paragraph_pstyle_set(para, h);
}

static wp_paragraph_layout *relayout(wp_document *doc, wp_paragraph *para, i32 width)
{
    if (wp_paragraph_relayout(doc, para, width) != WP_OK) return NULL;
    return (wp_paragraph_layout *)para->layout;
}

void test_layout_geometry_indent_and_alignment_place_the_line(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "ab cd\n");
    wp_para_style ps;
    wp_paragraph_layout *l;

    memset(&ps, 0, sizeof(ps));
    ps.indent_left = 600;
    ps.indent_right = 300;
    ps.indent_first = 240;

    /* Left: the content starts at the indent, first line's included. */
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 3000);
    ASSERT_EQ_U32((u32)l->lines[0].x_twips, 840);
    ASSERT_EQ_U32((u32)l->lines[0].avail_twips, 1860);   /* 3000 - 600 - 300 - 240 */
    ASSERT_EQ_U32((u32)l->lines[0].width_twips, 600);

    /* Centre and right: inside the indented width, not the column. */
    ps.align = WP_ALIGN_CENTER;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 3000);
    ASSERT_EQ_U32((u32)l->lines[0].x_twips, 840 + (1860 - 600) / 2);
    ps.align = WP_ALIGN_RIGHT;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 3000);
    ASSERT_EQ_U32((u32)l->lines[0].x_twips, 840 + 1860 - 600);

    wp_document_free(&doc);
}

void test_layout_geometry_later_lines_use_the_left_indent_only(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "aaaa bbbb\n");
    wp_para_style ps;
    wp_paragraph_layout *l;

    memset(&ps, 0, sizeof(ps));
    ps.indent_left = 100;
    ps.indent_first = -100;   /* a hanging indent */
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 700);
    ASSERT_EQ_U32(l->line_count, 2);
    ASSERT_EQ_U32((u32)l->lines[0].x_twips, 0);
    ASSERT_EQ_U32((u32)l->lines[1].x_twips, 100);

    wp_document_free(&doc);
}

void test_layout_geometry_spacing_shapes_the_line_boxes(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "aaaa bbbb\n");
    wp_para_style ps;
    wp_paragraph_layout *l;

    memset(&ps, 0, sizeof(ps));
    ps.space_before = 100;
    ps.space_after = 200;
    ps.line_spacing = 360;   /* one and a half */
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 600);
    ASSERT_EQ_U32(l->line_count, 2);
    ASSERT_EQ_U32((u32)l->lines[0].text_top_twips, 100);
    ASSERT_EQ_U32((u32)l->lines[0].text_height_twips, 300);
    ASSERT_EQ_U32((u32)l->lines[0].height_twips, 100 + 450);
    ASSERT_EQ_U32((u32)l->lines[1].text_top_twips, 0);
    ASSERT_EQ_U32((u32)l->lines[1].height_twips, 450 + 200);

    /* "At least": a minimum, never a squeeze. */
    ps.space_before = ps.space_after = 0;
    ps.line_spacing = -500;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 600);
    ASSERT_EQ_U32((u32)l->lines[0].height_twips, 500);
    ps.line_spacing = -200;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 600);
    ASSERT_EQ_U32((u32)l->lines[0].height_twips, 300);

    wp_document_free(&doc);
}

void test_layout_geometry_default_tabs_advance_to_half_inches(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "a\tb\n");
    wp_para_style ps;
    wp_paragraph_layout *l;

    l = relayout(&doc, para, 6000);
    ASSERT_EQ_U32(l->lines[0].tab_count, 1);
    ASSERT_EQ_U32((u32)l->tab_widths[0], 600);           /* 120 -> 720 */
    ASSERT_EQ_U32((u32)l->lines[0].width_twips, 840);

    /* Stops are measured from the column, not the indent. */
    memset(&ps, 0, sizeof(ps));
    ps.indent_left = 1000;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 6000);
    ASSERT_EQ_U32((u32)l->tab_widths[0], 1440 - 1120);

    wp_document_free(&doc);
}

void test_layout_geometry_a_tab_can_start_a_line(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "\tx\n");
    wp_paragraph_layout *l = relayout(&doc, para, 6000);

    ASSERT_EQ_U32((u32)l->tab_widths[0], 720);
    ASSERT_EQ_U32((u32)l->lines[0].width_twips, 840);
    ASSERT_EQ_U32(l->lines[0].start_offset, 0);   /* the tab is on the line */

    wp_document_free(&doc);
}

void test_layout_geometry_right_and_centre_stops(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "a\tbcd\n");
    wp_para_style ps;
    wp_paragraph_layout *l;

    memset(&ps, 0, sizeof(ps));
    ps.tab_count = 1;
    ps.tab_pos[0] = 3000;
    ps.tab_kind[0] = (u8)WP_TAB_RIGHT;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 6000);
    ASSERT_EQ_U32((u32)l->tab_widths[0], 3000 - 120 - 360);   /* "bcd" ends on the stop */
    ASSERT_EQ_U32((u32)l->lines[0].width_twips, 3000);

    ps.tab_pos[0] = 2000;
    ps.tab_kind[0] = (u8)WP_TAB_CENTER;
    set_style(&doc, para, &ps);
    l = relayout(&doc, para, 6000);
    ASSERT_EQ_U32((u32)l->tab_widths[0], 2000 - 120 - 180);   /* "bcd" centred on it */

    /* Past the last explicit stop, the default grid takes over. */
    ASSERT_EQ_U32((u32)wp_tab_next_stop(&ps, 2500, NULL), 2880);
    ASSERT_EQ_U32((u32)wp_tab_next_stop(NULL, 720, NULL), 1440);

    wp_document_free(&doc);
}

void test_layout_geometry_fragments_caret_and_clicks_agree_on_tabs(void)
{
    wp_document doc;
    wp_paragraph *para = one_para(&doc, "a\tb\n");
    wp_paragraph_layout *l = relayout(&doc, para, 6000);
    const wp_line *line = &l->lines[0];
    wp_line_fragment_iter it;
    wp_line_fragment f;
    u32 run, off;

    wp_line_fragment_iter_init(&it, line);
    ASSERT_TRUE(wp_line_next_fragment(&doc, para, line, &it, &f));
    ASSERT_TRUE(!f.is_tab && f.byte_end == 1);
    ASSERT_TRUE(wp_line_next_fragment(&doc, para, line, &it, &f));
    ASSERT_TRUE(f.is_tab && f.byte_start == 1 && f.byte_end == 2);
    ASSERT_EQ_U32((u32)f.tab_twips, 600);
    ASSERT_TRUE(wp_line_next_fragment(&doc, para, line, &it, &f));
    ASSERT_TRUE(!f.is_tab && f.byte_start == 2);
    ASSERT_TRUE(!wp_line_next_fragment(&doc, para, line, &it, &f));

    ASSERT_EQ_U32((u32)wp_line_cursor_x_twips(&doc, para, line, 0, 1, 0), 120);
    ASSERT_EQ_U32((u32)wp_line_cursor_x_twips(&doc, para, line, 0, 2, 0), 720);
    ASSERT_EQ_U32((u32)wp_line_cursor_x_twips(&doc, para, line, 0, 3, 0), 840);

    ASSERT_TRUE(wp_line_pos_at_x_twips(&doc, para, line, 300, 0, &run, &off));
    ASSERT_EQ_U32(off, 1);   /* nearer the tab's start */
    ASSERT_TRUE(wp_line_pos_at_x_twips(&doc, para, line, 600, 0, &run, &off));
    ASSERT_EQ_U32(off, 2);   /* nearer its end */

    /* A tabbed line is never stretched. */
    ASSERT_EQ_U32((u32)wp_justify_space_extra_twips(&doc, para, line, WP_ALIGN_JUSTIFY,
                                                    WP_FALSE, 6000), 0);

    wp_document_free(&doc);
}

void test_layout_geometry_styles_with_different_tabs_stay_apart(void)
{
    wp_document doc;
    wp_para_style a, b;
    wp_pstyle ha, hb;

    (void)one_para(&doc, "x\n");
    memset(&a, 0, sizeof(a));
    a.tab_count = 1;
    a.tab_pos[0] = 1000;
    b = a;
    b.tab_kind[0] = (u8)WP_TAB_RIGHT;
    ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &a, &ha) == WP_OK);
    ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &b, &hb) == WP_OK);
    ASSERT_TRUE(ha != hb);
    ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &a, &hb) == WP_OK);
    ASSERT_TRUE(ha == hb);

    wp_document_free(&doc);
}
