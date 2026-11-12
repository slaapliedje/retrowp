/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "drawfile.h"
#include "format_wmf.h"
#include "platform.h"

/* ---- GEM metafiles ----------------------------------------------------
 *
 * The Atari Compendium, Appendix C, "The .GEM File Format": a header
 * (0xFFFF, its length in words, version, NDC flag, extents, page size in
 * tenths of a millimetre, the coordinate system's lower-left and upper-
 * right corners), then records -- VDI opcode, PTSIN count, INTIN count,
 * sub-ID, points, integers -- to an opcode of 0xFFFF. Every file met so
 * far is little-endian, even those an ST wrote; the header's length word
 * (small either way round) says which. The sub-ID is only meaningful on
 * the GDPs (opcode 11), the escapes (5) and NVDI's colour call (138); on
 * the rest writers leave whatever was there. */

#define GEM_POINT_MAX 16000u

typedef struct {
    const u8 *d;
    u32 len;
    wp_bool le;
    i32 llx, ury, sx, sy;       /* x' = (x - llx) * sx - ox, y measured down from ury */
    i32 ox, oy;
    u8  pal[256][3];            /* vs_color may set any of 256 */
    u8  line_rgb[3];
    i32 line_w;
    u8  fill_rgb[3];
    i32 interior, style;
    wp_bool perimeter;
    /* What the builder has been told last, so a record repeats nothing. */
    wp_bool pen_known, pen_on;
    u16 pen_w;
    u8  pen_rgb[3];
    wp_bool brush_known, brush_on;
    u8  brush_rgb[3];
} gem_reader;

static u16 gw(const gem_reader *g, u32 word)
{
    const u8 *p = g->d + word * 2u;
    return g->le ? (u16)((u16)p[0] | ((u16)p[1] << 8)) : (u16)(((u16)p[0] << 8) | p[1]);
}

static i32 gs(const gem_reader *g, u32 word) { return (i32)(i16)gw(g, word); }

