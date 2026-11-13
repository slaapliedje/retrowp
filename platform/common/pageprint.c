/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "devout.h"
#include "drawpaint.h"
#include "drawing.h"
#include "pageprint.h"
#include "pageview.h"
#include "layout.h"
#include "strpool.h"
#include "paraborder.h"

static void mark_all_dirty(wp_document *doc)
{
    u32 si, pi;

    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        for (pi = 0; pi < sec->para_count; pi++) {
            wp_section_paragraph_get(sec, pi)->layout_dirty = WP_TRUE;
        }
    }
}

/* Where a paper position lands on the device: the paper centred on the
   printable area (pageprint.h). */
typedef struct {
    const wp_page_device *dev;
    i32 off_x, off_y;   /* device pixels from the paper's corner to the printable area's */
    i32 org_x, org_y;   /* where the paper's corner is on the device (a page on a screen) */
    i32 zoom;           /* percent: a screen's page view at its zoom; 100 on paper */
    wp_bool screen;     /* a screen's page view (wp_pageprint_page): text in its colours */
    i32 clip_y0, clip_y1;   /* a screen's: only what lies wholly inside is drawn */
} page_frame;

/* The zoom in force, for the conversions handed to the border and
   drawing painters, which take a bare twips->pixels function. One page
   is drawn at a time, so one zoom at a time. */
static const wp_page_device *g_zdev;
static i32 g_zoom = 100;
static i32 zx(i32 twips) { return g_zdev->x_px(g_zoom == 100 ? twips : (i32)(((long)twips * g_zoom) / 100L)); }
static i32 zy(i32 twips) { return g_zdev->y_px(g_zoom == 100 ? twips : (i32)(((long)twips * g_zoom) / 100L)); }

/* A face at the zoom: the style's size scaled, so a word drawn in it is
   about as wide on screen as the layout made it. Its measures, in
   twips at that size, come back through unzoom. */
static wp_status open_zoomed(wp_document *doc, const wp_char_style *cs, plat_font **out)
{
    wp_char_style z;
    if (g_zoom == 100) return wp_cstyle_font_open(doc, cs, out);
    z = *cs;
    z.size_twips = (i32)(((long)cs->size_twips * g_zoom) / 100L);
    if (z.size_twips < 20) z.size_twips = 20;
    return wp_cstyle_font_open(doc, &z, out);
}
static i32 unzoom(i32 twips) { return g_zoom == 100 ? twips : (i32)(((long)twips * 100L) / g_zoom); }

/* At a zoom, where a word goes is measured in the face the layout
   measured it in, the style's own size: a screen without a face as small
   as the zoom asks opens a bigger one, and words advanced by its widths
   would run past their column. The zoomed face only draws. One page at
   a time, so one cached face. */
static plat_font *g_base_font;
static wp_cstyle g_base_style;

static i32 base_width(wp_document *doc, wp_cstyle style, const u8 *bytes, u32 len)
{
    i32 w = 0;
    if (g_base_font == NULL || g_base_style != style) {
        const wp_char_style *cs = wp_document_cstyle_get(doc, style);
        if (g_base_font != NULL) plat_font_close(g_base_font);
        g_base_font = NULL;
        if (cs == NULL || wp_cstyle_font_open(doc, cs, &g_base_font) != WP_OK) { g_base_font = NULL; return 0; }
        g_base_style = style;
    }
    (void)plat_measure_text(g_base_font, bytes, len, &w);
    return w;
}

static void frame_for(page_frame *fr, const wp_page_device *dev, const wp_section *sec)
{
    fr->dev = dev;
    fr->org_x = fr->org_y = 0;
    fr->zoom = 100;
    fr->screen = WP_FALSE;
    fr->clip_y0 = -32000;
    fr->clip_y1 = 32000;
    g_zdev = dev;
    g_zoom = 100;
    fr->off_x = 0;
    fr->off_y = 0;
    if (dev->device_w_px > 0) {
        i32 d = dev->x_px(sec->page_width) - dev->device_w_px;
        if (d > 0) fr->off_x = d / 2;
    }
    if (dev->device_h_px > 0) {
        i32 d = dev->y_px(sec->page_height) - dev->device_h_px;
        if (d > 0) fr->off_y = d / 2;
    }
}

