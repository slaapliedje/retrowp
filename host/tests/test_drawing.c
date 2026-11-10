/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "drawing.h"
#include "format_wmf.h"
#include "format_rtf.h"
#include "layout.h"

/* Drawings (docs/document-model.md, "Drawings"). */

/* ---- A sink that records what it was handed ------------------------- */

#define REC_MAX 16
typedef struct {
    int    kind;           /* 'L' line, 'F' fill */
    u16    n;
    i32    width;
    u8     rgb[3];
    i32    xy[2 * 80];
} rec_call;

static rec_call g_calls[REC_MAX];
static u32 g_ncalls;

static void rec(int kind, const i32 *xy, u16 n, i32 width, const u8 *rgb)
{
    rec_call *c;
    if (g_ncalls >= REC_MAX) return;
    c = &g_calls[g_ncalls++];
    c->kind = kind;
    c->n = n;
    c->width = width;
    memcpy(c->rgb, rgb, 3);
    memcpy(c->xy, xy, (size_t)(n > 80 ? 80 : n) * 2 * sizeof(i32));
}
static void rec_line(void *u, const i32 *xy, u16 n, i32 width, const u8 *rgb) { (void)u; rec('L', xy, n, width, rgb); }
static void rec_fill(void *u, const i32 *xy, u16 n, const u8 *rgb) { (void)u; rec('F', xy, n, 0, rgb); }

static const wp_drawing_sink g_sink = { NULL, rec_line, rec_fill };

static i32 iabs(i32 v) { return v < 0 ? -v : v; }

/* ---- Rendering ------------------------------------------------------- */

/* A rectangle in a 1000 x 500 drawing, drawn 200 x 100 at (10, 20): the
   brush fills its four corners, then the pen -- 50 units, so 10 pixels
   -- goes round them and back to the first. A line after a PEN that
   turns the pen off draws nothing. */
void test_drawing_rect_and_pen(void)
{
    wp_drawing_builder b;
    static const i16 r[4] = { 0, 0, 1000, 500 };
    static const i16 l[4] = { 0, 0, 1000, 500 };

    wp_drawing_builder_init(&b);
    wp_drawing_pen(&b, 1, 50, 255, 0, 0);
    wp_drawing_brush(&b, 1, 0, 0, 255);
    wp_drawing_shape(&b, (u8)WP_DRAW_RECT, r);
    wp_drawing_pen(&b, 0, 0, 0, 0, 0);
    wp_drawing_poly(&b, (u8)WP_DRAW_LINE, l, 2);
    ASSERT_TRUE(b.error == WP_OK);

    g_ncalls = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, 1000, 500, 10, 20, 200, 100, &g_sink) == WP_OK);
    ASSERT_EQ_U32(g_ncalls, 2);
    ASSERT_TRUE(g_calls[0].kind == 'F');
    ASSERT_EQ_U32(g_calls[0].n, 4);
    ASSERT_EQ_U32(g_calls[0].rgb[2], 255);
    ASSERT_EQ_U32((u32)g_calls[0].xy[0], 10);  ASSERT_EQ_U32((u32)g_calls[0].xy[1], 20);
    ASSERT_EQ_U32((u32)g_calls[0].xy[4], 210); ASSERT_EQ_U32((u32)g_calls[0].xy[5], 120);
    ASSERT_TRUE(g_calls[1].kind == 'L');
    ASSERT_EQ_U32(g_calls[1].n, 5);
    ASSERT_EQ_U32((u32)g_calls[1].width, 10);
    ASSERT_EQ_U32(g_calls[1].rgb[0], 255);
    ASSERT_EQ_U32((u32)g_calls[1].xy[8], 10);  ASSERT_EQ_U32((u32)g_calls[1].xy[9], 20);
    wp_drawing_builder_free(&b);
}

/* An ellipse 400 x 200 pixels: every point within a pixel or two of the
   curve, the first at three o'clock, enough of them to look round. The
   default pen and brush: one black pixel, solid white. */
