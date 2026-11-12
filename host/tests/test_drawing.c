/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "drawing.h"
#include "format_wmf.h"
#include "drawfile.h"
#include "format_prn.h"
#include "format_rtf.h"
#include "layout.h"
#include "pageprint.h"
#include "draw_host.h"

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

/* The last TEXT handed over. */
static struct {
    i32 x, y, height;
    u16 angle;
    u8 align, style, family, rgb[3];
    char text[64];
    u32 count;
} g_text;
static void rec_text(void *u, i32 x, i32 y, i32 height, u16 angle, u8 align, u8 style, u8 family,
                     const u8 *rgb, const u8 *utf8, u16 len)
{
    (void)u;
    g_text.x = x; g_text.y = y; g_text.height = height; g_text.angle = angle;
    g_text.align = align; g_text.style = style; g_text.family = family;
    memcpy(g_text.rgb, rgb, 3);
    if (len > 63) len = 63;
    memcpy(g_text.text, utf8, len);
    g_text.text[len] = '\0';
    g_text.count++;
}

static const wp_drawing_sink g_sink = { NULL, rec_line, rec_fill, rec_text };

static i32 iabs(i32 v) { return v < 0 ? -v : v; }

/* ---- Rendering ------------------------------------------------------- */

/* A rectangle in a 1000 x 500 drawing, drawn 200 x 100 at (10, 20): the
   brush fills its four corners, then the pen -- 50 units, 10 pixels --
   goes round as four quadrilaterals and an octagon at each of its five
   points (the last back at the first), red. The top edge's runs 5 each
   side of y 20. A line after a PEN that turns the pen off draws
   nothing. */
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
    ASSERT_EQ_U32(g_ncalls, 1 + 4 + 5);
    ASSERT_TRUE(g_calls[0].kind == 'F');
    ASSERT_EQ_U32(g_calls[0].n, 4);
    ASSERT_EQ_U32(g_calls[0].rgb[2], 255);
    ASSERT_EQ_U32((u32)g_calls[0].xy[0], 10);  ASSERT_EQ_U32((u32)g_calls[0].xy[1], 20);
    ASSERT_EQ_U32((u32)g_calls[0].xy[4], 210); ASSERT_EQ_U32((u32)g_calls[0].xy[5], 120);
    ASSERT_TRUE(g_calls[1].kind == 'F');
    ASSERT_EQ_U32(g_calls[1].n, 4);
    ASSERT_EQ_U32(g_calls[1].rgb[0], 255);
    ASSERT_EQ_U32((u32)g_calls[1].xy[1], 25);  ASSERT_EQ_U32((u32)g_calls[1].xy[5], 15);
    ASSERT_EQ_U32(g_calls[5].n, 8);            /* the first octagon */
    wp_drawing_builder_free(&b);
}

/* A pen 50 units wide in a 1000 x 500 drawing drawn 200 x 200: 10
   pixels across but 20 down, as on a screen whose pixels are half as
   tall as wide. A horizontal line's stroke is 20 pixels thick, a
   vertical one's 10 -- the same thickness on the glass. */
void test_drawing_pen_aspect(void)
{
    wp_drawing_builder b;
    static const i16 h[4] = { 100, 250, 900, 250 };
    static const i16 v[4] = { 500, 50, 500, 450 };

    wp_drawing_builder_init(&b);
    wp_drawing_pen(&b, 1, 50, 0, 0, 0);
    wp_drawing_poly(&b, (u8)WP_DRAW_LINE, h, 2);
    wp_drawing_poly(&b, (u8)WP_DRAW_LINE, v, 2);
    g_ncalls = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, 1000, 500, 0, 0, 200, 200, &g_sink) == WP_OK);
    ASSERT_EQ_U32(g_ncalls, 6);   /* each: a quadrilateral, two octagons */
    /* The horizontal one, y 100: corners at 110 and 90. */
    ASSERT_EQ_U32((u32)g_calls[0].xy[1], 110);
    ASSERT_EQ_U32((u32)g_calls[0].xy[5], 90);
    /* The vertical one, x 100: corners at 95 and 105. */
    ASSERT_EQ_U32((u32)g_calls[3].xy[0], 95);
    ASSERT_EQ_U32((u32)g_calls[3].xy[4], 105);
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