static i32 at_x(const page_frame *fr, i32 twips) { return fr->org_x + zx(twips) - fr->off_x; }
static i32 at_y(const page_frame *fr, i32 twips) { return fr->org_y + zy(twips) - fr->off_y; }

/* One '|' part of a header or footer, '#' replaced by the page number. */
static u32 header_part(const u8 *text, u32 len, u32 *pos, wp_bool last, u32 page_no,
                       u8 *out, u32 cap)
{
    u32 n = 0;

    while (*pos < len) {
        u8 c = text[(*pos)++];
        if (c == '|' && !last) break;
        if (c == '#') {
            char digits[12];
            u32 d = 0, v = page_no;
            do { digits[d++] = (char)('0' + v % 10u); v /= 10u; } while (v > 0 && d < 12);
            while (d > 0 && n < cap) out[n++] = (u8)digits[--d];
            continue;
        }
        if (n < cap) out[n++] = c;
    }
    return n;
}

/* The header or footer, its text centred on the margin's middle line. */
static void draw_margin_text(wp_document *doc, const page_frame *fr, const wp_section *sec,
                             wp_span span, i32 mid_twips, u32 page_no)
{
    const u8 *text;
    const wp_char_style *cs;
    plat_font *font = NULL;
    plat_font_metrics fm;
    u32 parts = 1, i, pos = 0;
    i32 left, right, baseline;

    if (span.length == 0) return;
    text = wp_strpool_get(&doc->strings, span);
    cs = wp_document_cstyle_get(doc, doc->default_cstyle);
    if (text == NULL || cs == NULL) return;
    for (i = 0; i < span.length && parts < 3; i++) if (text[i] == '|') parts++;
    if (open_zoomed(doc, cs, &font) != WP_OK) return;
    if (plat_font_metrics_get(font, &fm) != WP_OK) { plat_font_close(font); return; }
    fm.ascent = unzoom(fm.ascent);
    fm.descent = unzoom(fm.descent);

    left = sec->margin_left;
    right = sec->page_width - sec->margin_right;
    baseline = mid_twips - (fm.ascent + fm.descent) / 2 + fm.ascent;
    if (at_y(fr, baseline - fm.ascent) < fr->clip_y0 || at_y(fr, baseline + fm.descent) > fr->clip_y1) {
        plat_font_close(font);   /* off a screen's view */
        return;
    }
    for (i = 0; i < parts; i++) {
        u8 buf[256];
        u32 n = header_part(text, span.length, &pos, (wp_bool)(i == 2), page_no, buf, sizeof(buf));
        i32 w = 0, x;
        /* One part is centred; two are left and right; three left, centre, right. */
        u32 where = (parts == 1) ? 1u : (parts == 2 && i == 1) ? 2u : i;

        if (n == 0) continue;
        (void)plat_measure_text(font, buf, n, &w);
        w = unzoom(w);
        x = (where == 0) ? left : (where == 2) ? right - w : left + (right - left - w) / 2;
        (void)wp_dev_text(fr->dev->gc, font, doc, cs, at_x(fr, x), at_y(fr, baseline),
                          at_x(fr, x + w) - at_x(fr, x),
                          at_y(fr, baseline + fm.descent) - at_y(fr, baseline - fm.ascent),
                          buf, n, 0, 0, 0);
    }
    plat_font_close(font);
}

static void begin_page(wp_document *doc, const page_frame *fr, const wp_section *sec, u32 page_no)
{
    draw_margin_text(doc, fr, sec, sec->header, sec->margin_top / 2, page_no);
    draw_margin_text(doc, fr, sec, sec->footer, sec->page_height - sec->margin_bottom / 2, page_no);
}