void test_drawing_ellipse_points(void)
{
    wp_drawing_builder b;
    static const i16 e[4] = { 0, 0, 400, 200 };
    u32 i;

    wp_drawing_builder_init(&b);
    wp_drawing_shape(&b, (u8)WP_DRAW_ELLIPSE, e);
    g_ncalls = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, 400, 200, 0, 0, 400, 200, &g_sink) == WP_OK);
    ASSERT_EQ_U32(g_ncalls, 2);
    ASSERT_TRUE(g_calls[0].kind == 'F' && g_calls[0].rgb[0] == 255);
    ASSERT_TRUE(g_calls[1].kind == 'L' && g_calls[1].width == 1 && g_calls[1].rgb[0] == 0);
    ASSERT_TRUE(g_calls[0].n >= 32);
    ASSERT_EQ_U32((u32)g_calls[0].xy[0], 400);
    ASSERT_EQ_U32((u32)g_calls[0].xy[1], 100);
    for (i = 0; i < g_calls[0].n; i++) {
        /* (x-200)^2/200^2 + (y-100)^2/100^2 = 1, times 200^2 */
        i32 dx = g_calls[0].xy[i * 2] - 200, dy = g_calls[0].xy[i * 2 + 1] - 100;
        i32 err = dx * dx + 4 * dy * dy - 40000;
        ASSERT_TRUE(iabs(err) < 1600);   /* within ~2 pixels */
    }
    wp_drawing_builder_free(&b);
}

/* An arc from three o'clock to twelve, counter-clockwise as it looks:
   from the right-hand point up to the top one, all in that quarter. */
void test_drawing_arc_quarter(void)
{
    wp_drawing_builder b;
    static const i16 a[8] = { 0, 0, 200, 200, 300, 100, 100, -50 };
    u32 i, n;

    wp_drawing_builder_init(&b);
    wp_drawing_shape(&b, (u8)WP_DRAW_ARC, a);
    g_ncalls = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, 200, 200, 0, 0, 200, 200, &g_sink) == WP_OK);
    ASSERT_EQ_U32(g_ncalls, 1);
    ASSERT_TRUE(g_calls[0].kind == 'L');
    n = g_calls[0].n;
    ASSERT_TRUE(n >= 3);
    ASSERT_EQ_U32((u32)g_calls[0].xy[0], 200);
    ASSERT_EQ_U32((u32)g_calls[0].xy[1], 100);
    ASSERT_EQ_U32((u32)g_calls[0].xy[(n - 1) * 2], 100);
    ASSERT_EQ_U32((u32)g_calls[0].xy[(n - 1) * 2 + 1], 0);
    for (i = 0; i < n; i++) {
        ASSERT_TRUE(g_calls[0].xy[i * 2] >= 100 && g_calls[0].xy[i * 2 + 1] <= 100);
    }
    wp_drawing_builder_free(&b);
}

/* A list cut short draws what came before and says so. */
void test_drawing_truncated(void)
{
    wp_drawing_builder b;
    static const i16 r[4] = { 0, 0, 10, 10 };

    wp_drawing_builder_init(&b);
    wp_drawing_brush(&b, 0, 0, 0, 0);
    wp_drawing_shape(&b, (u8)WP_DRAW_RECT, r);
    wp_drawing_shape(&b, (u8)WP_DRAW_RECT, r);
    g_ncalls = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length - 3, 10, 10, 0, 0, 10, 10, &g_sink) == WP_BADFMT);
    ASSERT_EQ_U32(g_ncalls, 1);   /* the first rectangle's outline; no brush */
    wp_drawing_builder_free(&b);
}

/* ---- Metafiles ------------------------------------------------------- */