/* ---- Printed --------------------------------------------------------- */

static i32 px_same(i32 t) { return t; }
static wp_status page_done(void *user) { (void)user; return WP_OK; }

/* The drawing as the first paragraph, 1440 x 720 twips, printed with
   pixels = twips and inch margins: everything inside its box at (1440,
   1440). build_all's arc has its pen off and draws nothing; the rest
   span 5..95 across (1512..2808) and 6..96 down (1483..2131). The pen
   is 3 units of 100: 43 pixels across, 22 down (the box is stretched),
   so the strokes -- polygons, every one -- reach 22 further out each
   side and 11 up and down. */
void test_drawing_pageprint(void)
{
    wp_drawing_builder a;
    wp_document doc;
    wp_image h;
    wp_section *sec;
    wp_page_device dev;
    u32 pages = 0;
    i32 x0, y0, x1, y1;
    static const char src[] = "{\\rtf1\\ansi X\\par}";

    build_all(&a);
    ASSERT_TRUE(format_rtf_import((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_TRUE(wp_document_drawing_add(&doc, a.bytes, a.length, 100, 100, 1440, 720, NULL, &h) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->paras[0].run_count = 0;
    sec->paras[0].kind = (u8)WP_PARA_IMAGE;
    sec->paras[0].image = h;
    sec->paras[0].layout_dirty = WP_TRUE;

    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = px_same;
    dev.y_px = px_same;
    dev.end_page = page_done;
    draw_host_reset_fills();
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(pages, 1);
    ASSERT_EQ_U32(draw_host_polyline_calls(), 0);
    ASSERT_TRUE(draw_host_vector_box(&x0, &y0, &x1, &y1));
    ASSERT_EQ_U32((u32)x0, 1512 - 22);
    ASSERT_EQ_U32((u32)y0, 1483 - 11);
    ASSERT_EQ_U32((u32)x1, 2808 + 22);
    ASSERT_EQ_U32((u32)y1, 2131 + 11);
    wp_document_free(&doc);
    wp_drawing_builder_free(&a);
}

/* ---- retroplat's helpers (src/polyfill.c) ---------------------------- */

/* A diamond 100 rows tall: black, a span per row through plat_fill_rect;
   shaded 37% (a mid grey's darkness), a row each through plat_fill_shade.
   A colour is not a grey, but has a darkness by its brightness: orange
   (200, 100, 40) is 52%, which a two-colour device shades it at. */
void test_polyfill_spans(void)
{
    static const i32 d[8] = { 50, 0, 100, 50, 50, 100, 0, 50 };

    ASSERT_EQ_U32(plat_grey_percent(160, 160, 160), 37);
    ASSERT_EQ_U32(plat_grey_percent(250, 250, 250), 0);
    ASSERT_EQ_U32(plat_grey_percent(200, 100, 100), 0);
    ASSERT_EQ_U32(plat_luma_percent(200, 100, 40), 52);
    ASSERT_EQ_U32(plat_luma_percent(250, 250, 250), 0);
    draw_host_reset_fills();
    ASSERT_TRUE(plat_polygon_by_spans(NULL, d, 4, 0, 0, 0, 0) == WP_OK);
    ASSERT_EQ_U32(draw_host_grey_fills(0), 100);
    ASSERT_EQ_U32(draw_host_shade_calls(), 0);
    draw_host_reset_fills();
    ASSERT_TRUE(plat_polygon_by_spans(NULL, d, 4, 160, 160, 160, plat_grey_percent(160, 160, 160)) == WP_OK);
    ASSERT_EQ_U32(draw_host_shade_calls(), 100);
    ASSERT_EQ_U32(draw_host_last_shade(), 37);
    ASSERT_EQ_U32(draw_host_grey_fills(160), 0);
}

/* A line 4 pixels wide from (0, 10) to (100, 10): a quadrilateral 2 each
   side, and an octagon of radius 2 at each end -- three polygons, inside
   x -2..102, y 8..12. */
void test_polyfill_wide_line(void)
{
    static const i32 l[4] = { 0, 10, 100, 10 };
    i32 x0, y0, x1, y1;

    draw_host_reset_fills();
    ASSERT_TRUE(plat_polyline_by_polygons(NULL, l, 2, 4, 0, 0, 0) == WP_OK);
    ASSERT_EQ_U32(draw_host_polygon_calls(), 3);
    ASSERT_TRUE(draw_host_vector_box(&x0, &y0, &x1, &y1));
    ASSERT_EQ_U32((u32)x0, (u32)-2);
    ASSERT_EQ_U32((u32)y0, 8);
    ASSERT_EQ_U32((u32)x1, 102);
    ASSERT_EQ_U32((u32)y1, 12);
}

/* ---- Drawing files (drawfile.h) -------------------------------------- */

/* A GEM metafile, written by hand: a coordinate system 1000 units each
   way, y down, on a page 254 mm (10 inches) square; extents 1000 x 500.
   Records: NVDI's RGB for the fill (red), solid interior, perimeter off,
   a circle radius 100 at (500, 250); a line (0, 0)-(1000, 500); a
   Bezier fill of one cubic (0,500) (0,0) (1000,0) (1000,500). */
static const u16 g_gem_words[] = {
    0xFFFF, 24, 101, 2, 0, 0, 1000, 500, 2540, 2540, 0, 1000, 1000, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    138, 0, 4, 1, 1000, 0, 0, 0,
    23, 0, 1, 0, 1,
    104, 0, 1, 0, 0,
    11, 3, 0, 4, 500, 250, 0, 0, 100, 0,
    6, 2, 0, 0, 0, 0, 1000, 500,
    9, 4, 2, 13, 0, 500, 0, 0, 1000, 0, 1000, 500, 0x0001, 0x0000,
    0xFFFF
};

/* The words little-endian (the flag bytes, the 0x0001 word, become 01 00:
   point 0 starts the cubic), or big-endian with the flag words kept in
   file order. */
static u32 make_gem(u8 *out, wp_bool le)
{
    u32 i, n = (u32)(sizeof(g_gem_words) / sizeof(g_gem_words[0]));
    for (i = 0; i < n; i++) {
        u16 v = g_gem_words[i];
        wp_bool flagword = (wp_bool)(i == n - 3 || i == n - 2);
        if (le || flagword) { out[i * 2] = (u8)(v & 0xFF); out[i * 2 + 1] = (u8)(v >> 8); }
        else { out[i * 2] = (u8)(v >> 8); out[i * 2 + 1] = (u8)(v & 0xFF); }
    }
    return n * 2u;
}

static i32 bd16(const u8 *p) { return (i32)(i16)(((u16)p[0] << 8) | p[1]); }

void test_drawfile_gem(void)
{
    static u8 file[256];
    wp_drawing_builder b;
    wp_drawfile_info info;
    u32 len, pos;
    int pass;

    for (pass = 0; pass < 2; pass++) {
        len = make_gem(file, (wp_bool)(pass == 0));
        ASSERT_TRUE(drawfile_detect(file, len));
        wp_drawing_builder_init(&b);
        ASSERT_TRUE(drawfile_read(file, len, &b, &info) == WP_OK);
        ASSERT_EQ_U32((u32)info.ext_w, 1000);
        ASSERT_EQ_U32((u32)info.ext_h, 500);
        ASSERT_EQ_U32((u32)info.width_twips, 14400);
        ASSERT_EQ_U32((u32)info.height_twips, 7200);
        /* BRUSH red; PEN off (perimeter); ELLIPSE (400,150)-(600,350) */
        pos = 0;
        ASSERT_TRUE(b.length > 60);
        if (b.length <= 60) { wp_drawing_builder_free(&b); return; }
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_BRUSH);
        ASSERT_EQ_U32(b.bytes[pos + 2], 255); ASSERT_EQ_U32(b.bytes[pos + 3], 0);
        pos += 5;
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_PEN); ASSERT_EQ_U32(b.bytes[pos + 1], 0);
        pos += 7;
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_ELLIPSE);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 1), 400);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 3), 150);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 5), 600);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 7), 350);
        pos += 9;
        /* PEN on, black hairline; LINE (0,0)-(1000,500) */
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_PEN); ASSERT_EQ_U32(b.bytes[pos + 1], 1);
        pos += 7;
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_LINE);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 7), 1000);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 9), 500);
        pos += 11;
        /* PEN off again; the cubic as a POLYGON of 9, its middle (500,125) */
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_PEN); ASSERT_EQ_U32(b.bytes[pos + 1], 0);
        pos += 7;
        ASSERT_EQ_U32(b.bytes[pos], WP_DRAW_POLYGON);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 1), 9);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 3 + 4 * 4), 500);
        ASSERT_EQ_U32((u32)bd16(b.bytes + pos + 5 + 4 * 4), 125);
        ASSERT_EQ_U32(pos + 3 + 9 * 4, b.length);
        wp_drawing_builder_free(&b);
    }
}

