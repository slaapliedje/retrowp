/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "bitmapfile.h"

/* Bitmap pictures read on the machine (bitmapfile.h): one small file of
   each kind, built here, its pixels checked as they come out of the
   sink. */

static u8 g_px[65536];
static u32 g_used;
static i32 g_w, g_h;

static wp_status sk_begin(void *u, i32 w, i32 h, char *path, u32 cap, u32 *n)
{
    (void)u; (void)cap;
    g_w = w; g_h = h; g_used = 0;
    memcpy(path, "mem", 3);
    *n = 3;
    return WP_OK;
}
static wp_status sk_write(void *u, const u8 *b, u32 n)
{
    (void)u;
    if (g_used + n <= sizeof(g_px)) memcpy(g_px + g_used, b, n);
    g_used += n;
    return WP_OK;
}
static wp_status sk_end(void *u) { (void)u; return WP_OK; }

/* The colour at (x, y) of the picture last read. */
static const u8 *px(const wp_image_desc *d, i32 x, i32 y)
{
    u32 rb = ((u32)g_w + 1u) / 2u;
    u8 v = g_px[(u32)y * rb + (u32)x / 2u];
    return d->palette[(x & 1) ? (v & 15) : (v >> 4)];
}

static wp_status read_pic(const u8 *file, u32 len, const char *name, u32 colors, i32 mw, i32 mh,
                          wp_image_desc *d)
{
    wp_image_sink sk;
    wp_document doc;
    wp_status st;

    sk.begin = sk_begin; sk.write = sk_write; sk.end = sk_end; sk.user = NULL;
    wp_document_init(&doc);
    st = bitmapfile_read(file, len, name, colors, mw, mh, &sk, &doc, d);
    wp_document_free(&doc);
    return st;
}

#define RGB_IS(c, r, g, b) ((c)[0] == (r) && (c)[1] == (g) && (c)[2] == (b))

/* A GEM .IMG, mono, 16 x 4: a solid black row; a literal (F0 0F); then a
   pattern (AA 55) on a row repeated twice. */
void test_bitmapfile_img(void)
{
    static const u8 f[] = {
        0, 1, 0, 8, 0, 1, 0, 2, 0, 85, 0, 85, 0, 16, 0, 4,
        0x82,
        0x80, 2, 0xF0, 0x0F,
        0x00, 0x00, 0xFF, 0x02, 0x00, 0x01, 0xAA, 0x55
    };
    wp_image_desc d;

    ASSERT_TRUE(bitmapfile_detect(f, (u32)sizeof(f), "A.IMG"));
    ASSERT_TRUE(read_pic(f, (u32)sizeof(f), "A.IMG", 16, 640, 480, &d) == WP_OK);
    ASSERT_EQ_U32((u32)g_w, 16); ASSERT_EQ_U32((u32)g_h, 4);
    ASSERT_TRUE(RGB_IS(px(&d, 0, 0), 0, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 0, 1), 0, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 5, 1), 255, 255, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 12, 1), 0, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 0, 2), 0, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 1, 2), 255, 255, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 8, 3), 255, 255, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 9, 3), 0, 0, 0));
}

/* An IFF ILBM, 4 x 2, two planes, ByteRun1, palette white red green blue:
   row 0 the four colours in order, row 1 all blue. */
void test_bitmapfile_ilbm(void)
{
    static const u8 f[] = {
        'F','O','R','M', 0,0,0,70, 'I','L','B','M',
        'B','M','H','D', 0,0,0,20, 0,4, 0,2, 0,0, 0,0, 2, 0, 1, 0, 0,0, 10, 11, 0,4, 0,2,
        'C','M','A','P', 0,0,0,12, 255,255,255, 255,0,0, 0,255,0, 0,0,255,
        'B','O','D','Y', 0,0,0,10, 0x01,0x50,0x00, 0x01,0x30,0x00, 0xFF,0xF0, 0xFF,0xF0
    };
    wp_image_desc d;

    ASSERT_TRUE(read_pic(f, (u32)sizeof(f), NULL, 16, 640, 480, &d) == WP_OK);
    ASSERT_EQ_U32((u32)g_w, 4); ASSERT_EQ_U32((u32)g_h, 2);
    ASSERT_TRUE(RGB_IS(px(&d, 0, 0), 255, 255, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 1, 0), 255, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 2, 0), 0, 255, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 3, 0), 0, 0, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 0, 1), 0, 0, 255));
}

/* A PCX, 8 bits, 4 x 2: a run of four index 5, then 1 2 3 4; the
   256-colour palette at the end. */