/* A picture's frame, one device pixel wide. */
static void draw_frame(const page_frame *fr, i32 x, i32 y, i32 w, i32 h)
{
    i32 x0 = at_x(fr, x), y0 = at_y(fr, y);
    i32 x1 = at_x(fr, x + w), y1 = at_y(fr, y + h);
    plat_gc *gc = fr->dev->gc;

    (void)wp_dev_fill_rect(gc, x0, y0, x1 - x0, 1, 0, 0, 0);
    (void)wp_dev_fill_rect(gc, x0, y1 - 1, x1 - x0, 1, 0, 0, 0);
    (void)wp_dev_fill_rect(gc, x0, y0, 1, y1 - y0, 0, 0, 0);
    (void)wp_dev_fill_rect(gc, x1 - 1, y0, 1, y1 - y0, 0, 0, 0);
}

/* Rows of a picture drawn at a time: a band's worth of bits is all a
   picture costs, so a full-page one prints on a 1 MB ST. */
#define PRINT_BAND_ROWS 32

/* An ordered dither: a pixel prints black when its brightness is under
   its cell's threshold, so greys come out as even dot patterns. */
static const u8 g_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/* A picture's pixels, scaled to its box at the device's resolution and
   dithered to black and white -- a printer has two colours, and a
   screen's palette pens are not a printer's -- drawn a band of rows at
   a time through plat_draw_mono. WP_FALSE (nothing drawn) when its
   pixels cannot be read or a buffer cannot be had. */
static wp_bool draw_picture(wp_document *doc, const page_frame *fr, const wp_image_desc *img,
                            i32 x, i32 y, i32 w, i32 h)
{
    char path[256];
    const u8 *path_bytes;
    plat_file *f;
    u32 row_bytes, size, got = 0, row_words;
    u8 *pixels;
    u16 *band, *src_x;
    u8 luma[16];
    i32 x0 = at_x(fr, x), y0 = at_y(fr, y);
    i32 dw = at_x(fr, x + w) - x0, dh = at_y(fr, y + h) - y0;
    i32 dx, by;
    u32 i;

    if (img->blob_path.length == 0 || img->blob_path.length >= sizeof(path)) return WP_FALSE;
    if (img->width_px <= 0 || img->height_px <= 0 || dw <= 0 || dh <= 0) return WP_FALSE;
    path_bytes = wp_strpool_get(&doc->strings, img->blob_path);
    if (path_bytes == NULL) return WP_FALSE;
    memcpy(path, path_bytes, img->blob_path.length);
    path[img->blob_path.length] = '\0';

    row_bytes = ((u32)img->width_px + 1u) / 2u;
    size = row_bytes * (u32)img->height_px;
    if (plat_file_open(path, PLAT_FILE_READ, &f) != WP_OK) return WP_FALSE;
    pixels = (u8 *)mem_alloc(size);
    if (pixels == NULL || plat_file_read(f, pixels, size, &got) != WP_OK || got != size) {
        plat_file_close(f);
        if (pixels != NULL) mem_free(pixels);
        return WP_FALSE;
    }
    plat_file_close(f);

    row_words = ((u32)dw + 15u) / 16u;
    band = (u16 *)mem_alloc(row_words * PRINT_BAND_ROWS * (u32)sizeof(u16));
    src_x = (u16 *)mem_alloc((u32)dw * (u32)sizeof(u16));
    if (band == NULL || src_x == NULL) {
        if (band != NULL) mem_free(band);
        if (src_x != NULL) mem_free(src_x);
        mem_free(pixels);
        return WP_FALSE;
    }
    for (dx = 0; dx < dw; dx++) src_x[dx] = (u16)(((u32)dx * (u32)img->width_px) / (u32)dw);
    for (i = 0; i < 16; i++) {
        const u8 *c = img->palette[i < img->palette_count ? i : 0];
        luma[i] = (u8)(((u32)c[0] * 299u + (u32)c[1] * 587u + (u32)c[2] * 114u) / 1000u);
    }

    for (by = 0; by < dh; by += PRINT_BAND_ROWS) {
        i32 rows = (dh - by < PRINT_BAND_ROWS) ? dh - by : PRINT_BAND_ROWS, r;

        memset(band, 0, row_words * (u32)rows * (u32)sizeof(u16));
        for (r = 0; r < rows; r++) {
            i32 dy = by + r;
            const u8 *src = pixels + (((u32)dy * (u32)img->height_px) / (u32)dh) * row_bytes;
            u16 *out = band + (u32)r * row_words;
            const u8 *thr = g_bayer[dy & 3];

            for (dx = 0; dx < dw; dx++) {
                u16 sx = src_x[dx];
                u8 nib = (u8)((sx & 1u) ? (src[sx >> 1] & 0x0F) : (src[sx >> 1] >> 4));
                if (luma[nib] < (u8)(thr[dx & 3] * 16u + 8u)) {
                    out[dx >> 4] = (u16)(out[dx >> 4] | (0x8000u >> (dx & 15)));
                }
            }
        }
        if (fr->dev->draw_mono != NULL) {
            (void)fr->dev->draw_mono(fr->dev->gc, x0, y0 + by, dw, rows, band, row_words, 0, 0, 0);
        } else {
            (void)wp_dev_draw_mono(fr->dev->gc, x0, y0 + by, dw, rows, band, row_words, 0, 0, 0);
        }
    }
    mem_free(src_x);
    mem_free(band);
    mem_free(pixels);
    return WP_TRUE;
}