/* Not a drawing: text, a too-short file, a bitmap's header. */
void test_drawfile_detect(void)
{
    static const u8 txt[] = "Hello, this is plain text and not a drawing at all.";
    static const u8 bmp[32] = { 'B', 'M' };
    wp_drawing_builder b;
    wp_drawfile_info info;

    ASSERT_TRUE(!drawfile_detect(txt, (u32)sizeof(txt)));
    ASSERT_TRUE(!drawfile_detect(bmp, (u32)sizeof(bmp)));
    ASSERT_TRUE(!drawfile_detect((const u8 *)"\xFF\xFF", 2));
    wp_drawing_builder_init(&b);
    ASSERT_TRUE(drawfile_read(txt, (u32)sizeof(txt), &b, &info) == WP_UNSUPPORTED);
    wp_drawing_builder_free(&b);
}

/* No coordinate system in the header: NDC, (0, 0) at the lower left and
   y going up. Extents (0,0)-(1000,1000); a line from the lower left to
   the upper right comes in from (0, 1000) to (1000, 0), y going down. */
void test_drawfile_gem_ndc(void)
{
    static const u16 w[] = {
        0xFFFF, 14, 101, 0, 0, 0, 1000, 1000, 0, 0, 0, 0, 0, 0,
        6, 2, 0, 0, 0, 0, 1000, 1000,
        0xFFFF
    };
    u8 file[64];
    u32 i, n = (u32)(sizeof(w) / sizeof(w[0]));
    wp_drawing_builder b;
    wp_drawfile_info info;

    for (i = 0; i < n; i++) { file[i * 2] = (u8)(w[i] & 0xFF); file[i * 2 + 1] = (u8)(w[i] >> 8); }
    wp_drawing_builder_init(&b);
    ASSERT_TRUE(drawfile_read(file, n * 2u, &b, &info) == WP_OK);
    ASSERT_EQ_U32((u32)info.ext_h, 1000);
    ASSERT_EQ_U32((u32)info.width_twips, 0);   /* no page size: the file does not say */
    /* PEN, then LINE: n at +8, points from +10 */
    ASSERT_TRUE(b.length == 7u + 3u + 8u);
    if (b.length == 18u) {
        ASSERT_EQ_U32(b.bytes[7], WP_DRAW_LINE);
        ASSERT_EQ_U32((u32)bd16(b.bytes + 12), 1000);
        ASSERT_EQ_U32((u32)bd16(b.bytes + 16), 0);
    }
    wp_drawing_builder_free(&b);
}

