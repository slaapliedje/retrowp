/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "pagecanvas.h"
#include "pageview.h"
#include "layout.h"
#include "selspan.h"

void wp_pagecanvas_init(wp_pagecanvas *c, wp_page_px_fn x_px, wp_page_px_fn y_px,
                        wp_page_px_fn x_tw, wp_page_px_fn y_tw)
{
    memset(c, 0, sizeof(*c));
    c->x_px = x_px;
    c->y_px = y_px;
    c->x_tw = x_tw;
    c->y_tw = y_tw;
    c->fit = WP_ZOOM_FIT_WIDTH;
    c->zoom = c->set_zoom = 100;
    c->grid_twips = 360;   /* a quarter inch */
}

void wp_pagecanvas_free(wp_pagecanvas *c)
{
    if (c->pages != NULL) mem_free(c->pages);
    c->pages = NULL;
    c->page_count = c->page_cap = 0;
}

static i32 scale(i32 v, i32 num, i32 den) { return (i32)(((long)v * num) / den); }

i32 wp_pagecanvas_zx(const wp_pagecanvas *c, i32 twips) { return c->x_px(scale(twips, c->zoom, 100)); }
i32 wp_pagecanvas_zy(const wp_pagecanvas *c, i32 twips) { return c->y_px(scale(twips, c->zoom, 100)); }
i32 wp_pagecanvas_tx(const wp_pagecanvas *c, i32 px) { return scale(c->x_tw(px), 100, c->zoom); }
i32 wp_pagecanvas_ty(const wp_pagecanvas *c, i32 px) { return scale(c->y_tw(px), 100, c->zoom); }

static void clamp_scroll(wp_pagecanvas *c)
{
    i32 mx = c->canvas_w - c->view_w, my = c->canvas_h - c->view_h;
    if (c->scroll_x > mx) c->scroll_x = mx;
    if (c->scroll_y > my) c->scroll_y = my;
    if (c->scroll_x < 0) c->scroll_x = 0;
    if (c->scroll_y < 0) c->scroll_y = 0;
}

wp_status wp_pagecanvas_update(wp_pagecanvas *c, wp_document *doc, i32 view_w, i32 view_h)
{
    u32 si, pi, li, n = 0, k;
    i32 max_w = 0, max_h = 0, y;

    c->view_w = view_w;
    c->view_h = view_h;
    /* The pages, each from its first line: a page's lines are together
       in reading order (a frame's and a note's carry the page of the
       text before them). */
    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph_layout *l = (const wp_paragraph_layout *)sec->paras[pi].layout;
            if (l == NULL) continue;
            for (li = 0; li < l->line_count; li++) {
                u32 p = l->lines[li].page;
                if (p < n) continue;
                if (p >= c->page_cap) {
                    u32 cap = c->page_cap < 16u ? 16u : c->page_cap * 2u;
                    void *mem;
                    while (cap <= p) cap *= 2u;
                    mem = mem_realloc(c->pages, c->page_cap * (u32)sizeof(wp_pagecanvas_page),
                                      cap * (u32)sizeof(wp_pagecanvas_page));
                    if (mem == NULL) return WP_NOMEM;
                    c->pages = (wp_pagecanvas_page *)mem;
                    c->page_cap = cap;
                }
                for (k = n; k <= p; k++) {   /* a page with no line of its own takes this one's */
                    c->pages[k].si = si;
                    c->pages[k].pi = pi;
                    c->pages[k].li = li;
                    c->pages[k].section = si;
                }
                n = p + 1;
            }
        }
    }
    c->page_count = n;
    for (k = 0; k < n; k++) {
        const wp_section *sec = &doc->sections[c->pages[k].section];
        if (sec->page_width > max_w) max_w = sec->page_width;
        if (sec->page_height > max_h) max_h = sec->page_height;
    }
    /* The zoom a fit asks for, at 100% conversions. */
    if (c->fit != WP_ZOOM_FIXED && max_w > 0) {
        i32 w100 = c->x_px(max_w), h100 = c->y_px(max_h);
        i32 z = w100 > 0 ? scale(view_w - 2 * WP_PAGECANVAS_GAP_PX, 100, w100) : 100;
        if (c->fit == WP_ZOOM_FIT_PAGE && h100 > 0) {
            i32 zh = scale(view_h - 2 * WP_PAGECANVAS_GAP_PX, 100, h100);
            if (zh < z) z = zh;
        }
        c->zoom = z;
    } else {
        c->zoom = c->set_zoom;
    }
    if (c->zoom < 10) c->zoom = 10;
    if (c->zoom > 400) c->zoom = 400;

    y = WP_PAGECANVAS_GAP_PX;
    c->canvas_w = view_w;
    for (k = 0; k < n; k++) {
        const wp_section *sec = &doc->sections[c->pages[k].section];
        c->pages[k].top = y;
        c->pages[k].w = wp_pagecanvas_zx(c, sec->page_width);
        c->pages[k].h = wp_pagecanvas_zy(c, sec->page_height);
        if (c->pages[k].w + 2 * WP_PAGECANVAS_GAP_PX > c->canvas_w) c->canvas_w = c->pages[k].w + 2 * WP_PAGECANVAS_GAP_PX;
        y += c->pages[k].h + WP_PAGECANVAS_GAP_PX;
    }
    c->canvas_h = y;
    clamp_scroll(c);
    return WP_OK;
}