void test_bitmapfile_pcx(void)
{
    static u8 f[128 + 6 + 769];
    wp_image_desc d;
    u8 *pal;

    memset(f, 0, sizeof(f));
    f[0] = 0x0A; f[1] = 5; f[2] = 1; f[3] = 8;
    f[8] = 3; f[10] = 1;           /* xmax 3, ymax 1 */
    f[65] = 1; f[66] = 4;          /* one plane, 4 bytes a line */
    f[128] = 0xC4; f[129] = 5;
    f[130] = 1; f[131] = 2; f[132] = 3; f[133] = 4;
    f[134] = 0x0C;
    pal = f + 135;
    pal[5 * 3] = 10; pal[5 * 3 + 1] = 20; pal[5 * 3 + 2] = 30;
    pal[1 * 3] = 255; pal[2 * 3 + 1] = 255; pal[3 * 3 + 2] = 255;
    pal[4 * 3] = pal[4 * 3 + 1] = pal[4 * 3 + 2] = 255;
    ASSERT_TRUE(read_pic(f, (u32)sizeof(f), NULL, 16, 640, 480, &d) == WP_OK);
    ASSERT_EQ_U32((u32)g_w, 4); ASSERT_EQ_U32((u32)g_h, 2);
    ASSERT_TRUE(RGB_IS(px(&d, 0, 0), 10, 20, 30));
    ASSERT_TRUE(RGB_IS(px(&d, 3, 0), 10, 20, 30));
    ASSERT_TRUE(RGB_IS(px(&d, 0, 1), 255, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 2, 1), 0, 0, 255));
}

/* A 24-bit BMP, 2 x 2, rows bottom-up and padded to 8 bytes: the top row
   blue, white; the bottom red, green. */
void test_bitmapfile_bmp(void)
{
    static u8 f[54 + 16];
    wp_image_desc d;

    memset(f, 0, sizeof(f));
    f[0] = 'B'; f[1] = 'M'; f[10] = 54; f[14] = 40;
    f[18] = 2; f[22] = 2; f[26] = 1; f[28] = 24;
    /* bottom row: red, green (BGR) */
    f[54 + 2] = 255; f[54 + 4] = 255;
    /* top row: blue, white */
    f[62] = 255; f[65] = 255; f[66] = 255; f[67] = 255;
    ASSERT_TRUE(read_pic(f, (u32)sizeof(f), NULL, 16, 640, 480, &d) == WP_OK);
    ASSERT_TRUE(RGB_IS(px(&d, 0, 0), 0, 0, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 1, 0), 255, 255, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 0, 1), 255, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 1, 1), 0, 255, 0));
}

/* MacPaint in a MacBinary wrapper: 576 x 720, a black first row and the
   rest white. Fitted to 320 x 200 it is 160 x 200, and the first output
   row -- standing for source rows 0..3 -- keeps the line: black. */
void test_bitmapfile_macpaint(void)
{
    static u8 f[128 + 512 + 720 * 2];
    wp_image_desc d;
    u32 i;

    memset(f, 0, sizeof(f));
    f[1] = 4; memcpy(f + 2, "TEST", 4); memcpy(f + 65, "PNTG", 4);
    for (i = 0; i < 720; i++) {
        f[640 + i * 2] = 0xB9;             /* -71: the next byte 72 times */
        f[640 + i * 2 + 1] = (u8)(i == 0 ? 0xFF : 0x00);
    }
    ASSERT_TRUE(bitmapfile_detect(f, (u32)sizeof(f), "TEST"));
    ASSERT_TRUE(read_pic(f, (u32)sizeof(f), "TEST", 2, 320, 200, &d) == WP_OK);
    ASSERT_EQ_U32((u32)g_w, 160); ASSERT_EQ_U32((u32)g_h, 200);
    ASSERT_TRUE(RGB_IS(px(&d, 40, 0), 0, 0, 0));
    ASSERT_TRUE(RGB_IS(px(&d, 40, 1), 255, 255, 255));
    ASSERT_TRUE(RGB_IS(px(&d, 40, 199), 255, 255, 255));
}

/* Not a bitmap: text, a PNG's start. */
void test_bitmapfile_detect(void)
{
    static const u8 txt[] = "Plain words, nothing more, and certainly not a picture of anything.";
    static const u8 png[16] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    ASSERT_TRUE(!bitmapfile_detect(txt, (u32)sizeof(txt), "NOTES.TXT"));
    ASSERT_TRUE(!bitmapfile_detect(png, (u32)sizeof(png), "PIC.PNG"));
}