/* A coordinate system one line high -- (0, 0) to (181, 0), no extents --
   is a rule, as Atari Works draws its signature lines: read as a
   drawing one unit tall, not refused for having no height. */
void test_drawfile_gem_line_high(void)
{
    static const u16 w[] = {
        0xFFFF, 14, 101, 2, 0, 0, 0, 0, 504, 2, 0, 0, 181, 0,
        6, 2, 0, 0, 0, 0, 181, 0,
        0xFFFF
    };
    u8 file[64];
    u32 i, n = (u32)(sizeof(w) / sizeof(w[0]));
    wp_drawing_builder b;
    wp_drawfile_info info;

    for (i = 0; i < n; i++) { file[i * 2] = (u8)(w[i] & 0xFF); file[i * 2 + 1] = (u8)(w[i] >> 8); }
    wp_drawing_builder_init(&b);
    ASSERT_TRUE(drawfile_read(file, n * 2u, &b, &info) == WP_OK);
    ASSERT_EQ_U32((u32)info.ext_w, 181);
    ASSERT_EQ_U32((u32)info.ext_h, 1);
    wp_drawing_builder_free(&b);
}

/* ---- Text ------------------------------------------------------------ */

/* A TEXT in a 1000 x 500 drawing drawn 200 x 100 at (10, 20): its point
   and height scaled as the rest, the rest handed over as it was. */
