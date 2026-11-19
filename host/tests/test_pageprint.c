/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "pageprint.h"
#include "format_txt.h"
#include "strpool.h"
#include "draw_host.h"
#include "psdev.h"
#include "pageview.h"
#include "metrics_host.h"

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

    make_doc(&doc);                  /* format_txt_import initialises doc */
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

    make_doc(&doc);                  /* format_txt_import initialises doc */
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

/* A picture prints its pixels: 2x2, a black column and a white one, in
   a box 300 twips square -- 30 device pixels at a tenth of a twip each.
   One band of 30 rows goes to plat_draw_mono, and each row's black is
   one run (the host draws a run as one fill): 30 black fills, and none
   of the white column, which the dither never darkens. Unreadable
   pixels print the frame instead. */
static i32 tenth(i32 twips) { return twips / 10; }

/* A one-picture document: a 2x2 picture, black over white columns,
   shown 300 twips square, its pixels in the file at path. */
static void make_picture_doc(wp_document *doc, const char *path)
{
    static const u8 pixels[2] = { 0x01, 0x01 };   /* nibbles: black, white */
    wp_image_desc desc;
    wp_image handle;
    wp_section *sec;
    wp_paragraph *para;
    plat_file *f;
    u32 si, pi;

    ASSERT_TRUE(plat_file_open(path, PLAT_FILE_WRITE, &f) == WP_OK);
    ASSERT_TRUE(plat_file_write(f, pixels, 2) == WP_OK);
    plat_file_close(f);

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &si) == WP_OK);
    sec = wp_document_section_get(doc, si);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &pi) == WP_OK);
    memset(&desc, 0, sizeof(desc));
    desc.width_px = 2;
    desc.height_px = 2;
    desc.display_width_twips = 300;
    desc.display_height_twips = 300;
    desc.palette_count = 2;
    desc.palette[1][0] = desc.palette[1][1] = desc.palette[1][2] = 255;
    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)path, (u32)strlen(path),
                                  &desc.blob_path) == WP_OK);
    ASSERT_TRUE(wp_document_image_add(doc, &desc, &handle) == WP_OK);
    para = wp_section_paragraph_get(sec, pi);
    para->kind = (u8)WP_PARA_IMAGE;
    para->image = handle;
    para->style = doc->default_pstyle;
}

void test_pageprint_picture(void)
{
    static const char path[] = "build/test_pageprint_pic.bin";
    wp_document doc;
    wp_page_device dev;
    u32 pages = 0;

    make_picture_doc(&doc, path);
    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = tenth;
    dev.y_px = tenth;
    draw_host_reset_fills();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(pages, 1);
    ASSERT_EQ_U32(draw_host_mono_calls(), 1);
    ASSERT_EQ_U32(draw_host_grey_fills(0), 30);

    (void)remove(path);
    draw_host_reset_fills();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(draw_host_mono_calls(), 0);
    ASSERT_EQ_U32(draw_host_grey_fills(0), 4);   /* the frame's four sides */
    wp_document_free(&doc);
}

typedef struct { char buf[32768]; u32 n; } ps_sink;
static ps_sink g_sink;
static wp_status sink_put(void *user, const u8 *b, u32 n)
{
    ps_sink *o = (ps_sink *)user;
    if (o->n + n < sizeof(o->buf)) { memcpy(o->buf + o->n, b, n); o->n += n; }
    return WP_OK;
}

/* To PostScript (psdev.h) the same picture prints as imagemask bands of
   its dithered pixels -- 300 twips is 150 tenths of a point, so five
   bands of up to 32 rows -- and nothing reaches the platform: the page
   is the device's own. Without its pixels, the frame as rectangles. */
void test_pageprint_picture_postscript(void)
{
    static const char path[] = "build/test_pageprint_pic_ps.bin";
    wp_document doc;
    u32 pages = 0, bands = 0;
    const char *p;

    make_picture_doc(&doc, path);
    draw_host_reset_fills();
    g_sink.n = 0;
    ASSERT_TRUE(wp_ps_print(&doc, sink_put, &g_sink, 0, &pages) == WP_OK);
    g_sink.buf[g_sink.n] = '\0';
    ASSERT_EQ_U32(pages, 1);
    for (p = g_sink.buf; (p = strstr(p, "imagemask")) != NULL; p++) bands++;
    ASSERT_EQ_U32(bands, 5);
    ASSERT_TRUE(strstr(g_sink.buf, "150 32 true [150 0 0 32 0 0]") != NULL);
    ASSERT_EQ_U32(draw_host_mono_calls(), 0);
    ASSERT_EQ_U32(draw_host_grey_fills(0), 0);

    (void)remove(path);
    g_sink.n = 0;
    ASSERT_TRUE(wp_ps_print(&doc, sink_put, &g_sink, 2, &pages) == WP_OK);
    g_sink.buf[g_sink.n] = '\0';
    ASSERT_TRUE(strstr(g_sink.buf, "imagemask") == NULL);
    ASSERT_TRUE(strstr(g_sink.buf, "true DPX") != NULL);   /* two-sided, short edge */
    ASSERT_TRUE(strstr(g_sink.buf, " R\n") != NULL);       /* the frame */
    ASSERT_EQ_U32(draw_host_grey_fills(0), 0);
    wp_document_free(&doc);
}

/* Page view at a zoom the screen has no face for -- asked 7.2 pt, the
   smallest face 8 pt, as the ST's bitmap fonts stop -- keeps each line's
   baseline where the layout put it (its own face's ascent below the
   text's top), not the bigger face's ascent lower down, which hung a
   table's labels low in their rows. */
void test_pageprint_page_baseline_at_zoom(void)
{
    wp_page_device dev;
    wp_document doc;
    u32 i, n;
    i32 gx = 0, gy = -1;

    make_doc(&doc);
    wp_document_section_get(&doc, 0)->header.length = 0;
    (void)wp_pageview_relayout(&doc, 0);
    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = same;
    dev.y_px = same;
    metrics_host_set_min_size(160);
    draw_host_reset_text();
    ASSERT_TRUE(wp_pageprint_page(&doc, &dev, 0, 0, 0, 60, -32000, 32000) == WP_OK);
    metrics_host_set_min_size(0);
    n = draw_host_text_count();
    for (i = 0; i < n; i++) {
        const char *t = draw_host_text_at(i, &gx, &gy);
        if (t != NULL && strcmp(t, "One") == 0) break;
    }
    ASSERT_TRUE(i < n);
    ASSERT_EQ_U32((u32)gy, (u32)(((600 + 240) * 60) / 100));   /* 504, not the 8 pt face's 519 */
    wp_document_free(&doc);
}