static i16 clamp16(i32 v) { return (i16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static i32 raw_x(const gem_reader *g, i32 x) { return (x - g->llx) * g->sx; }
static i32 raw_y(const gem_reader *g, i32 y) { return (y - g->ury) * g->sy; }
static i16 gx(const gem_reader *g, i32 x) { return clamp16(raw_x(g, x) - g->ox); }
static i16 gy(const gem_reader *g, i32 y) { return clamp16(raw_y(g, y) - g->oy); }

/* The VDI's first sixteen colours. */
static const u8 g_vdi_pal[16][3] = {
    {255,255,255}, {0,0,0}, {255,0,0}, {0,255,0}, {0,0,255}, {0,255,255},
    {255,255,0}, {255,0,255}, {192,192,192}, {128,128,128}, {128,0,0},
    {0,128,0}, {0,0,128}, {0,128,128}, {128,128,0}, {128,0,128}
};

static void pal_rgb(const gem_reader *g, i32 idx, u8 *rgb)
{
    if (idx < 0 || idx > 255) idx = 1;
    rgb[0] = g->pal[idx][0]; rgb[1] = g->pal[idx][1]; rgb[2] = g->pal[idx][2];
}

/* A VDI colour level, 0..1000. */
static u8 level(i32 v) { return (u8)(v <= 0 ? 0 : v >= 1000 ? 255 : (v * 255 + 500) / 1000); }

static void pen(gem_reader *g, wp_drawing_builder *b, wp_bool on, i32 width, const u8 *rgb)
{
    u16 w = (u16)(width < 0 ? 0L : width > 65535L ? 65535L : width);
    if (g->pen_known && g->pen_on == on && (!on || (g->pen_w == w &&
        memcmp(g->pen_rgb, rgb, 3) == 0))) return;
    g->pen_known = WP_TRUE;
    g->pen_on = on;
    g->pen_w = w;
    memcpy(g->pen_rgb, rgb, 3);
    wp_drawing_pen(b, (u8)(on ? 1 : 0), w, rgb[0], rgb[1], rgb[2]);
}

static void line_pen(gem_reader *g, wp_drawing_builder *b)
{
    pen(g, b, WP_TRUE, g->line_w > 1 ? g->line_w : 0, g->line_rgb);
}

/* A filled shape's brush -- a pattern as a lighter shade of its colour,
   which is how it reads on paper -- and its perimeter, a hairline in the
   fill colour, as the VDI draws it. */
static void fill_attrs(gem_reader *g, wp_drawing_builder *b)
{
    static const u8 off[3] = { 0, 0, 0 };
    i32 dens = 1000;
    u8 rgb[3];
    wp_bool on = (wp_bool)(g->interior != 0);
    u32 k;

    if (g->interior == 2) dens = g->style >= 8 ? 1000 : g->style <= 0 ? 125 : g->style * 125;
    else if (g->interior == 3) dens = 250;
    else if (g->interior == 4) dens = 500;
    for (k = 0; k < 3; k++) rgb[k] = (u8)(255 - ((255 - (i32)g->fill_rgb[k]) * dens) / 1000);
    if (!g->brush_known || g->brush_on != on || (on && memcmp(g->brush_rgb, rgb, 3) != 0)) {
        g->brush_known = WP_TRUE;
        g->brush_on = on;
        memcpy(g->brush_rgb, rgb, 3);
        wp_drawing_brush(b, (u8)(on ? 1 : 0), rgb[0], rgb[1], rgb[2]);
    }
    if (g->perimeter) pen(g, b, WP_TRUE, 0, g->fill_rgb);
    else pen(g, b, WP_FALSE, 0, off);
}

/* Points pts..pts+n into a mapped buffer; NULL where none can be had. */
static i16 *map_points(const gem_reader *g, u32 pts, u32 n)
{
    i16 *xy;
    u32 i;
    if (n == 0 || n > GEM_POINT_MAX) return NULL;
    xy = (i16 *)mem_alloc(n * 4u);
    if (xy == NULL) return NULL;
    for (i = 0; i < n; i++) {
        xy[i * 2] = gx(g, gs(g, pts + i * 2u));
        xy[i * 2 + 1] = gy(g, gs(g, pts + i * 2u + 1u));
    }
    return xy;
}

/* A Bezier path (v_bez, v_bez_fill): a flag byte a point -- bit 0 starts
   a four-point cubic (anchor, control, control, anchor), bit 1 jumps
   there without drawing -- packed two to an INTIN word in the order the
   bytes lie in the file. Flattened here, eight segments to a cubic, each
   subpath a LINE or (fill) a POLYGON on its own. */
static void bezier(gem_reader *g, wp_drawing_builder *b, u32 pts, u32 n, u32 ints, wp_bool fill)
{
    const u8 *flags = g->d + ints * 2u;
    i16 *src = map_points(g, pts, n), *out;
    u32 i = 0, k = 0;

    if (src == NULL) return;
    out = (i16 *)mem_alloc((n * 8u + 1u) * 4u);
    if (out == NULL) { mem_free(src); b->error = WP_NOMEM; return; }
#define BEZ_FLUSH() do { \
        if (fill ? k >= 3 : k >= 2) wp_drawing_poly(b, (u8)(fill ? WP_DRAW_POLYGON : WP_DRAW_LINE), out, (u16)k); \
        k = 0; } while (0)
    out[0] = src[0]; out[1] = src[1]; k = 1;
    while (i + 1 < n) {
        if (flags[i + 1] & 0x02) {            /* a jump: a new subpath */
            BEZ_FLUSH();
            out[0] = src[(i + 1) * 2]; out[1] = src[(i + 1) * 2 + 1]; k = 1;
            i++;
        } else if ((flags[i] & 0x01) && i + 3 < n) {
            i32 x0 = src[i * 2], y0 = src[i * 2 + 1], x1 = src[i * 2 + 2], y1 = src[i * 2 + 3];
            i32 x2 = src[i * 2 + 4], y2 = src[i * 2 + 5], x3 = src[i * 2 + 6], y3 = src[i * 2 + 7];
            i32 t;
            for (t = 1; t <= 8; t++) {
                i32 u = 8 - t;
                out[k * 2] = (i16)((x0 * u * u * u + 3 * x1 * u * u * t + 3 * x2 * u * t * t + x3 * t * t * t) / 512);
                out[k * 2 + 1] = (i16)((y0 * u * u * u + 3 * y1 * u * u * t + 3 * y2 * u * t * t + y3 * t * t * t) / 512);
                k++;
            }
            i += 3;
        } else {
            out[k * 2] = src[(i + 1) * 2]; out[k * 2 + 1] = src[(i + 1) * 2 + 1]; k++;
            i++;
        }
    }
    BEZ_FLUSH();
#undef BEZ_FLUSH
    mem_free(out);
    mem_free(src);
}

/* An arc's radial points (ARC's xs ys xe ye) from VDI angles, counter-
   clockwise from three o'clock as it looks. */
static void arc_rays(i32 cx, i32 cy, i32 rx, i32 ry, i32 a0, i32 a1, i16 *v)
{
    i32 c, s;
    wp_drawing_polar(a0, &c, &s);
    v[4] = clamp16(cx + (rx * c) / 16384); v[5] = clamp16(cy - (ry * s) / 16384);
    wp_drawing_polar(a1, &c, &s);
    v[6] = clamp16(cx + (rx * c) / 16384); v[7] = clamp16(cy - (ry * s) / 16384);
}

/* A pie slice as a polygon: the centre and its arc, flattened. */
static void pie(gem_reader *g, wp_drawing_builder *b, i32 cx, i32 cy, i32 rx, i32 ry, i32 a0, i32 a1)
{
    i16 xy[2 * 34];
    i32 span, k, n;

    a0 %= 3600; a1 %= 3600;
    if (a0 < 0) a0 += 3600;
    if (a1 < 0) a1 += 3600;
    span = a1 - a0;
    if (span <= 0) span += 3600;
    n = (span * 32 + 3599) / 3600;
    if (n < 2) n = 2;
    xy[0] = clamp16(cx); xy[1] = clamp16(cy);
    for (k = 0; k <= n; k++) {
        i32 c, s;
        wp_drawing_polar(a0 + (span * k) / n, &c, &s);
        xy[(k + 1) * 2] = clamp16(cx + (rx * c) / 16384);
        xy[(k + 1) * 2 + 1] = clamp16(cy - (ry * s) / 16384);
    }
    fill_attrs(g, b);
    wp_drawing_poly(b, (u8)WP_DRAW_POLYGON, xy, (u16)(n + 2));
}

static i32 iabs(i32 v) { return v < 0 ? -v : v; }

static void gem_gdp(gem_reader *g, wp_drawing_builder *b, u16 sub, u32 pts, u32 np, u32 ints, u32 ni)
{
    i16 v[8];
    i32 cx = 0, cy = 0, rx = 0, ry = 0, a0 = 0, a1 = 0;

    if (np >= 1) { cx = gx(g, gs(g, pts)); cy = gy(g, gs(g, pts + 1)); }
    if (ni >= 2) { a0 = gs(g, ints); a1 = gs(g, ints + 1); }
    switch (sub) {
    case 1:   /* v_bar */
    case 8:   /* v_rbox */
    case 9:   /* v_rfbox */
        if (np < 2) return;
        v[0] = gx(g, gs(g, pts)); v[1] = gy(g, gs(g, pts + 1));
        v[2] = gx(g, gs(g, pts + 2)); v[3] = gy(g, gs(g, pts + 3));
        if (sub == 8) {
            static const u8 none[3] = { 0, 0, 0 };
            if (!g->brush_known || g->brush_on) {
                g->brush_known = WP_TRUE; g->brush_on = WP_FALSE;
                wp_drawing_brush(b, 0, none[0], none[1], none[2]);
            }
            line_pen(g, b);
        } else {
            fill_attrs(g, b);
        }
        if (sub == 1) { wp_drawing_shape(b, (u8)WP_DRAW_RECT, v); return; }
        {   /* the VDI's corners: about a fifth of the shorter side */
            i32 c = iabs(v[2] - v[0]) < iabs(v[3] - v[1]) ? iabs(v[2] - v[0]) : iabs(v[3] - v[1]);
            v[4] = v[5] = clamp16(c / 5);
        }
        wp_drawing_shape(b, (u8)WP_DRAW_RRECT, v);
        return;
    case 2:   /* v_arc: centre, radius in the fourth point's x */
    case 3:   /* v_pieslice */
        if (np < 4) return;
        rx = ry = iabs(gs(g, pts + 6));
        break;
    case 4:   /* v_circle: radius in the third point's x */
        if (np < 3) return;
        rx = ry = iabs(gs(g, pts + 4));
        break;
    case 5:   /* v_ellipse: radii in the second point */
    case 6:   /* v_ellarc */
    case 7:   /* v_ellpie */
        if (np < 2) return;
        rx = iabs(gs(g, pts + 2)); ry = iabs(gs(g, pts + 3));
        break;
    default:
        return;   /* text, bezier switches: not drawn */
    }
    v[0] = clamp16(cx - rx); v[1] = clamp16(cy - ry);
    v[2] = clamp16(cx + rx); v[3] = clamp16(cy + ry);
    if (sub == 4 || sub == 5) {
        fill_attrs(g, b);
        wp_drawing_shape(b, (u8)WP_DRAW_ELLIPSE, v);
    } else if (sub == 3 || sub == 7) {
        pie(g, b, cx, cy, rx, ry, a0, a1);
    } else {
        arc_rays(cx, cy, rx, ry, a0, a1, v);
        line_pen(g, b);
        wp_drawing_shape(b, (u8)WP_DRAW_ARC, v);
    }
}

static wp_status gem_read(const u8 *d, u32 len, wp_drawing_builder *b, wp_drawfile_info *info)
{
    gem_reader *g;
    u32 hl, pos, start;
    i32 urx, lly, x0, x1, y0, y1, page_w, page_h, span_x, span_y;
    wp_status st = WP_OK;

    g = (gem_reader *)mem_alloc((u32)sizeof(gem_reader));
    if (g == NULL) return WP_NOMEM;
    memset(g, 0, sizeof(*g));
    g->d = d;
    g->len = len;
    g->le = (wp_bool)(d[3] == 0 || d[2] > d[3]);   /* the length word, small either way */
    hl = gw(g, 1);
    if (hl < 10u || hl * 2u > len) { mem_free(g); return WP_BADFMT; }
    {   /* the VDI's sixteen, then black until vs_color says otherwise */
        u32 k;
        memset(g->pal, 0, sizeof(g->pal));
        for (k = 0; k < 16; k++) memcpy(g->pal[k], g_vdi_pal[k], 3);
    }
    g->line_rgb[0] = g->line_rgb[1] = g->line_rgb[2] = 0;
    g->fill_rgb[0] = g->fill_rgb[1] = g->fill_rgb[2] = 0;
    g->line_w = 1;
    g->perimeter = WP_TRUE;

    /* The coordinate system; none given is NDC, (0, 0) at the lower left. */
    g->llx = hl >= 14u ? gs(g, 10) : 0; lly = hl >= 14u ? gs(g, 11) : 0;
    urx = hl >= 14u ? gs(g, 12) : 0;    g->ury = hl >= 14u ? gs(g, 13) : 0;
    if (g->llx == 0 && lly == 0 && urx == 0 && g->ury == 0) { urx = 32767; g->ury = 32767; }
    g->sx = urx >= g->llx ? 1 : -1;
    g->sy = lly >= g->ury ? 1 : -1;
    span_x = iabs(urx - g->llx);
    span_y = iabs(lly - g->ury);

    /* The drawing is the extents box where the file sets one, else the
       whole coordinate system. */
    x0 = raw_x(g, gs(g, 4)); x1 = raw_x(g, gs(g, 6));
    y0 = raw_y(g, gs(g, 5)); y1 = raw_y(g, gs(g, 7));
    if (x0 > x1) { i32 t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { i32 t = y0; y0 = y1; y1 = t; }
    if (x1 <= x0 || y1 <= y0) { x0 = 0; x1 = span_x; y0 = 0; y1 = span_y; }
    /* A coordinate system no taller (or wider) than a line is a rule --
       Atari Works draws its signature lines so -- one unit across. */
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    g->ox = x0;
    g->oy = y0;
    info->ext_w = x1 - x0;
    info->ext_h = y1 - y0;
    if (info->ext_w > 32767) info->ext_w = 32767;
    if (info->ext_h > 32767) info->ext_h = 32767;
    page_w = gs(g, 8);
    page_h = gs(g, 9);
    info->width_twips = info->height_twips = 0;
    if (page_w > 0 && page_h > 0 && span_x > 0 && span_y > 0) {   /* tenths of a millimetre */
        info->width_twips = ((info->ext_w * page_w) / span_x) * 1440L / 254L;
        info->height_twips = ((info->ext_h * page_h) / span_y) * 1440L / 254L;
    }

    start = b->length;
    pos = hl;
    while ((pos + 4u) * 2u <= len && b->error == WP_OK) {
        u16 op = gw(g, pos), sub;
        u32 np, ni, pts, ints;

        if (op == 0xFFFFu) break;
        np = gw(g, pos + 1);
        ni = gw(g, pos + 2);
        sub = gw(g, pos + 3);
        pts = pos + 4u;
        ints = pts + np * 2u;
        if ((ints + ni) * 2u > len) { st = WP_BADFMT; break; }
        pos = ints + ni;

        switch (op) {
        case 6:    /* v_pline, or v_bez with its flags */
        case 9:    /* v_fillarea, or v_bez_fill */
            if (np < 2) break;
            if (ni > 0 && ni == (np + 1u) / 2u) {
                if (op == 6) line_pen(g, b); else fill_attrs(g, b);
                bezier(g, b, pts, np, ints, (wp_bool)(op == 9));
            } else {
                i16 *xy = map_points(g, pts, np);
                if (xy == NULL) break;
                if (op == 6) {
                    line_pen(g, b);
                    wp_drawing_poly(b, (u8)WP_DRAW_LINE, xy, (u16)np);
                } else if (np >= 3) {
                    fill_attrs(g, b);
                    wp_drawing_poly(b, (u8)WP_DRAW_POLYGON, xy, (u16)np);
                }
                mem_free(xy);
            }
            break;
        case 11:
            gem_gdp(g, b, sub, pts, np, ints, ni);
            break;
        case 14:   /* vs_color: index, red, green, blue in thousandths */
            if (ni >= 4 && gs(g, ints) >= 0 && gs(g, ints) < 256) {
                i32 k = gs(g, ints);
                g->pal[k][0] = level(gs(g, ints + 1));
                g->pal[k][1] = level(gs(g, ints + 2));
                g->pal[k][2] = level(gs(g, ints + 3));
            }
            break;
        case 16:   /* vsl_width */
            if (np >= 1) g->line_w = iabs(gs(g, pts));
            break;
        case 17:   /* vsl_color */
            if (ni >= 1) pal_rgb(g, gs(g, ints), g->line_rgb);
            break;
        case 23:   /* vsf_interior */
            if (ni >= 1) g->interior = gs(g, ints);
            break;
        case 24:   /* vsf_style */
            if (ni >= 1) g->style = gs(g, ints);
            break;
        case 25:   /* vsf_color */
            if (ni >= 1) pal_rgb(g, gs(g, ints), g->fill_rgb);
            break;
        case 104:  /* vsf_perimeter */
            if (ni >= 1) g->perimeter = (wp_bool)(gs(g, ints) != 0);
            break;
        case 138:  /* NVDI's colour as RGB: sub-ID 1 the fill, 2 the line */
            if (ni >= 3) {
                u8 *rgb = sub == 1 ? g->fill_rgb : sub == 2 ? g->line_rgb : NULL;
                if (rgb != NULL) {
                    rgb[0] = level(gs(g, ints)); rgb[1] = level(gs(g, ints + 1));
                    rgb[2] = level(gs(g, ints + 2));
                }
            }
            break;
        default:
            break;  /* text, markers, line styles, escapes: not drawn */
        }
    }
    mem_free(g);
    if (b->error != WP_OK) return b->error;
    if (b->length == start || info->ext_w <= 0 || info->ext_h <= 0) return WP_BADFMT;
    (void)st;   /* a file cut short keeps what came before it */
    return WP_OK;
}

/* ---- Either kind ----------------------------------------------------- */

#define WMF_PLACEABLE_KEY 0x9AC6CDD7uL

static u32 rd32le(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static wp_bool is_gem(const u8 *d, u32 len)
{
    return (wp_bool)(len >= 28u && d[0] == 0xFF && d[1] == 0xFF &&
                     ((d[3] == 0 && d[2] >= 10) || (d[2] == 0 && d[3] >= 10)));
}

static wp_bool is_wmf(const u8 *d, u32 len)
{
    if (len >= 22u && rd32le(d) == WMF_PLACEABLE_KEY) return WP_TRUE;
    return (wp_bool)(len >= 18u && (d[0] == 1 || d[0] == 2) && d[1] == 0 && d[2] == 9 && d[3] == 0 &&
                     d[4] == 0 && (d[5] == 1 || d[5] == 3));
}

wp_bool drawfile_detect(const u8 *bytes, u32 len)
{
    return (wp_bool)(is_gem(bytes, len) || is_wmf(bytes, len));
}

wp_status drawfile_read(const u8 *bytes, u32 len, wp_drawing_builder *b, wp_drawfile_info *info)
{
    memset(info, 0, sizeof(*info));
    if (is_gem(bytes, len)) return gem_read(bytes, len, b, info);
    if (is_wmf(bytes, len)) {
        u32 start = b->length;
        wp_status st = wmf_to_drawing(bytes, len, b, &info->ext_w, &info->ext_h);
        if (st != WP_OK) return st;
        if (b->length == start || info->ext_w <= 0 || info->ext_h <= 0) return WP_BADFMT;
        /* A placeable header says how many units make an inch. */
        if (rd32le(bytes) == WMF_PLACEABLE_KEY) {
            i32 inch = (i32)((u32)bytes[14] | ((u32)bytes[15] << 8));
            if (inch > 0) {
                info->width_twips = (info->ext_w * 1440L) / inch;
                info->height_twips = (info->ext_h * 1440L) / inch;
            }
        }
        return WP_OK;
    }
    return WP_UNSUPPORTED;
}