wp_bool wp_pagecanvas_page_rect(const wp_pagecanvas *c, u32 page, i32 *x, i32 *y, i32 *w, i32 *h)
{
    if (page >= c->page_count) return WP_FALSE;
    *w = c->pages[page].w;
    *h = c->pages[page].h;
    *x = (c->canvas_w - *w) / 2;
    *y = c->pages[page].top;
    return WP_TRUE;
}

/* A page's lines in order, each with its top on the paper as the page
   printer puts it (a printing walk: footnotes at the foot). */
typedef struct {
    wp_document *doc;
    u32 page, si, pi, li;
    wp_line_walk walk;
    wp_bool started;
} page_iter;

static i32 same_twips(i32 t) { return t; }

static void iter_start(page_iter *it, const wp_pagecanvas *c, wp_document *doc, u32 page)
{
    it->doc = doc;
    it->page = page;
    it->si = c->pages[page].si;
    it->pi = c->pages[page].pi;
    it->li = c->pages[page].li;
    it->started = WP_FALSE;
}

/* The next line of the page: *line, its section and paragraph, its top
   in twips from the paper's top. */
static wp_bool iter_next(page_iter *it, const wp_line **line_out, u32 *si, u32 *pi, u32 *li, i32 *top)
{
    while (it->si < it->doc->section_count) {
        wp_section *sec = &it->doc->sections[it->si];
        if (it->pi >= sec->para_count) { it->si++; it->pi = 0; it->li = 0; continue; }
        {
            const wp_paragraph_layout *l = (const wp_paragraph_layout *)sec->paras[it->pi].layout;
            const wp_line *line;
            if (l == NULL || it->li >= l->line_count) { it->pi++; it->li = 0; continue; }
            line = &l->lines[it->li];
            if (line->page > it->page) return WP_FALSE;
            if (line->page < it->page) { it->li++; continue; }
            if (!it->started) {
                wp_line_walk_start(&it->walk, sec->margin_top);
                it->walk.paper = WP_TRUE;
                it->started = WP_TRUE;
            }
            *line_out = line;
            *si = it->si;
            *pi = it->pi;
            *li = it->li;
            *top = wp_line_walk_next(&it->walk, line, same_twips);
            it->li++;
            return WP_TRUE;
        }
    }
    return WP_FALSE;
}

/* A line's justify extra, as the painters work it out. */
static i32 line_extra(wp_document *doc, wp_paragraph *para, const wp_line *line, u32 li)
{
    const wp_paragraph_layout *l = (const wp_paragraph_layout *)para->layout;
    const wp_para_style *ps = wp_document_pstyle_get(doc, para->style);
    return wp_justify_space_extra_twips(doc, para, line, ps != NULL ? ps->align : WP_ALIGN_LEFT,
                                        (wp_bool)(li + 1 == l->line_count), line->avail_twips);
}

/* The x, on the paper, where a line's content starts. */
static i32 line_left(const wp_section *sec, const wp_line *line)
{
    return sec->margin_left + line->cell_x0 + line->x_twips;
}

