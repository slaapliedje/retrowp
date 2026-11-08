/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "pageprint.h"
#include "pageview.h"
#include "layout.h"
#include "strpool.h"

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
} page_frame;

static void frame_for(page_frame *fr, const wp_page_device *dev, const wp_section *sec)
{
    fr->dev = dev;
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

static i32 at_x(const page_frame *fr, i32 twips) { return fr->dev->x_px(twips) - fr->off_x; }
static i32 at_y(const page_frame *fr, i32 twips) { return fr->dev->y_px(twips) - fr->off_y; }

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
    if (wp_cstyle_font_open(doc, cs, &font) != WP_OK) return;
    if (plat_font_metrics_get(font, &fm) != WP_OK) { plat_font_close(font); return; }

    left = sec->margin_left;
    right = sec->page_width - sec->margin_right;
    baseline = mid_twips - (fm.ascent + fm.descent) / 2 + fm.ascent;
    for (i = 0; i < parts; i++) {
        u8 buf[256];
        u32 n = header_part(text, span.length, &pos, (wp_bool)(i == 2), page_no, buf, sizeof(buf));
        i32 w = 0, x;
        /* One part is centred; two are left and right; three left, centre, right. */
        u32 where = (parts == 1) ? 1u : (parts == 2 && i == 1) ? 2u : i;

        if (n == 0) continue;
        (void)plat_measure_text(font, buf, n, &w);
        x = (where == 0) ? left : (where == 2) ? right - w : left + (right - left - w) / 2;
        (void)plat_draw_text(fr->dev->gc, font, at_x(fr, x), at_y(fr, baseline), buf, n, 0, 0, 0);
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

    (void)plat_fill_rect(gc, x0, y0, x1 - x0, 1, 0, 0, 0);
    (void)plat_fill_rect(gc, x0, y1 - 1, x1 - x0, 1, 0, 0, 0);
    (void)plat_fill_rect(gc, x0, y0, 1, y1 - y0, 0, 0, 0);
    (void)plat_fill_rect(gc, x1 - 1, y0, 1, y1 - y0, 0, 0, 0);
}

/* One line's text, the fragment walk every painter makes: a tab moves
   to its stop, a space widens by justify's extra, a word is drawn at
   its baseline. */
static void draw_line(wp_document *doc, const page_frame *fr, wp_paragraph *para,
                      const wp_line *line, wp_bool last_line, i32 left_twips, i32 top_twips,
                      plat_font **font, wp_cstyle *cached, wp_bool *have_font, i32 *ascent)
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
            if (cs != NULL && wp_cstyle_font_open(doc, cs, font) == WP_OK) {
                if (plat_font_metrics_get(*font, &fm) == WP_OK) {
                    *ascent = fm.ascent;
                    *cached = run->style;
                    *have_font = WP_TRUE;
                } else {
                    plat_font_close(*font);
                    *font = NULL;
                }
            }
        }
        if (len == 0 || *font == NULL) continue;
        (void)plat_measure_text(*font, bytes + frag.byte_start, len, &w);
        if (!frag.is_space) {
            (void)plat_draw_text(fr->dev->gc, *font, at_x(fr, x), at_y(fr, text_top + *ascent),
                                 bytes + frag.byte_start, len, 0, 0, 0);
        }
        x += w;
        if (frag.is_space) x += extra;
    }
}

wp_status wp_pageprint(wp_document *doc, const wp_page_device *dev, u32 *pages_out)
{
    u32 si, pi, li;
    u32 cur_page = 0, pages = 0;
    i32 y = 0;
    page_frame fr;
    plat_font *font = NULL;
    wp_cstyle cached = 0;
    wp_bool have_font = WP_FALSE;
    i32 ascent = 0;
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
                    begin_page(doc, &fr, sec, pages);
                    y = sec->margin_top;
                }
                if (para->kind == (u8)WP_PARA_IMAGE) {
                    draw_frame(&fr, sec->margin_left + line->x_twips, y + line->text_top_twips,
                               line->width_twips, line->text_height_twips);
                } else {
                    draw_line(doc, &fr, para, line, (wp_bool)(li == layout->line_count - 1),
                              sec->margin_left, y, &font, &cached, &have_font, &ascent);
                }
                y += line->height_twips;
            }
        }
    }
    if (font != NULL) plat_font_close(font);
    if (st == WP_OK && pages > 0 && dev->end_page != NULL) st = dev->end_page(dev->user);

    mark_all_dirty(doc);
    if (pages_out != NULL) *pages_out = pages;
    return st;
}