void test_drawing_text_renders(void)
{
    wp_drawing_builder b;
    static const u8 hi[] = "Caf\xc3\xa9";

    wp_drawing_builder_init(&b);
    wp_drawing_text(&b, 500, 250, 100, 900, WP_DRAW_TEXT_CENTER, 1, 2, 10, 20, 30, hi, 5);
    ASSERT_TRUE(b.error == WP_OK);
    memset(&g_text, 0, sizeof(g_text));
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, 1000, 500, 10, 20, 200, 100, &g_sink) == WP_OK);
    ASSERT_EQ_U32(g_text.count, 1);
    ASSERT_EQ_U32((u32)g_text.x, 110);
    ASSERT_EQ_U32((u32)g_text.y, 70);
    ASSERT_EQ_U32((u32)g_text.height, 20);
    ASSERT_EQ_U32(g_text.angle, 900);
    ASSERT_EQ_U32(g_text.align, WP_DRAW_TEXT_CENTER);
    ASSERT_EQ_U32(g_text.style, 1);
    ASSERT_EQ_U32(g_text.family, 2);
    ASSERT_EQ_U32(g_text.rgb[2], 30);
    ASSERT_EQ_STR(g_text.text, "Caf\xc3\xa9");
    /* Cut short: refused, nothing handed over. */
    g_text.count = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length - 1, 1000, 500, 10, 20, 200, 100, &g_sink) == WP_BADFMT);
    ASSERT_EQ_U32(g_text.count, 0);
    wp_drawing_builder_free(&b);
}

/* Through a Windows metafile and back -- a font made, selected and let
   go round a TextOut -- the same TEXT record: what an RTF save keeps. */
void test_drawing_text_wmf_round_trip(void)
{
    wp_drawing_builder a, back;
    static membuf m;
    i32 ew = 0, eh = 0;
    static const u8 t[] = "Fax: 241 8844";

    wp_drawing_builder_init(&a);
    wp_drawing_text(&a, 40, 300, 120, 2700, WP_DRAW_TEXT_RIGHT | WP_DRAW_TEXT_TOP, 1 | 4, 1,
                    200, 0, 0, t, (u16)(sizeof(t) - 1));
    wp_drawing_text(&a, 5, 6, 48, 0, 0, 2, 3, 0, 0, 0, (const u8 *)"x", 1);
    m.n = 0;
    ASSERT_TRUE(drawing_to_wmf(a.bytes, a.length, 1000, 800, put_mem, &m, NULL) == WP_OK);
    wp_drawing_builder_init(&back);
    ASSERT_TRUE(wmf_to_drawing(m.buf, m.n, &back, &ew, &eh) == WP_OK);
    ASSERT_EQ_U32(back.length, a.length);
    if (back.length == a.length) ASSERT_MEM_EQ(back.bytes, a.bytes, a.length);
    wp_drawing_builder_free(&back);
    wp_drawing_builder_free(&a);
}

/* GEM text: vst_point in the page the header gives, vst_rotation,
   vst_effects (bold, underlined), vst_color, vst_alignment (centred,
   top), then v_gtext's characters in the Atari set. A 254 mm page of
   2540 units: a millimetre is 10 units, 18 pt is 63.5 mm / 10 = 63. */