static void fill(plat_gc *gc, i32 x, i32 y, i32 w, i32 h, i32 cx, i32 cy, i32 cw, i32 ch, u8 v)
{
    i32 x1 = x + w, y1 = y + h;
    if (x < cx) x = cx;
    if (y < cy) y = cy;
    if (x1 > cx + cw) x1 = cx + cw;
    if (y1 > cy + ch) y1 = cy + ch;
    if (x1 > x && y1 > y) (void)plat_fill_rect(gc, x, y, x1 - x, y1 - y, v, v, v);
}

void wp_pagecanvas_paint(const wp_pagecanvas *c, wp_document *doc, plat_gc *gc,
                         i32 win_x, i32 win_y, i32 clip_x, i32 clip_y, i32 clip_w, i32 clip_h)
{
    u32 p;
    wp_page_device dev;

    /* The desk. */
    (void)plat_fill_shade(gc, clip_x, clip_y, clip_w, clip_h, 50);
    memset(&dev, 0, sizeof(dev));
    dev.gc = gc;
    dev.x_px = c->x_px;
    dev.y_px = c->y_px;
    for (p = 0; p < c->page_count; p++) {
        const wp_section *sec = &doc->sections[c->pages[p].section];
        i32 px = 0, py = 0, pw = 0, ph = 0, ml, mt, mr, mb, m = 6;

        (void)wp_pagecanvas_page_rect(c, p, &px, &py, &pw, &ph);
        px += win_x - c->scroll_x;
        py += win_y - c->scroll_y;
        if (py > clip_y + clip_h) break;
        if (py + ph + 2 < clip_y) continue;
        /* The paper, its edge and its shadow. */
        fill(gc, px, py, pw, ph, clip_x, clip_y, clip_w, clip_h, 255);
        fill(gc, px - 1, py - 1, pw + 2, 1, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, px - 1, py + ph, pw + 2, 1, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, px - 1, py, 1, ph, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, px + pw, py, 1, ph, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, px + 2, py + ph + 1, pw, 2, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, px + pw + 1, py + 2, 2, ph, clip_x, clip_y, clip_w, clip_h, 0);
        /* The margins' corners, as crop marks. */
        ml = px + wp_pagecanvas_zx(c, sec->margin_left);
        mt = py + wp_pagecanvas_zy(c, sec->margin_top);
        mr = px + wp_pagecanvas_zx(c, sec->page_width - sec->margin_right);
        mb = py + wp_pagecanvas_zy(c, sec->page_height - sec->margin_bottom);
        fill(gc, ml - m, mt, m, 1, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, ml, mt - m, 1, m, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, mr, mt, m, 1, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, mr, mt - m, 1, m, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, ml - m, mb, m, 1, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, ml, mb, 1, m, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, mr, mb, m, 1, clip_x, clip_y, clip_w, clip_h, 0);
        fill(gc, mr, mb, 1, m, clip_x, clip_y, clip_w, clip_h, 0);
        /* The grid: a dot at each crossing inside the margins, from their
           corner, where a dot is at least three pixels from the next. */
        if (c->grid_on && c->grid_twips > 0 &&
            wp_pagecanvas_zx(c, c->grid_twips) >= 3 && wp_pagecanvas_zy(c, c->grid_twips) >= 3) {
            i32 gx, gy;
            i32 tw = sec->page_width - sec->margin_left - sec->margin_right;
            i32 th = sec->page_height - sec->margin_top - sec->margin_bottom;
            for (gy = 0; gy <= th; gy += c->grid_twips) {
                i32 dy = mt + wp_pagecanvas_zy(c, gy);
                if (dy < clip_y || dy >= clip_y + clip_h) continue;
                for (gx = 0; gx <= tw; gx += c->grid_twips) {
                    i32 dx = ml + wp_pagecanvas_zx(c, gx);
                    if (dx >= clip_x && dx < clip_x + clip_w) (void)plat_fill_rect(gc, dx, dy, 1, 1, 0, 0, 0);
                }
            }
        }
        /* The page itself, as it prints. */
        (void)wp_pageprint_page(doc, &dev, p, px, py, c->zoom, clip_y, clip_y + clip_h);
    }
}

