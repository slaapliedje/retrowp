/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "format_wmf.h"
#include "platform.h"

/* Record functions (the low byte names the call, the high byte its
   parameter count, which is not relied on here). */
#define WMF_EOF               0x0000u
#define WMF_SETWINDOWORG      0x020Bu
#define WMF_SETWINDOWEXT      0x020Cu
#define WMF_LINETO            0x0213u
#define WMF_MOVETO            0x0214u
#define WMF_SELECTOBJECT      0x012Du
#define WMF_DELETEOBJECT      0x01F0u
#define WMF_CREATEPENINDIRECT 0x02FAu
#define WMF_CREATEBRUSHINDIRECT 0x02FCu
#define WMF_POLYGON           0x0324u
#define WMF_POLYLINE          0x0325u
#define WMF_ELLIPSE           0x0418u
#define WMF_RECTANGLE         0x041Bu
#define WMF_POLYPOLYGON       0x0538u
#define WMF_ROUNDRECT         0x061Cu
#define WMF_ARC               0x0817u
#define WMF_PIE               0x081Au
#define WMF_CHORD             0x0830u

#define WMF_PLACEABLE_KEY 0x9AC6CDD7uL

static u16 le16(const u8 *p) { return (u16)((u16)p[0] | ((u16)p[1] << 8)); }
static i32 sle16(const u8 *p) { return (i32)(i16)le16(p); }
static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ---- Reading --------------------------------------------------------- */

/* Objects take the lowest free slot as they are made, so every record
   that makes one -- fonts, palettes, regions -- must take a slot here
   too, or the later SelectObject numbers would point at the wrong one. */
#define WMF_SLOTS 64
enum { SLOT_FREE = 0, SLOT_PEN, SLOT_BRUSH, SLOT_OTHER };
typedef struct {
    u8  kind, style;
    u16 width;
    u8  rgb[3];
} wmf_slot;

typedef struct {
    i32 org_x, org_y;
    i32 sign_x, sign_y;     /* -1 where the window extent is negative */
    wmf_slot slot[WMF_SLOTS];
    i16 run[2 * 128];       /* LineTo points waiting to go out as one line */
    u16 run_n;
    i32 cur_x, cur_y;
} wmf_reader;

static i16 clamp16(i32 v) { return (i16)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static i16 mx(const wmf_reader *r, i32 x) { return clamp16((x - r->org_x) * r->sign_x); }
static i16 my(const wmf_reader *r, i32 y) { return clamp16((y - r->org_y) * r->sign_y); }

static void flush_run(wmf_reader *r, wp_drawing_builder *b)
{
    if (r->run_n >= 2) wp_drawing_poly(b, (u8)WP_DRAW_LINE, r->run, r->run_n);
    r->run_n = 0;
}

static void take_slot(wmf_reader *r, const wmf_slot *s)
{
    u32 i;
    for (i = 0; i < WMF_SLOTS; i++) {
        if (r->slot[i].kind == SLOT_FREE) { r->slot[i] = *s; return; }
    }
}

/* A Windows COLORREF, low byte first: red, green, blue, flags. */
static void colorref(const u8 *p, u8 *rgb) { rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2]; }

