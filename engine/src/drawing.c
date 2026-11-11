/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "drawing.h"
#include "platform.h"

/* ---- Building -------------------------------------------------------- */

void wp_drawing_builder_init(wp_drawing_builder *b)
{
    b->bytes = NULL;
    b->length = b->capacity = 0;
    b->error = WP_OK;
}

void wp_drawing_builder_free(wp_drawing_builder *b)
{
    mem_free(b->bytes);
    wp_drawing_builder_init(b);
}

/* Room for n more bytes; WP_FALSE (and the error kept) if there is none. */
static wp_bool room(wp_drawing_builder *b, u32 n)
{
    if (b->error != WP_OK) return WP_FALSE;
    if (b->length + n > b->capacity) {
        u32 cap = b->capacity < 64u ? 64u : b->capacity;
        void *mem;
        while (cap < b->length + n) cap *= 2u;
        mem = mem_realloc(b->bytes, b->capacity, cap);
        if (mem == NULL) { b->error = WP_NOMEM; return WP_FALSE; }
        b->bytes = (u8 *)mem;
        b->capacity = cap;
    }
    return WP_TRUE;
}

static void put8(wp_drawing_builder *b, u8 v) { b->bytes[b->length++] = v; }
static void put16(wp_drawing_builder *b, u16 v)
{
    b->bytes[b->length++] = (u8)(v >> 8);
    b->bytes[b->length++] = (u8)(v & 0xFFu);
}

void wp_drawing_pen(wp_drawing_builder *b, u8 style, u16 width, u8 r, u8 g, u8 bl)
{
    if (!room(b, 7u)) return;
    put8(b, (u8)WP_DRAW_PEN); put8(b, style); put16(b, width);
    put8(b, r); put8(b, g); put8(b, bl);
}

void wp_drawing_brush(wp_drawing_builder *b, u8 style, u8 r, u8 g, u8 bl)
{
    if (!room(b, 5u)) return;
    put8(b, (u8)WP_DRAW_BRUSH); put8(b, style);
    put8(b, r); put8(b, g); put8(b, bl);
}

void wp_drawing_poly(wp_drawing_builder *b, u8 op, const i16 *xy, u16 n)
{
    u32 i;
    if (!room(b, 3u + (u32)n * 4u)) return;
    put8(b, op); put16(b, n);
    for (i = 0; i < (u32)n * 2u; i++) put16(b, (u16)xy[i]);
}

static u32 shape_values(u8 op)
{
    return op == WP_DRAW_RRECT ? 6u : op == WP_DRAW_ARC ? 8u : 4u;
}

void wp_drawing_shape(wp_drawing_builder *b, u8 op, const i16 *v)
{
    u32 i, n = shape_values(op);
    if (!room(b, 1u + n * 2u)) return;
    put8(b, op);
    for (i = 0; i < n; i++) put16(b, (u16)v[i]);
}

wp_status wp_document_drawing_add(wp_document *doc, const u8 *ops, u32 len,
                                  i32 ext_w, i32 ext_h, i32 display_w, i32 display_h,
                                  const char *name, wp_image *out)
{
    wp_image_desc desc;

    if (len == 0 || ext_w <= 0 || ext_h <= 0) return WP_BADFMT;
    memset(&desc, 0, sizeof(desc));
    desc.width_px = ext_w;
    desc.height_px = ext_h;
    desc.display_width_twips = display_w;
    desc.display_height_twips = display_h;
    if (wp_strpool_append(&doc->strings, ops, len, &desc.drawing) != WP_OK) return WP_NOMEM;
    if (name != NULL && name[0] != '\0' &&
        wp_strpool_append(&doc->strings, (const u8 *)name, (u32)strlen(name),
                          &desc.source_name) != WP_OK) return WP_NOMEM;
    return wp_document_image_add(doc, &desc, out);
}

/* ---- Rendering ------------------------------------------------------- */

/* sin over a quarter turn, 64 steps, x 16384: angles here are 256ths of
   a turn, so the targets need no floating point. */
static const i16 g_sin_q[65] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756,
    5139, 5520, 5897, 6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102,
    9434, 9760, 10080, 10394, 10702, 11003, 11297, 11585, 11866, 12140,
    12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449,
    14635, 14811, 14978, 15137, 15286, 15426, 15557, 15679, 15791, 15893,
    15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384
};

static i32 isin(i32 a)
{
    a &= 255;
    if (a <= 64) return g_sin_q[a];
    if (a <= 128) return g_sin_q[128 - a];
    if (a <= 192) return -g_sin_q[a - 128];
    return -g_sin_q[256 - a];
}

static i32 icos(i32 a) { return isin(a + 64); }