wp_bool wp_pagecanvas_hit(const wp_pagecanvas *c, wp_document *doc, i32 cx, i32 cy, wp_pos *out)
{
    u32 p = 0, si, pi, li, bsi = 0, bpi = 0, bli = 0;
    i32 px = 0, py = 0, pw = 0, ph = 0, x, y, top, best = 0x7FFFFFFF;
    const wp_line *line, *bline = NULL;
    page_iter it;
    wp_section *sec;
    wp_paragraph *para;
    u32 run = 0, offset = 0;

    if (c->page_count == 0) return WP_FALSE;
    /* The page under the click, or the last above it. */
    while (p + 1 < c->page_count && cy >= c->pages[p + 1].top) p++;
    (void)wp_pagecanvas_page_rect(c, p, &px, &py, &pw, &ph);
    x = wp_pagecanvas_tx(c, cx - px);
    y = wp_pagecanvas_ty(c, cy - py);
    iter_start(&it, c, doc, p);
    while (iter_next(&it, &line, &si, &pi, &li, &top)) {
        const wp_section *s = &doc->sections[si];
        i32 x0 = s->margin_left + line->cell_x0;
        i32 x1 = line->cell_x1 != 0 ? s->margin_left + line->cell_x1 : s->page_width - s->margin_right;
        i32 d = y < top ? top - y : y >= top + line->height_twips ? y - (top + line->height_twips) + 1 : 0;
        /* Beside a cell or a frame is further than over it. */
        if (x < x0) d += (x0 - x) / 4;
        else if (x > x1) d += (x - x1) / 4;
        if (d < best) { best = d; bline = line; bsi = si; bpi = pi; bli = li; }
    }
    if (bline == NULL) return WP_FALSE;
    sec = wp_document_section_get(doc, bsi);
    para = wp_section_paragraph_get(sec, bpi);
    {
        i32 lx = x - line_left(sec, bline);
        (void)wp_line_pos_at_x_twips(doc, para, bline, lx > 0 ? lx : 0, line_extra(doc, para, bline, bli),
                                     &run, &offset);
    }
    memset(out, 0, sizeof(*out));
    out->section = bsi;
    out->paragraph = bpi;
    out->run = run;
    out->offset = offset;
    return WP_TRUE;
}

/* The line a position is on (the last for one past the end, as the
   painters have it). */
static wp_bool line_of(wp_document *doc, const wp_pos *pos, u32 *li_out)
{
    wp_section *sec;
    const wp_paragraph_layout *l;
    u32 li;

    if (pos->section >= doc->section_count) return WP_FALSE;
    sec = &doc->sections[pos->section];
    if (pos->paragraph >= sec->para_count) return WP_FALSE;
    l = (const wp_paragraph_layout *)sec->paras[pos->paragraph].layout;
    if (l == NULL || l->line_count == 0) return WP_FALSE;
    *li_out = l->line_count - 1;
    for (li = 0; li < l->line_count; li++) {
        const wp_line *ln = &l->lines[li];
        if (pos->run < ln->start_run || pos->run > ln->end_run) continue;
        if (pos->run == ln->start_run && pos->offset < ln->start_offset) continue;
        if (pos->run == ln->end_run && pos->offset > ln->end_offset) continue;
        *li_out = li;
        break;
    }
    return WP_TRUE;
}

/* The top (twips, on the paper) of line li of paragraph (si, pi). */
static wp_bool line_top(const wp_pagecanvas *c, wp_document *doc, u32 si, u32 pi, u32 li, i32 *top)
{
    const wp_paragraph_layout *l = (const wp_paragraph_layout *)doc->sections[si].paras[pi].layout;
    u32 page = l->lines[li].page, a, b, d;
    const wp_line *line;
    page_iter it;

    if (page >= c->page_count) return WP_FALSE;
    iter_start(&it, c, doc, page);
    while (iter_next(&it, &line, &a, &b, &d, top)) {
        if (a == si && b == pi && d == li) return WP_TRUE;
    }
    return WP_FALSE;
}