/* One line's text, the fragment walk every painter makes: a tab moves
   to its stop, a space widens by justify's extra, a word is drawn at
   its baseline. */
static void draw_line(wp_document *doc, const page_frame *fr, wp_paragraph *para,
                      const wp_line *line, wp_bool last_line, i32 left_twips, i32 top_twips,
                      plat_font **font, wp_cstyle *cached, wp_bool *have_font, i32 *ascent,
                      i32 *descent)
{
    const wp_para_style *pstyle = wp_document_pstyle_get(doc, para->style);
    u8 align = (pstyle != NULL) ? pstyle->align : WP_ALIGN_LEFT;
    i32 x = left_twips + line->x_twips;
    i32 text_top = top_twips + line->text_top_twips;
    i32 extra = wp_justify_space_extra_twips(doc, para, line, align, last_line, line->avail_twips);
    wp_line_fragment_iter iter;
    wp_line_fragment frag;

    wp_line_fragment_iter_init(&iter, line);
    while (wp_line_next_fragment(doc, para, line, &iter, &frag)) {
        wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);
        u32 len = frag.byte_end - frag.byte_start;
        const u8 *bytes;
        i32 w = 0;

        if (frag.is_tab) {
            x += frag.tab_twips;
            continue;
        }
        if (run == NULL) continue;
        bytes = wp_strpool_get(&doc->strings, run->text);
        if (bytes == NULL) continue;
        if (!*have_font || run->style != *cached) {
            const wp_char_style *cs = wp_document_cstyle_get(doc, run->style);
            plat_font_metrics fm;

            if (*font != NULL) {
                plat_font_close(*font);
                *font = NULL;
            }
            *have_font = WP_FALSE;
            if (cs != NULL && open_zoomed(doc, cs, font) == WP_OK) {
                /* On a screen, how big the drawing face really is -- with
                   paper metrics on (papermetrics.h) every face answers in
                   paper sizes, and greeking needs the glyphs' own. */
                wp_bool paper = plat_metrics_paper();
                wp_status mst;
                if (fr->screen) plat_metrics_set_paper(WP_FALSE);
                mst = plat_font_metrics_get(*font, &fm);
                if (fr->screen) plat_metrics_set_paper(paper);
                if (mst == WP_OK) {
                    *ascent = unzoom(fm.ascent);
                    *descent = unzoom(fm.descent);
                    *cached = run->style;
                    *have_font = WP_TRUE;
                } else {
                    plat_font_close(*font);
                    *font = NULL;
                }
            }
        }
        if (len == 0 || *font == NULL) continue;
        if (g_zoom != 100) {
            w = base_width(doc, run->style, bytes + frag.byte_start, len);
        } else {
            (void)plat_measure_text(*font, bytes + frag.byte_start, len, &w);
        }
        if (fr->screen && *ascent + *descent > (line->text_height_twips * 5) / 4) {
            /* Greeked: the screen has no face this small (the ST's own
               bitmap fonts stop at a size a page fitted to the window
               is well under), so a word that would overrun its line is
               a grey bar its laid-out width -- what DTP programs show
               for text too small to read. Paper is never greeked. */
            if (!frag.is_space) {
                i32 by = text_top + (line->text_height_twips * 2) / 5;
                i32 bh = at_y(fr, by + line->text_height_twips / 3) - at_y(fr, by);
                (void)wp_dev_fill_shade(fr->dev->gc, at_x(fr, x), at_y(fr, by), at_x(fr, x + w) - at_x(fr, x),
                                        bh > 0 ? bh : 1, 50);
            }
            x += w;
            if (frag.is_space) x += extra;
            continue;
        }
        if (!frag.is_space) {
            /* Black on paper (a mono printer would lose light text); the
               text's own colour on a screen's page view. */
            const wp_char_style *tcs = wp_document_cstyle_get(doc, run->style);
            u8 r = 0, g = 0, b = 0;
            if (fr->screen && tcs != NULL) { r = tcs->color_r; g = tcs->color_g; b = tcs->color_b; }
            (void)wp_dev_text(fr->dev->gc, *font, doc, tcs,
                              at_x(fr, x), at_y(fr, text_top + *ascent), at_x(fr, x + w) - at_x(fr, x),
                              at_y(fr, text_top + *ascent + *descent) - at_y(fr, text_top),
                              bytes + frag.byte_start, len, r, g, b);
        }
        x += w;
        if (frag.is_space) x += extra;
    }
}