void wp_drawing_polar(i32 tenths, i32 *cos_out, i32 *sin_out)
{
    i32 a;
    tenths %= 3600;
    if (tenths < 0) tenths += 3600;
    a = (tenths * 256 + 1800) / 3600;
    *cos_out = icos(a);
    *sin_out = isin(a);
}

/* The angle (256ths, counter-clockwise as it looks, y down) of the
   vector dx, dy: the table angle whose direction it lies closest to. */
static i32 iangle(i32 dx, i32 dy)
{
    i32 best = 0, best_dot = 0, a;
    wp_bool first = WP_TRUE;

    dy = -dy;   /* y down on the page, up in the angle */
    while (dx > 16383 || dx < -16383 || dy > 16383 || dy < -16383) { dx /= 2; dy /= 2; }
    for (a = 0; a < 256; a++) {
        i32 dot = dx * icos(a) + dy * isin(a);
        if (first || dot > best_dot) { best = a; best_dot = dot; first = WP_FALSE; }
    }
    return best;
}

typedef struct {
    i32 ext_w, ext_h, x, y, w, h;
} xform;

static i32 tx(const xform *t, i32 v) { return t->x + (v * t->w) / t->ext_w; }
static i32 ty(const xform *t, i32 v) { return t->y + (v * t->h) / t->ext_h; }

/* The state the records change. The pen's width in pixels across and
   down: on a screen whose pixels are not square the two differ, and a
   pen drawn a pixel count wide in both would draw horizontal strokes
   fatter than vertical ones. */
typedef struct {
    u8 pen_on, brush_on;
    u8 pen_rgb[3], brush_rgb[3];
    i32 pen_px, pen_py;
} pen_state;

/* Segments for a full ellipse of these radii in pixels: enough that
   none is longer than a few pixels, a power of two from 8 to 64. */
static i32 segments(i32 rx, i32 ry)
{
    i32 r = rx > ry ? rx : ry, n = 8;
    while (n < 64 && n * 3 < r * 6) n *= 2;
    return n;
}