/* One of everything. */
static void build_all(wp_drawing_builder *b)
{
    static const i16 pts[6] = { 10, 10, 90, 10, 50, 80 };
    static const i16 rect[4] = { 5, 6, 95, 96 };
    static const i16 rr[6] = { 10, 10, 60, 40, 8, 6 };
    static const i16 arc[8] = { 0, 0, 100, 100, 100, 50, 50, 0 };

    wp_drawing_builder_init(b);
    wp_drawing_pen(b, 1, 3, 10, 20, 30);
    wp_drawing_brush(b, 1, 200, 100, 50);
    wp_drawing_poly(b, (u8)WP_DRAW_POLYGON, pts, 3);
    wp_drawing_poly(b, (u8)WP_DRAW_LINE, pts, 3);
    wp_drawing_shape(b, (u8)WP_DRAW_RECT, rect);
    wp_drawing_brush(b, 0, 0, 0, 0);
    wp_drawing_shape(b, (u8)WP_DRAW_RRECT, rr);
    wp_drawing_shape(b, (u8)WP_DRAW_ELLIPSE, rect);
    wp_drawing_pen(b, 0, 0, 0, 0, 0);
    wp_drawing_shape(b, (u8)WP_DRAW_ARC, arc);
}

typedef struct { u8 buf[1024]; u32 n; } membuf;
static void put_mem(void *u, const u8 *bytes, u32 n)
{
    membuf *m = (membuf *)u;
    if (m->n + n <= sizeof(m->buf)) memcpy(m->buf + m->n, bytes, n);
    m->n += n;
}

/* Out to a metafile and back: the same primitives, byte for byte, and
   the same extent; the measured length is the written one. */
void test_drawing_wmf_round_trip(void)
{
    wp_drawing_builder a, back;
    static membuf m;
    u32 measured = 0;
    i32 ew = 0, eh = 0;

    build_all(&a);
    ASSERT_TRUE(drawing_to_wmf(a.bytes, a.length, 100, 100, NULL, NULL, &measured) == WP_OK);
    m.n = 0;
    ASSERT_TRUE(drawing_to_wmf(a.bytes, a.length, 100, 100, put_mem, &m, NULL) == WP_OK);
    ASSERT_EQ_U32(m.n, measured);
    ASSERT_EQ_U32(m.buf[0], 1);                         /* in memory */
    ASSERT_EQ_U32((u32)m.buf[6] | ((u32)m.buf[7] << 8), m.n / 2);   /* size in words */

    wp_drawing_builder_init(&back);
    ASSERT_TRUE(wmf_to_drawing(m.buf, m.n, &back, &ew, &eh) == WP_OK);
    ASSERT_EQ_U32((u32)ew, 100);
    ASSERT_EQ_U32((u32)eh, 100);
    ASSERT_EQ_U32(back.length, a.length);
    if (back.length == a.length) ASSERT_MEM_EQ(back.bytes, a.bytes, a.length);
    wp_drawing_builder_free(&back);
    wp_drawing_builder_free(&a);
}

/* A metafile as another program writes one: a placeable header, a
   window origin of (100, 200), a font made before the pen (so the pen
   is object 1), a rectangle, and a path of MoveTo / LineTo / LineTo,
   which comes in as one three-point line. */
void test_drawing_wmf_foreign(void)
{
    static const u8 wmf[] = {
        0xD7, 0xCD, 0xC6, 0x9A,  0, 0,  100, 0, 200, 0, 0x4C, 0x04, 0xB0, 0x04,  0x40, 0x06,  0, 0, 0, 0,  0, 0,
        1, 0, 9, 0, 0, 3,  0, 0, 0, 0,  2, 0,  0, 0, 0, 0,  0, 0,
        5, 0, 0, 0, 0x0B, 0x02, 200, 0, 100, 0,                     /* SetWindowOrg y, x */
        5, 0, 0, 0, 0x0C, 0x02, 0xE8, 0x03, 0xE8, 0x03,             /* SetWindowExt 1000, 1000 */
        4, 0, 0, 0, 0xFB, 0x02, 0, 0,                                /* CreateFontIndirect: slot 0 */
        8, 0, 0, 0, 0xFA, 0x02, 0, 0, 7, 0, 0, 0, 0x11, 0x22, 0x33, 0,   /* CreatePen width 7: slot 1 */
        4, 0, 0, 0, 0x2D, 0x01, 1, 0,                                /* SelectObject 1 */
        7, 0, 0, 0, 0x1B, 0x04, 0xBC, 0x02, 0x58, 0x02, 200, 0, 100, 0,  /* Rectangle b700 r600 t200 l100 */
        5, 0, 0, 0, 0x14, 0x02, 200, 0, 100, 0,                     /* MoveTo (100, 200) */
        5, 0, 0, 0, 0x13, 0x02, 0x2C, 0x01, 200, 0,                 /* LineTo (200, 300) */
        5, 0, 0, 0, 0x13, 0x02, 200, 0, 0x2C, 0x01,                 /* LineTo (300, 200) */
        3, 0, 0, 0, 0, 0
    };
    static const u8 want[] = {
        WP_DRAW_PEN, 1, 0, 7, 0x11, 0x22, 0x33,
        WP_DRAW_RECT, 0, 0, 0, 0, 0x01, 0xF4, 0x01, 0xF4,
        WP_DRAW_LINE, 0, 3, 0, 0, 0, 0, 0, 100, 0, 100, 0, 200, 0, 0
    };
    wp_drawing_builder b;
    i32 ew = 0, eh = 0;

    wp_drawing_builder_init(&b);
    ASSERT_TRUE(wmf_to_drawing(wmf, (u32)sizeof(wmf), &b, &ew, &eh) == WP_OK);
    ASSERT_EQ_U32((u32)ew, 1000);
    ASSERT_EQ_U32((u32)eh, 1000);
    ASSERT_EQ_U32(b.length, (u32)sizeof(want));
    if (b.length == sizeof(want)) ASSERT_MEM_EQ(b.bytes, want, sizeof(want));
    wp_drawing_builder_free(&b);
}