wp_status wmf_to_drawing(const u8 *wmf, u32 len, wp_drawing_builder *b,
                         i32 *ext_w, i32 *ext_h)
{
    static wmf_reader r;   /* 0.5 KB: kept off the Amiga's 4 KB stack */
    u32 pos = 0;
    i32 box_w = 0, box_h = 0;

    memset(&r, 0, sizeof(r));
    r.sign_x = r.sign_y = 1;
    *ext_w = *ext_h = 0;

    if (len >= 22u && le32(wmf) == WMF_PLACEABLE_KEY) {
        r.org_x = sle16(wmf + 6);
        r.org_y = sle16(wmf + 8);
        box_w = sle16(wmf + 10) - r.org_x;
        box_h = sle16(wmf + 12) - r.org_y;
        pos = 22u;
    }
    if (pos + 18u > len) return WP_BADFMT;
    if (le16(wmf + pos) != 1u && le16(wmf + pos) != 2u) return WP_BADFMT;
    if (le16(wmf + pos + 2) != 9u) return WP_BADFMT;
    pos += 18u;

    while (pos + 6u <= len && b->error == WP_OK) {
        u32 words = le32(wmf + pos);
        u16 fn = le16(wmf + pos + 4);
        const u8 *p = wmf + pos + 6;
        u32 np;   /* parameter words */

        if (words < 3u || words > (len - pos) / 2u) return WP_BADFMT;
        np = words - 3u;
        pos += words * 2u;
        if (fn == WMF_EOF) break;
        if (fn != WMF_LINETO) {
            if (fn != WMF_MOVETO) flush_run(&r, b);
        }

        switch (fn) {
        case WMF_SETWINDOWORG:
            if (np >= 2) { r.org_y = sle16(p); r.org_x = sle16(p + 2); }
            break;
        case WMF_SETWINDOWEXT:
            if (np >= 2) {
                i32 ey = sle16(p), ex = sle16(p + 2);
                r.sign_x = ex < 0 ? -1 : 1;
                r.sign_y = ey < 0 ? -1 : 1;
                *ext_w = ex < 0 ? -ex : ex;
                *ext_h = ey < 0 ? -ey : ey;
            }
            break;
        case WMF_MOVETO:
            if (np >= 2) {
                flush_run(&r, b);
                r.cur_y = sle16(p); r.cur_x = sle16(p + 2);
            }
            break;
        case WMF_LINETO:
            if (np >= 2) {
                if (r.run_n == 0 || r.run_n >= 128) {
                    if (r.run_n >= 128) {   /* full: go on from its last point */
                        flush_run(&r, b);
                    }
                    r.run[0] = mx(&r, r.cur_x); r.run[1] = my(&r, r.cur_y);
                    r.run_n = 1;
                }
                r.cur_y = sle16(p); r.cur_x = sle16(p + 2);
                r.run[r.run_n * 2] = mx(&r, r.cur_x);
                r.run[r.run_n * 2 + 1] = my(&r, r.cur_y);
                r.run_n++;
            }
            break;
        case WMF_CREATEPENINDIRECT:
            if (np >= 5) {
                wmf_slot s;
                s.kind = SLOT_PEN;
                s.style = (u8)((le16(p) & 0x0Fu) == 5u ? 0 : 1);   /* PS_NULL, else drawn solid */
                s.width = le16(p + 2);
                colorref(p + 6, s.rgb);
                take_slot(&r, &s);
            }
            break;
        case WMF_CREATEBRUSHINDIRECT:
            if (np >= 4) {
                wmf_slot s;
                u16 style = le16(p);
                s.kind = SLOT_BRUSH;
                s.style = (u8)(style == 1u ? 0 : 1);   /* BS_NULL, else filled solid */
                s.width = 0;
                colorref(p + 2, s.rgb);
                if (style == 2u) {   /* hatched: a half-tone of its colour */
                    s.rgb[0] = (u8)((s.rgb[0] + 255u) / 2u);
                    s.rgb[1] = (u8)((s.rgb[1] + 255u) / 2u);
                    s.rgb[2] = (u8)((s.rgb[2] + 255u) / 2u);
                }
                take_slot(&r, &s);
            }
            break;
        case 0x02FBu:   /* CreateFontIndirect */
        case 0x00F7u:   /* CreatePalette */
        case 0x06FFu:   /* CreateRegion */
        case 0x01F9u:   /* CreatePatternBrush */
        case 0x0142u: { /* DibCreatePatternBrush */
            wmf_slot s;
            memset(&s, 0, sizeof(s));
            s.kind = SLOT_OTHER;
            take_slot(&r, &s);
            break;
        }
        case WMF_SELECTOBJECT:
            if (np >= 1 && le16(p) < WMF_SLOTS) {
                const wmf_slot *s = &r.slot[le16(p)];
                if (s->kind == SLOT_PEN) wp_drawing_pen(b, s->style, s->width, s->rgb[0], s->rgb[1], s->rgb[2]);
                else if (s->kind == SLOT_BRUSH) wp_drawing_brush(b, s->style, s->rgb[0], s->rgb[1], s->rgb[2]);
            }
            break;
        case WMF_DELETEOBJECT:
            if (np >= 1 && le16(p) < WMF_SLOTS) r.slot[le16(p)].kind = SLOT_FREE;
            break;
        case WMF_POLYLINE:
        case WMF_POLYGON:
            if (np >= 1) {
                u16 n = le16(p), i;
                i16 *xy;
                if ((u32)n * 2u + 1u > np || n < 2) break;
                xy = (i16 *)mem_alloc((u32)n * 4u);
                if (xy == NULL) { b->error = WP_NOMEM; break; }
                for (i = 0; i < n; i++) {
                    xy[i * 2] = mx(&r, sle16(p + 2 + i * 4));
                    xy[i * 2 + 1] = my(&r, sle16(p + 4 + i * 4));
                }
                wp_drawing_poly(b, (u8)(fn == WMF_POLYLINE ? WP_DRAW_LINE : WP_DRAW_POLYGON), xy, n);
                mem_free(xy);
            }
            break;
        case WMF_POLYPOLYGON:
            /* Each polygon on its own: a hole is filled over, not cut. */
            if (np >= 1) {
                u16 polys = le16(p), k;
                u32 at = 1u + polys;   /* the first point's word */
                if (at > np) break;
                for (k = 0; k < polys; k++) {
                    u16 n = le16(p + 2 + k * 2), i;
                    i16 *xy;
                    if (at + (u32)n * 2u > np) break;
                    if (n >= 2) {
                        xy = (i16 *)mem_alloc((u32)n * 4u);
                        if (xy == NULL) { b->error = WP_NOMEM; break; }
                        for (i = 0; i < n; i++) {
                            xy[i * 2] = mx(&r, sle16(p + (at + i * 2u) * 2u));
                            xy[i * 2 + 1] = my(&r, sle16(p + (at + i * 2u + 1u) * 2u));
                        }
                        wp_drawing_poly(b, (u8)WP_DRAW_POLYGON, xy, n);
                        mem_free(xy);
                    }
                    at += (u32)n * 2u;
                }
            }
            break;
        case WMF_RECTANGLE:
        case WMF_ELLIPSE:
            if (np >= 4) {   /* bottom, right, top, left */
                i16 v[4];
                v[0] = mx(&r, sle16(p + 6)); v[1] = my(&r, sle16(p + 4));
                v[2] = mx(&r, sle16(p + 2)); v[3] = my(&r, sle16(p));
                wp_drawing_shape(b, (u8)(fn == WMF_RECTANGLE ? WP_DRAW_RECT : WP_DRAW_ELLIPSE), v);
            }
            break;
        case WMF_ROUNDRECT:
            if (np >= 6) {   /* corner height, width, bottom, right, top, left */
                i16 v[6];
                v[0] = mx(&r, sle16(p + 10)); v[1] = my(&r, sle16(p + 8));
                v[2] = mx(&r, sle16(p + 6));  v[3] = my(&r, sle16(p + 4));
                v[4] = clamp16(sle16(p + 2)); v[5] = clamp16(sle16(p));
                wp_drawing_shape(b, (u8)WP_DRAW_RRECT, v);
            }
            break;
        case WMF_ARC:
        case WMF_PIE:     /* drawn as their arc, for now */
        case WMF_CHORD:
            if (np >= 8) {   /* yEnd xEnd yStart xStart bottom right top left */
                i16 v[8];
                v[0] = mx(&r, sle16(p + 14)); v[1] = my(&r, sle16(p + 12));
                v[2] = mx(&r, sle16(p + 10)); v[3] = my(&r, sle16(p + 8));
                v[4] = mx(&r, sle16(p + 6));  v[5] = my(&r, sle16(p + 4));
                v[6] = mx(&r, sle16(p + 2));  v[7] = my(&r, sle16(p));
                if (r.sign_x * r.sign_y < 0) {   /* mirrored: the arc runs the other way */
                    i16 t;
                    t = v[4]; v[4] = v[6]; v[6] = t;
                    t = v[5]; v[5] = v[7]; v[7] = t;
                }
                wp_drawing_shape(b, (u8)WP_DRAW_ARC, v);
            }
            break;
        default:
            break;   /* text, bitmaps, clipping, modes: not drawn */
        }
    }
    flush_run(&r, b);
    if (*ext_w == 0 && *ext_h == 0 && box_w > 0 && box_h > 0) {
        *ext_w = box_w;
        *ext_h = box_h;
    }
    return b->error;
}