static i32 isqrt(i32 v)
{
    i32 r = 0, bit = 1L << 30;

    if (v <= 0) return 0;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* a / b to the nearest, b > 0. */
static i32 rdiv(i32 a, i32 b)
{
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

/* A line: a pixel wide, as a line; wider, as polygons -- each segment
   a quadrilateral, an octagon at every joint and end -- with the pen an
   ellipse pen_px across and pen_py down, so it is round on paper
   whatever shape the device's pixels are. */
static void stroke(const wp_drawing_sink *s, const pen_state *ps, const i32 *xy, u16 n)
{
    i32 q[16];
    i32 rx = ps->pen_px, ry = ps->pen_py;
    u16 i;

    if (!ps->pen_on || n < 2) return;
    if (rx <= 1 && ry <= 1) {
        if (s->line != NULL) s->line(s->user, xy, n, 1, ps->pen_rgb);
        return;
    }
    if (s->fill == NULL) return;
    for (i = 0; i + 1 < n; i++) {
        i32 ax = xy[i * 2], ay = xy[i * 2 + 1], bx = xy[i * 2 + 2], by = xy[i * 2 + 3];
        /* The direction with y scaled so that the pen is a circle. */
        i32 dx = bx - ax, dy = ((by - ay) * rx) / ry, len;

        while (dx > 16000 || dx < -16000 || dy > 16000 || dy < -16000) { dx /= 2; dy /= 2; }
        len = isqrt(dx * dx + dy * dy);
        if (len == 0) continue;
        q[0] = ax + rdiv(-dy * (rx / 2), len);       q[1] = ay + rdiv(dx * (ry / 2), len);
        q[2] = bx + rdiv(-dy * (rx / 2), len);       q[3] = by + rdiv(dx * (ry / 2), len);
        q[4] = bx + rdiv(dy * (rx - rx / 2), len);   q[5] = by + rdiv(-dx * (ry - ry / 2), len);
        q[6] = ax + rdiv(dy * (rx - rx / 2), len);   q[7] = ay + rdiv(-dx * (ry - ry / 2), len);
        s->fill(s->user, q, 4, ps->pen_rgb);
    }
    /* A pen a pixel or two thick one way makes polygons a row or a
       column thin, which a VDI's polygon fill can leave out altogether:
       the centre line too, a pixel wide, so the stroke is never lost. */
    if ((rx <= 2 || ry <= 2) && s->line != NULL) s->line(s->user, xy, n, 1, ps->pen_rgb);
    if (rx >= 3 || ry >= 3) {
        i32 hx = rx / 2, hy = ry / 2;
        i32 kx = (hx * 106 + 128) / 256, ky = (hy * 106 + 128) / 256;   /* tan 22.5 */
        for (i = 0; i < n; i++) {
            i32 x = xy[i * 2], y = xy[i * 2 + 1];
            q[0] = x + hx;  q[1] = y - ky;   q[2] = x + hx;  q[3] = y + ky;
            q[4] = x + kx;  q[5] = y + hy;   q[6] = x - kx;  q[7] = y + hy;
            q[8] = x - hx;  q[9] = y + ky;   q[10] = x - hx; q[11] = y - ky;
            q[12] = x - kx; q[13] = y - hy;  q[14] = x + kx; q[15] = y - hy;
            s->fill(s->user, q, 8, ps->pen_rgb);
        }
    }
}

/* A closed outline: filled, then stroked round (back to its first point). */
static void closed(const wp_drawing_sink *s, const pen_state *ps, i32 *xy, u16 n, wp_bool fill)
{
    if (n < 2) return;
    if (fill && ps->brush_on && n >= 3 && s->fill != NULL) s->fill(s->user, xy, n, ps->brush_rgb);
    xy[n * 2] = xy[0];
    xy[n * 2 + 1] = xy[1];
    stroke(s, ps, xy, (u16)(n + 1));
}

/* Points along the ellipse with centre cx, cy and radii rx, ry from
   angle a0 to a1 (a1 > a0) in `steps` steps, appended at xy[2k]. */
static u16 arc_points(i32 *xy, u16 k, i32 cx, i32 cy, i32 rx, i32 ry, i32 a0, i32 a1, i32 steps)
{
    i32 i;
    for (i = 0; i <= steps; i++) {
        i32 a = a0 + ((a1 - a0) * i) / steps;
        xy[k * 2] = cx + (rx * icos(a)) / 16384;
        xy[k * 2 + 1] = cy - (ry * isin(a)) / 16384;
        k++;
    }
    return k;
}

/* An ellipse (64 points), a rounded rectangle (4 x 17) or an arc (65),
   and the point that closes it. Static, not on the stack: an Amiga
   program gets 4 KB of stack in all. */
static i32 g_curve[2 * (68 + 1)];

/* The angle of the ray from the centre of the box v[0..3] through px, py,
   taken on the box squashed to a square, so that a point on the ellipse
   names its own angle. */
static i32 ray_angle(const i32 *v, i32 px, i32 py)
{
    i32 sw = v[2] - v[0], sh = v[3] - v[1];
    if (sw < 0) sw = -sw;
    if (sh < 0) sh = -sh;
    while (sw > 4096 || sh > 4096) { sw /= 2; sh /= 2; }
    return iangle((px - (v[0] + v[2]) / 2) * (sh > 0 ? sh : 1),
                  (py - (v[1] + v[3]) / 2) * (sw > 0 ? sw : 1));
}

static i16 rd16(const u8 *p) { return (i16)(((u16)p[0] << 8) | p[1]); }

wp_status wp_drawing_render(const u8 *ops, u32 len, i32 ext_w, i32 ext_h,
                            i32 x, i32 y, i32 w, i32 h, const wp_drawing_sink *sink)
{
    xform t;
    pen_state ps;
    u32 pos = 0;
    wp_status result = WP_OK;
    i32 *curve = g_curve;

    if (ext_w <= 0 || ext_h <= 0 || w <= 0 || h <= 0) return WP_OK;
    t.ext_w = ext_w; t.ext_h = ext_h; t.x = x; t.y = y; t.w = w; t.h = h;
    ps.pen_on = 1; ps.brush_on = 1; ps.pen_px = ps.pen_py = 1;
    ps.pen_rgb[0] = ps.pen_rgb[1] = ps.pen_rgb[2] = 0;
    ps.brush_rgb[0] = ps.brush_rgb[1] = ps.brush_rgb[2] = 255;

    while (pos < len) {
        u8 op = ops[pos++];
        const u8 *p = ops + pos;

        if (op == WP_DRAW_PEN) {
            i32 units, wd;
            if (pos + 6u > len) return WP_BADFMT;
            ps.pen_on = p[0] != 0;
            units = (i32)(((u16)p[1] << 8) | p[2]);
            wd = (units * w + ext_w / 2) / ext_w;
            ps.pen_px = wd < 1 ? 1 : wd;
            wd = (units * h + ext_h / 2) / ext_h;
            ps.pen_py = wd < 1 ? 1 : wd;
            ps.pen_rgb[0] = p[3]; ps.pen_rgb[1] = p[4]; ps.pen_rgb[2] = p[5];
            pos += 6u;
        } else if (op == WP_DRAW_BRUSH) {
            if (pos + 4u > len) return WP_BADFMT;
            ps.brush_on = p[0] != 0;
            ps.brush_rgb[0] = p[1]; ps.brush_rgb[1] = p[2]; ps.brush_rgb[2] = p[3];
            pos += 4u;
        } else if (op == WP_DRAW_LINE || op == WP_DRAW_POLYGON) {
            u16 n, i;
            i32 *xy;
            if (pos + 2u > len) return WP_BADFMT;
            n = (u16)(((u16)p[0] << 8) | p[1]);
            if (pos + 2u + (u32)n * 4u > len) return WP_BADFMT;
            pos += 2u + (u32)n * 4u;
            if (n < 2 || n > 16000u) continue;
            xy = (i32 *)mem_alloc(((u32)n + 1u) * 2u * (u32)sizeof(i32));
            if (xy == NULL) { result = WP_NOMEM; continue; }
            for (i = 0; i < n; i++) {
                xy[i * 2] = tx(&t, rd16(p + 2 + i * 4));
                xy[i * 2 + 1] = ty(&t, rd16(p + 4 + i * 4));
            }
            if (op == WP_DRAW_LINE) stroke(sink, &ps, xy, n);
            else closed(sink, &ps, xy, n, WP_TRUE);
            mem_free(xy);
        } else if (op >= WP_DRAW_RECT && op <= WP_DRAW_ARC) {
            u32 nv = shape_values(op), i;
            i32 v[8];
            i32 l, tp, r, b, cx, cy, rx, ry, n;
            u16 k = 0;

            if (pos + nv * 2u > len) return WP_BADFMT;
            for (i = 0; i < nv; i++) v[i] = rd16(p + i * 2u);
            pos += nv * 2u;
            l = tx(&t, v[0]); tp = ty(&t, v[1]); r = tx(&t, v[2]); b = ty(&t, v[3]);
            if (r < l) { i32 s = l; l = r; r = s; }
            if (b < tp) { i32 s = tp; tp = b; b = s; }
            cx = (l + r) / 2; cy = (tp + b) / 2; rx = (r - l) / 2; ry = (b - tp) / 2;

            if (op == WP_DRAW_RECT) {
                curve[0] = l; curve[1] = tp; curve[2] = r; curve[3] = tp;
                curve[4] = r; curve[5] = b;  curve[6] = l; curve[7] = b;
                closed(sink, &ps, curve, 4, WP_TRUE);
            } else if (op == WP_DRAW_ELLIPSE) {
                n = segments(rx, ry);
                k = arc_points(curve, 0, cx, cy, rx, ry, 0, 256 - 256 / n, n - 1);
                closed(sink, &ps, curve, k, WP_TRUE);
            } else if (op == WP_DRAW_RRECT) {
                /* Four quarter ellipses of the corner's radii, joined. */
                i32 qx = (v[4] * w) / ext_w / 2, qy = (v[5] * h) / ext_h / 2;
                if (qx < 0) qx = -qx;
                if (qy < 0) qy = -qy;
                if (qx > rx) qx = rx;
                if (qy > ry) qy = ry;
                n = segments(qx, qy) / 4;
                if (n < 2) n = 2;
                k = arc_points(curve, k, r - qx, tp + qy, qx, qy, 0, 64, n);
                k = arc_points(curve, k, l + qx, tp + qy, qx, qy, 64, 128, n);
                k = arc_points(curve, k, l + qx, b - qy, qx, qy, 128, 192, n);
                k = arc_points(curve, k, r - qx, b - qy, qx, qy, 192, 256, n);
                closed(sink, &ps, curve, k, WP_TRUE);
            } else {
                i32 a0 = ray_angle(v, v[4], v[5]), a1 = ray_angle(v, v[6], v[7]);
                i32 steps;
                if (a1 <= a0) a1 += 256;
                n = segments(rx, ry);
                steps = (n * (a1 - a0) + 255) / 256;
                if (steps < 1) steps = 1;
                k = arc_points(curve, 0, cx, cy, rx, ry, a0, a1, steps);
                stroke(sink, &ps, curve, k);
            }
        } else {
            return WP_BADFMT;   /* an op this reader does not know: stop */
        }
    }
    return result;
}

wp_status wp_drawing_render_image(const wp_document *doc, const wp_image_desc *desc,
                                  i32 x, i32 y, i32 w, i32 h, const wp_drawing_sink *sink)
{
    if (desc == NULL || !WP_IMAGE_IS_DRAWING(desc)) return WP_BADFMT;
    return wp_drawing_render(wp_strpool_get(&doc->strings, desc->drawing), desc->drawing.length,
                             desc->width_px, desc->height_px, x, y, w, h, sink);
}