void test_drawfile_gem_text(void)
{
    static const u16 w[] = {
        0xFFFF, 14, 101, 2, 0, 0, 2540, 2540, 2540, 2540, 0, 0, 2540, 2540,
        107, 0, 1, 0, 18,            /* vst_point 18 */
        13, 0, 1, 0, 900,            /* vst_rotation 90 */
        106, 0, 1, 0, 9,             /* vst_effects bold, underlined */
        22, 0, 1, 0, 2,              /* vst_color red */
        39, 0, 2, 0, 1, 5,           /* vst_alignment centre, top */
        8, 1, 3, 0, 100, 2440, 'C', 'a', 0x82,   /* v_gtext at (100, 100 from the top): "Ca" e-acute */
        0xFFFF
    };
    u8 file[128];
    u32 i, n = (u32)(sizeof(w) / sizeof(w[0]));
    wp_drawing_builder b;
    wp_drawfile_info info;

    for (i = 0; i < n; i++) { file[i * 2] = (u8)(w[i] & 0xFF); file[i * 2 + 1] = (u8)(w[i] >> 8); }
    wp_drawing_builder_init(&b);
    ASSERT_TRUE(drawfile_read(file, n * 2u, &b, &info) == WP_OK);
    memset(&g_text, 0, sizeof(g_text));
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, info.ext_w, info.ext_h, 0, 0,
                                  info.ext_w, info.ext_h, &g_sink) == WP_OK);
    ASSERT_EQ_U32(g_text.count, 1);
    ASSERT_EQ_STR(g_text.text, "Ca\xc3\xa9");
    ASSERT_EQ_U32((u32)g_text.x, 100);
    ASSERT_EQ_U32((u32)g_text.y, 100);
    ASSERT_EQ_U32((u32)g_text.height, 63);
    ASSERT_EQ_U32(g_text.angle, 900);
    ASSERT_EQ_U32(g_text.style, 1 | 4);
    ASSERT_EQ_U32(g_text.align, WP_DRAW_TEXT_CENTER | WP_DRAW_TEXT_TOP);
    ASSERT_EQ_U32(g_text.rgb[0], 255);
    wp_drawing_builder_free(&b);
}

/* ---- PostScript ------------------------------------------------------ */

typedef struct { char buf[131072]; u32 n; } psbuf;
static wp_status ps_put(void *user, const u8 *bytes, u32 n)
{
    psbuf *p = (psbuf *)user;
    if (p->n + n < sizeof(p->buf)) { memcpy(p->buf + p->n, bytes, n); p->n += n; }
    return WP_OK;
}

/* A drawing printed to PostScript: its paths in a gsave, the box's left
   edge at the inch margin (72 pt), flipped to tenths of a point down
   from its top, then filled polygons and stroked lines in their colours;
   the text after it follows it. Other drivers print "[Picture]". */