/* ---- Writing --------------------------------------------------------- */

typedef struct {
    wmf_put_fn put;
    void *user;
    wp_bool emit;           /* the second pass; the first only counts */
    u32 words, max_record;
    wp_bool used[3];        /* object slots: a pen, a brush, one new */
    i32 pen_slot, brush_slot;
} wmf_writer;

static void w16(wmf_writer *w, u16 v)
{
    if (w->emit) {
        u8 b[2];
        b[0] = (u8)(v & 0xFFu);
        b[1] = (u8)(v >> 8);
        w->put(w->user, b, 2u);
    }
    w->words++;
}

static void begin(wmf_writer *w, u16 fn, u32 params)
{
    u32 size = 3u + params;
    if (size > w->max_record) w->max_record = size;
    w16(w, (u16)(size & 0xFFFFu));
    w16(w, (u16)(size >> 16));
    w16(w, fn);
}

static i32 free_slot(wmf_writer *w)
{
    i32 i;
    for (i = 0; i < 3; i++) if (!w->used[i]) { w->used[i] = WP_TRUE; return i; }
    return 0;
}

/* The new object selected, the old one (now unselected) let go. */
static void swap_in(wmf_writer *w, i32 *current, i32 slot)
{
    begin(w, WMF_SELECTOBJECT, 1u); w16(w, (u16)slot);
    if (*current >= 0) {
        begin(w, WMF_DELETEOBJECT, 1u); w16(w, (u16)*current);
        w->used[*current] = WP_FALSE;
    }
    *current = slot;
}