/* ---- In a document --------------------------------------------------- */

/* A drawing paragraph after a line of text, saved as RTF and read back:
   a \pict\wmetafile8, the same primitives and box; laid out, one line
   the box's height. */
void test_drawing_rtf_round_trip(void)
{
    wp_drawing_builder a;
    wp_document doc, back;
    wp_image h;
    u32 len = 0, i;
    wp_section *sec;
    static u8 out[8192];
    const wp_image_desc *d;
    wp_paragraph_layout *l;

    build_all(&a);
    {
        static const char src[] = "{\\rtf1\\ansi Above\\par Below\\par}";
        ASSERT_TRUE(format_rtf_import((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    }
    ASSERT_TRUE(wp_document_drawing_add(&doc, a.bytes, a.length, 100, 100, 1440, 720, "logo", &h) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(sec->para_count >= 2);
    sec->paras[1].run_count = 0;   /* "Below" becomes the drawing */
    sec->paras[1].kind = (u8)WP_PARA_IMAGE;
    sec->paras[1].image = h;

    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    out[len] = '\0';
    ASSERT_TRUE(strstr((const char *)out, "{\\pict\\wmetafile8\\picw2540\\pich1270\\picwgoal1440\\pichgoal720") != NULL);

    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    sec = wp_document_section_get(&back, 0);
    ASSERT_TRUE(sec->para_count >= 2);
    for (i = 0; i < sec->para_count && sec->paras[i].kind != (u8)WP_PARA_IMAGE; i++) {}
    ASSERT_EQ_U32(i, 1);
    if (i < sec->para_count) {
        d = wp_document_image_get(&back, sec->paras[i].image);
        ASSERT_TRUE(d != NULL && WP_IMAGE_IS_DRAWING(d));
        if (d != NULL) {
            ASSERT_EQ_U32((u32)d->width_px, 100);
            ASSERT_EQ_U32((u32)d->display_width_twips, 1440);
            ASSERT_EQ_U32((u32)d->display_height_twips, 720);
            ASSERT_EQ_U32(d->drawing.length, a.length);
            if (d->drawing.length == a.length)
                ASSERT_MEM_EQ(wp_strpool_get(&back.strings, d->drawing), a.bytes, a.length);
        }
        ASSERT_TRUE(wp_paragraph_relayout(&back, &sec->paras[i], 9360) == WP_OK);
        l = (wp_paragraph_layout *)sec->paras[i].layout;
        ASSERT_TRUE(l != NULL && l->line_count == 1);
        if (l != NULL && l->line_count == 1) ASSERT_TRUE(l->lines[0].height_twips >= 720);
    }
    wp_document_free(&back);
    wp_document_free(&doc);
    wp_drawing_builder_free(&a);
}