void test_drawing_postscript(void)
{
    wp_drawing_builder a;
    wp_document doc;
    wp_image h;
    wp_section *sec;
    static psbuf out;
    wp_prn_options opt;
    u32 pages = 0;
    static const char src[] = "{\\rtf1\\ansi Above\\par X\\par After\\par}";
    char *g, *after;

    build_all(&a);
    ASSERT_TRUE(format_rtf_import((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_TRUE(wp_document_drawing_add(&doc, a.bytes, a.length, 100, 100, 1440, 720, NULL, &h) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->paras[1].run_count = 0;
    sec->paras[1].kind = (u8)WP_PARA_IMAGE;
    sec->paras[1].image = h;

    memset(&opt, 0, sizeof(opt));
    opt.driver = WP_PRN_POSTSCRIPT;
    out.n = 0;
    ASSERT_TRUE(format_prn_export(&doc, &opt, ps_put, &out, &pages) == WP_OK);
    out.buf[out.n] = '\0';
    g = strstr(out.buf, "gsave 72.0 ");
    ASSERT_TRUE(g != NULL);
    ASSERT_TRUE(strstr(out.buf, "translate 0.1 -0.1 scale") != NULL);
    ASSERT_TRUE(strstr(out.buf, "200 255 div 100 255 div 50 255 div setrgbcolor") != NULL);   /* the brush */
    ASSERT_TRUE(strstr(out.buf, "closepath eofill") != NULL);
    ASSERT_TRUE(strstr(out.buf, "grestore") != NULL);
    ASSERT_TRUE(strstr(out.buf, "[Picture]") == NULL);
    after = strstr(out.buf, "(After)");
    ASSERT_TRUE(after != NULL && g != NULL && after > g);

    opt.driver = WP_PRN_PLAIN;
    out.n = 0;
    ASSERT_TRUE(format_prn_export(&doc, &opt, ps_put, &out, &pages) == WP_OK);
    out.buf[out.n] = '\0';
    ASSERT_TRUE(strstr(out.buf, "[Picture]") != NULL);
    wp_document_free(&doc);
    wp_drawing_builder_free(&a);
}

/* A pen 3 pixels across but 1 down (a 1000 x 500 drawing drawn 300 x 50):
   its polygons would be a row thin, which a VDI's fill can leave out, so
   the stroke carries a one-pixel centre line too -- never lost. */
void test_drawing_thin_pen_keeps_a_line(void)
{
    wp_drawing_builder b;
    static const i16 l[4] = { 100, 250, 900, 250 };
    u32 i, lines = 0, fills = 0;

    wp_drawing_builder_init(&b);
    wp_drawing_pen(&b, 1, 10, 0, 0, 0);
    wp_drawing_poly(&b, (u8)WP_DRAW_LINE, l, 2);
    g_ncalls = 0;
    ASSERT_TRUE(wp_drawing_render(b.bytes, b.length, 1000, 500, 0, 0, 300, 50, &g_sink) == WP_OK);
    for (i = 0; i < g_ncalls; i++) {
        if (g_calls[i].kind == 'L') { lines++; ASSERT_EQ_U32((u32)g_calls[i].width, 1); }
        else fills++;
    }
    ASSERT_EQ_U32(lines, 1);
    ASSERT_TRUE(fills >= 1);
    wp_drawing_builder_free(&b);
}

/* A centred picture stays centred through RTF: the paragraph's own
   properties, not the default's (the import used to give a picture the
   default style, losing \qc). Laid out in 9360, its 1440-wide line
   starts (9360 - 1440) / 2 in. */
void test_drawing_rtf_keeps_alignment(void)
{
    wp_drawing_builder a;
    wp_document doc, back;
    wp_image h;
    wp_section *sec;
    wp_para_style ps;
    wp_pstyle centred;
    static u8 out[8192];
    u32 len = 0, i;
    static const char src[] = "{\\rtf1\\ansi Above\\par Below\\par}";

    build_all(&a);
    ASSERT_TRUE(format_rtf_import((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_TRUE(wp_document_drawing_add(&doc, a.bytes, a.length, 100, 100, 1440, 720, NULL, &h) == WP_OK);
    memset(&ps, 0, sizeof(ps));
    ps.align = (u8)WP_ALIGN_CENTER;
    ASSERT_TRUE(wp_document_pstyle_find_or_add(&doc, &ps, &centred) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->paras[1].run_count = 0;
    sec->paras[1].kind = (u8)WP_PARA_IMAGE;
    sec->paras[1].image = h;
    sec->paras[1].style = centred;
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out) - 1, &len) == WP_OK);
    ASSERT_TRUE(format_rtf_import(out, len, &back) == WP_OK);
    sec = wp_document_section_get(&back, 0);
    for (i = 0; i < sec->para_count && sec->paras[i].kind != (u8)WP_PARA_IMAGE; i++) {}
    ASSERT_TRUE(i < sec->para_count);
    if (i < sec->para_count) {
        const wp_para_style *got = wp_document_pstyle_get(&back, sec->paras[i].style);
        wp_paragraph_layout *l;
        ASSERT_TRUE(got != NULL && got->align == (u8)WP_ALIGN_CENTER);
        ASSERT_TRUE(wp_paragraph_relayout(&back, &sec->paras[i], 9360) == WP_OK);
        l = (wp_paragraph_layout *)sec->paras[i].layout;
        ASSERT_EQ_U32((u32)l->lines[0].x_twips, 3960);
    }
    wp_document_free(&back);
    wp_document_free(&doc);
    wp_drawing_builder_free(&a);
}