/* One line of page drawn: its row's rules, its border and shading, its
   picture or its text. Twips here; the device's conversion (and a
   screen's zoom) comes after. A table line sits at its row's top plus
   its offset, its cell's left edge in from the column's. */
static void draw_one(wp_document *doc, const page_frame *fr, wp_section *sec, u32 pi,
                     wp_paragraph *para, wp_paragraph_layout *layout, u32 li, i32 y,
                     plat_font **font, wp_cstyle *cached, wp_bool *have_font, i32 *ascent, i32 *descent)
{
    const wp_line *line = &layout->lines[li];
    i32 left = sec->margin_left + line->col_x_twips + line->cell_x0;   /* its column's, its cell's */

    if (line->flags & WP_LINE_ROW_FIRST) {
        wp_border_draw_row(fr->dev->gc, wp_document_trow_get(doc, para->trow), line->row_h_twips,
                           (wp_bool)(pi > 0 && sec->paras[pi - 1].trow != 0 && !line->page_start),
                           at_x(fr, sec->margin_left + line->col_x_twips), at_y(fr, y),
                           zx, zy, -32000, 32000);
    }
    {   /* the paragraph's border and shading, under its text */
        wp_border_slice bs;
        if (wp_line_border_slice(doc, para, line, (wp_bool)(li == 0),
                                 (wp_bool)(li == layout->line_count - 1),
                                 layout->wrap_twips, &bs)) {
            wp_border_draw_slice(fr->dev->gc, &bs, at_x(fr, left), at_y(fr, y),
                                 zx, zy, -32000, 32000);
        }
    }
    if (para->kind == (u8)WP_PARA_IMAGE) {
        const wp_image_desc *img = wp_document_image_get(doc, para->image);
        i32 px = left + line->x_twips, py = y + line->text_top_twips;

        /* A drawing at the printer's own resolution; a bitmap
           dithered to it; an outline where neither can be. */
        if (img != NULL && WP_IMAGE_IS_DRAWING(img)) {
            (void)wp_paint_drawing(fr->dev->gc, doc, img, at_x(fr, px), at_y(fr, py),
                                   at_x(fr, px + line->width_twips) - at_x(fr, px),
                                   at_y(fr, py + line->text_height_twips) - at_y(fr, py));
        } else if (img == NULL ||
            !draw_picture(doc, fr, img, px, py, line->width_twips,
                          line->text_height_twips)) {
            draw_frame(fr, px, py, line->width_twips, line->text_height_twips);
        }
    } else {
        draw_line(doc, fr, para, line, (wp_bool)(li == layout->line_count - 1),
                  left, y, font, cached, have_font, ascent, descent);
    }
}

static i32 same_twips(i32 twips) { return twips; }