static i16 bd16(const u8 *p) { return (i16)(((u16)p[0] << 8) | p[1]); }

static wp_status write_records(wmf_writer *w, const u8 *ops, u32 len, i32 ext_w, i32 ext_h)
{
    u32 pos = 0;

    w->used[0] = w->used[1] = w->used[2] = WP_FALSE;
    w->pen_slot = w->brush_slot = -1;
    begin(w, WMF_SETWINDOWORG, 2u); w16(w, 0); w16(w, 0);
    begin(w, WMF_SETWINDOWEXT, 2u); w16(w, (u16)ext_h); w16(w, (u16)ext_w);

    while (pos < len) {
        u8 op = ops[pos++];
        const u8 *p = ops + pos;

        if (op == WP_DRAW_PEN) {
            i32 s;
            if (pos + 6u > len) return WP_BADFMT;
            s = free_slot(w);
            begin(w, WMF_CREATEPENINDIRECT, 5u);
            w16(w, (u16)(p[0] ? 0u : 5u));                  /* PS_SOLID or PS_NULL */
            w16(w, (u16)(((u16)p[1] << 8) | p[2])); w16(w, 0);   /* width, as a point */
            w16(w, (u16)(p[3] | ((u16)p[4] << 8))); w16(w, p[5]);
            swap_in(w, &w->pen_slot, s);
            pos += 6u;
        } else if (op == WP_DRAW_BRUSH) {
            i32 s;
            if (pos + 4u > len) return WP_BADFMT;
            s = free_slot(w);
            begin(w, WMF_CREATEBRUSHINDIRECT, 4u);
            w16(w, (u16)(p[0] ? 0u : 1u));                  /* BS_SOLID or BS_NULL */
            w16(w, (u16)(p[1] | ((u16)p[2] << 8))); w16(w, p[3]);
            w16(w, 0);                                      /* hatch */
            swap_in(w, &w->brush_slot, s);
            pos += 4u;
        } else if (op == WP_DRAW_LINE || op == WP_DRAW_POLYGON) {
            u16 n, i;
            if (pos + 2u > len) return WP_BADFMT;
            n = (u16)(((u16)p[0] << 8) | p[1]);
            if (pos + 2u + (u32)n * 4u > len) return WP_BADFMT;
            begin(w, op == WP_DRAW_LINE ? WMF_POLYLINE : WMF_POLYGON, 1u + (u32)n * 2u);
            w16(w, n);
            for (i = 0; i < n; i++) {
                w16(w, (u16)bd16(p + 2 + i * 4));
                w16(w, (u16)bd16(p + 4 + i * 4));
            }
            pos += 2u + (u32)n * 4u;
        } else if (op == WP_DRAW_RECT || op == WP_DRAW_ELLIPSE) {
            if (pos + 8u > len) return WP_BADFMT;
            begin(w, op == WP_DRAW_RECT ? WMF_RECTANGLE : WMF_ELLIPSE, 4u);
            w16(w, (u16)bd16(p + 6)); w16(w, (u16)bd16(p + 4));   /* bottom, right */
            w16(w, (u16)bd16(p + 2)); w16(w, (u16)bd16(p));       /* top, left */
            pos += 8u;
        } else if (op == WP_DRAW_RRECT) {
            if (pos + 12u > len) return WP_BADFMT;
            begin(w, WMF_ROUNDRECT, 6u);
            w16(w, (u16)bd16(p + 10)); w16(w, (u16)bd16(p + 8));  /* corner h, w */
            w16(w, (u16)bd16(p + 6)); w16(w, (u16)bd16(p + 4));
            w16(w, (u16)bd16(p + 2)); w16(w, (u16)bd16(p));
            pos += 12u;
        } else if (op == WP_DRAW_ARC) {
            if (pos + 16u > len) return WP_BADFMT;
            begin(w, WMF_ARC, 8u);
            w16(w, (u16)bd16(p + 14)); w16(w, (u16)bd16(p + 12));  /* end y, x */
            w16(w, (u16)bd16(p + 10)); w16(w, (u16)bd16(p + 8));   /* start y, x */
            w16(w, (u16)bd16(p + 6)); w16(w, (u16)bd16(p + 4));
            w16(w, (u16)bd16(p + 2)); w16(w, (u16)bd16(p));
            pos += 16u;
        } else {
            return WP_BADFMT;
        }
    }
    begin(w, WMF_EOF, 0u);
    return WP_OK;
}

wp_status drawing_to_wmf(const u8 *ops, u32 len, i32 ext_w, i32 ext_h,
                         wmf_put_fn put, void *user, u32 *len_out)
{
    wmf_writer w;
    wp_status st;
    u32 total;

    /* First pass: how long, and the longest record, for the header. */
    memset(&w, 0, sizeof(w));
    st = write_records(&w, ops, len, ext_w, ext_h);
    if (st != WP_OK) return st;
    total = 9u + w.words;
    if (len_out != NULL) *len_out = total * 2u;
    if (put == NULL) return WP_OK;

    w.put = put;
    w.user = user;
    w.emit = WP_TRUE;
    {
        u32 max_record = w.max_record;
        w.words = 0;
        w16(&w, 1u);                          /* in memory */
        w16(&w, 9u);                          /* header words */
        w16(&w, 0x0300u);                     /* Windows 3.0 */
        w16(&w, (u16)(total & 0xFFFFu)); w16(&w, (u16)(total >> 16));
        w16(&w, 3u);                          /* objects at once, at most */
        w16(&w, (u16)(max_record & 0xFFFFu)); w16(&w, (u16)(max_record >> 16));
        w16(&w, 0u);
    }
    return write_records(&w, ops, len, ext_w, ext_h);
}