wp_bool wp_pagecanvas_caret(const wp_pagecanvas *c, wp_document *doc, const wp_pos *pos,
                            i32 *cx, i32 *cy, i32 *ch)
{
    u32 li;
    i32 top, px = 0, py = 0, pw = 0, ph = 0, x;
    wp_section *sec;
    wp_paragraph *para;
    const wp_line *line;

    if (!line_of(doc, pos, &li) || !line_top(c, doc, pos->section, pos->paragraph, li, &top)) return WP_FALSE;
    sec = wp_document_section_get(doc, pos->section);
    para = wp_section_paragraph_get(sec, pos->paragraph);
    line = &((const wp_paragraph_layout *)para->layout)->lines[li];
    (void)wp_pagecanvas_page_rect(c, line->page, &px, &py, &pw, &ph);
    x = line_left(sec, line) +
        wp_line_cursor_x_twips(doc, para, line, pos->run, pos->offset, line_extra(doc, para, line, li));
    *cx = px + wp_pagecanvas_zx(c, x);
    *cy = py + wp_pagecanvas_zy(c, top + line->text_top_twips);
    *ch = wp_pagecanvas_zy(c, line->text_height_twips);
    if (*ch < 1) *ch = 1;
    return WP_TRUE;
}

void wp_pagecanvas_selection(const wp_pagecanvas *c, wp_document *doc, wp_selection sel,
                             wp_pagecanvas_rect_fn fn, void *user)
{
    u32 p;

    for (p = 0; p < c->page_count; p++) {
        i32 px = 0, py = 0, pw = 0, ph = 0, top;
        u32 si, pi, li;
        const wp_line *line;
        page_iter it;

        (void)wp_pagecanvas_page_rect(c, p, &px, &py, &pw, &ph);
        if (py > c->scroll_y + c->view_h || py + ph < c->scroll_y) continue;   /* out of view */
        iter_start(&it, c, doc, p);
        while (iter_next(&it, &line, &si, &pi, &li, &top)) {
            wp_pos from, to;
            wp_section *sec;
            wp_paragraph *para;
            i32 extra, x0, x1;

            if (!wp_selspan_for_line(sel, si, pi, line, &from, &to)) continue;
            sec = wp_document_section_get(doc, si);
            para = wp_section_paragraph_get(sec, pi);
            extra = line_extra(doc, para, line, li);
            x0 = line_left(sec, line) + wp_line_cursor_x_twips(doc, para, line, from.run, from.offset, extra);
            x1 = line_left(sec, line) + wp_line_cursor_x_twips(doc, para, line, to.run, to.offset, extra);
            x0 = px + wp_pagecanvas_zx(c, x0);
            x1 = px + wp_pagecanvas_zx(c, x1);
            if (x1 > x0) {
                fn(user, x0, py + wp_pagecanvas_zy(c, top + line->text_top_twips), x1 - x0,
                   wp_pagecanvas_zy(c, line->text_height_twips));
            }
        }
    }
}

void wp_pagecanvas_scroll_by(wp_pagecanvas *c, i32 dx, i32 dy)
{
    c->scroll_x += dx;
    c->scroll_y += dy;
    clamp_scroll(c);
}

void wp_pagecanvas_scroll_to(wp_pagecanvas *c, i32 x, i32 y)
{
    c->scroll_x = x;
    c->scroll_y = y;
    clamp_scroll(c);
}

wp_bool wp_pagecanvas_follow(wp_pagecanvas *c, wp_document *doc, const wp_pos *pos)
{
    i32 cx, cy, ch, ox = c->scroll_x, oy = c->scroll_y, m = 8;

    if (!wp_pagecanvas_caret(c, doc, pos, &cx, &cy, &ch)) return WP_FALSE;
    if (cy < c->scroll_y) c->scroll_y = cy - m;
    else if (cy + ch > c->scroll_y + c->view_h) c->scroll_y = cy + ch - c->view_h + m;
    if (cx < c->scroll_x) c->scroll_x = cx - m;
    else if (cx + 2 > c->scroll_x + c->view_w) c->scroll_x = cx + 2 - c->view_w + m;
    clamp_scroll(c);
    return (wp_bool)(c->scroll_x != ox || c->scroll_y != oy);
}

i32 wp_pagecanvas_snap(const wp_pagecanvas *c, i32 twips)
{
    i32 g = c->grid_twips;
    if (!c->grid_on || !c->snap_on || g <= 0) return twips;
    return twips >= 0 ? ((twips + g / 2) / g) * g : -(((-twips) + g / 2) / g) * g;
}
