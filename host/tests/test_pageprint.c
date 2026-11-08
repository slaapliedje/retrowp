/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "pageprint.h"
#include "format_txt.h"
#include "strpool.h"
#include "draw_host.h"

/* wp_pageprint against the host backend: device pixels are twips, and
   the host font is 12 pt = 240 twips ascent, 60 descent, 120 a
   character, so a line is 300 twips. */

static i32 same(i32 twips) { return twips; }

static u32 g_ends;
static wp_status count_end(void *user)
{
    WP_UNUSED(user);
    g_ends++;
    return WP_OK;
}

/* Four one-word paragraphs on pages of three lines: margins 500 across,
   600 down, the header "Left|#". */
static void make_doc(wp_document *doc)
{
    static const char text[] = "One\nTwo\nThree\nFour\n";
    wp_section *sec;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), doc) == WP_OK);
    sec = wp_document_section_get(doc, 0);
    sec->page_width = 4000;
    sec->margin_left = sec->margin_right = 500;
    sec->margin_top = sec->margin_bottom = 600;
    sec->page_height = 600 + 3 * 300 + 600;
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)"Left|#", 6, &sec->header) == WP_OK);
}

static void expect_text(u32 index, const char *text, i32 x, i32 y)
{
    i32 gx = 0, gy = 0;
    const char *got = draw_host_text_at(index, &gx, &gy);

    ASSERT_TRUE(got != NULL);
    if (got == NULL) return;
    ASSERT_EQ_STR(got, text);
    ASSERT_EQ_U32((u32)gx, (u32)x);
    ASSERT_EQ_U32((u32)gy, (u32)y);
}

/* Two pages: each starts with its header -- "Left" at the left margin,
   the page number against the right one, both on the top margin's
   middle line -- then its lines at the left margin, a line's baseline
   its ascent below its top. Each page ends, and the layouts are left
   dirty for the screen. */
void test_pageprint_pages(void)
{
    wp_document doc;
    wp_page_device dev;
    u32 pages = 0;
    plat_gc *gc = (plat_gc *)&dev;   /* the host ignores it; it only must not be NULL */

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    make_doc(&doc);
    memset(&dev, 0, sizeof(dev));
    dev.gc = gc;
    dev.x_px = same;
    dev.y_px = same;
    dev.end_page = count_end;
    g_ends = 0;
    draw_host_reset_text();

    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(pages, 2);
    ASSERT_EQ_U32(g_ends, 2);
    ASSERT_EQ_U32(draw_host_text_count(), 8);
    expect_text(0, "Left", 500, 390);        /* 300 - (240 + 60) / 2 + 240 */
    expect_text(1, "1", 3500 - 120, 390);
    expect_text(2, "One", 500, 840);         /* 600 + 240 */
    expect_text(3, "Two", 500, 1140);
    expect_text(4, "Three", 500, 1440);
    expect_text(5, "Left", 500, 390);
    expect_text(6, "2", 3380, 390);
    expect_text(7, "Four", 500, 840);
    ASSERT_TRUE(wp_document_section_get(&doc, 0)->paras[0].layout_dirty);
    wp_document_free(&doc);
}

/* A printable area 200 smaller than the paper each way: the paper is
   centred on it, so everything lands 100 up and to the left. An empty
   document prints nothing and ends no page. */
void test_pageprint_printable_area(void)
{
    wp_document doc;
    wp_page_device dev;
    u32 pages = 9;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    make_doc(&doc);
    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = same;
    dev.y_px = same;
    dev.device_w_px = 4000 - 200;
    dev.device_h_px = 2100 - 200;
    dev.end_page = count_end;
    g_ends = 0;
    draw_host_reset_text();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    expect_text(2, "One", 400, 740);
    wp_document_free(&doc);

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    g_ends = 0;
    draw_host_reset_text();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(pages, 0);
    ASSERT_EQ_U32(g_ends, 0);
    ASSERT_EQ_U32(draw_host_text_count(), 0);
    wp_document_free(&doc);
}