wp_status wp_pageprint(wp_document *doc, const wp_page_device *dev, u32 *pages_out)
{
    u32 si, pi, li;
    u32 cur_page = 0, pages = 0;
    i32 y = 0;
    wp_line_walk walk;
    page_frame fr;
    plat_font *font = NULL;
    wp_cstyle cached = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent = 0, descent = 0;
    wp_status st = WP_OK;

    if (pages_out != NULL) *pages_out = 0;
    if (doc == NULL || dev == NULL || dev->gc == NULL) return WP_ERR;

    mark_all_dirty(doc);
    (void)wp_pageview_relayout(doc, 0);

    for (si = 0; si < doc->section_count && st == WP_OK; si++) {
        wp_section *sec = wp_document_section_get(doc, si);

        for (pi = 0; pi < sec->para_count && st == WP_OK; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;

            if (layout == NULL) continue;
            for (li = 0; li < layout->line_count; li++) {
                const wp_line *line = &layout->lines[li];

                if (pages == 0 || line->page != cur_page) {
                    if (pages > 0 && dev->end_page != NULL) {
                        st = dev->end_page(dev->user);
                        if (st != WP_OK) break;
                    }
                    pages++;
                    cur_page = line->page;
                    frame_for(&fr, dev, sec);
                    if (dev->begin_page != NULL) {
                        st = dev->begin_page(dev->user, sec, pages);
                        if (st != WP_OK) break;
                    }
                    begin_page(doc, &fr, sec, pages);
                    wp_line_walk_start(&walk, sec->margin_top);
                    walk.paper = WP_TRUE;
                }
                y = wp_line_walk_next(&walk, line, same_twips);
                draw_one(doc, &fr, sec, pi, para, layout, li, y, &font, &cached, &have_font, &ascent, &descent);
            }
        }
    }
    if (font != NULL) plat_font_close(font);
    if (st == WP_OK && pages > 0 && dev->end_page != NULL) st = dev->end_page(dev->user);

    mark_all_dirty(doc);
    if (pages_out != NULL) *pages_out = pages;
    return st;
}

wp_status wp_pageprint_page(wp_document *doc, const wp_page_device *dev, u32 page,
                            i32 x, i32 y0, i32 zoom, i32 clip_y0, i32 clip_y1)
{
    u32 si, pi, li;
    wp_bool started = WP_FALSE;
    wp_line_walk walk;
    page_frame fr;
    plat_font *font = NULL;
    wp_cstyle cached = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent = 0, descent = 0;

    if (doc == NULL || dev == NULL || dev->gc == NULL || zoom <= 0) return WP_ERR;
    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;

            if (layout == NULL) continue;
            for (li = 0; li < layout->line_count; li++) {
                const wp_line *line = &layout->lines[li];

                /* A page's lines are together in reading order: a frame's
                   and a note's carry the page of the text before them. */
                if (line->page < page) continue;
                if (line->page > page) goto done;
                if (!started) {
                    started = WP_TRUE;
                    frame_for(&fr, dev, sec);
                    fr.org_x = x;
                    fr.org_y = y0;
                    fr.zoom = zoom;
                    fr.screen = WP_TRUE;
                    fr.clip_y0 = clip_y0;
                    fr.clip_y1 = clip_y1;
                    g_zoom = zoom;
                    begin_page(doc, &fr, sec, page + 1);
                    wp_line_walk_start(&walk, sec->margin_top);
                    walk.paper = WP_TRUE;
                }
                {
                    /* Walked whatever; drawn only where it lies wholly
                       inside the view (a toolkit that gives no clip --
                       MUI's -- would have it over the status line). A
                       row is the height of its rule, its tallest cell. */
                    i32 ly = wp_line_walk_next(&walk, line, same_twips);
                    i32 lh = (line->flags & WP_LINE_ROW_FIRST) ? line->row_h_twips : line->height_twips;
                    if (at_y(&fr, ly) >= clip_y0 && at_y(&fr, ly + lh) <= clip_y1) {
                        draw_one(doc, &fr, sec, pi, para, layout, li, ly,
                                 &font, &cached, &have_font, &ascent, &descent);
                    }
                }
            }
        }
    }
done:
    if (font != NULL) plat_font_close(font);
    if (g_base_font != NULL) plat_font_close(g_base_font);
    g_base_font = NULL;
    g_zoom = 100;
    return WP_OK;
}
