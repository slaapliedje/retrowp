/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "rulerbar.h"
#include "app_ctx.h"
#include "keymap.h"
#include "layout.h"

typedef enum {
    WP_ICON_NEW, WP_ICON_OPEN, WP_ICON_SAVE, WP_ICON_CUT, WP_ICON_COPY,
    WP_ICON_PASTE, WP_ICON_UNDO, WP_ICON_BOLD, WP_ICON_ITALIC,
    WP_ICON_UNDERLINE, WP_ICON_LEFT, WP_ICON_CENTER, WP_ICON_RIGHT,
    WP_ICON_JUSTIFY, WP_ICON_SINGLE, WP_ICON_ONEHALF, WP_ICON_DOUBLE,
    WP_ICON_INDENT_LESS, WP_ICON_INDENT_MORE,
    WP_ICON_COUNT
} wp_icon_id;

/* GENERATED from scratch icons.txt (16x16, bit 15 = leftmost pixel); one row a u16. */
static const u16 g_icon_bits[WP_ICON_COUNT][16] = {
    /* NEW */ { 0x0000, 0x1FC0, 0x1060, 0x1050, 0x1078, 0x1008, 0x1008, 0x1008, 0x1008, 0x1008, 0x1008, 0x1008, 0x1008, 0x1FF8, 0x0000, 0x0000 },
    /* OPEN */ { 0x0000, 0x0000, 0x3C00, 0x4200, 0x41F8, 0x4008, 0x47FC, 0x4804, 0x4808, 0x5008, 0x5010, 0x6010, 0x7FE0, 0x0000, 0x0000, 0x0000 },
    /* SAVE */ { 0x0000, 0x7FF8, 0x502C, 0x5024, 0x5024, 0x5FE4, 0x4004, 0x4FE4, 0x4824, 0x4B24, 0x4B24, 0x4824, 0x7FFC, 0x0000, 0x0000, 0x0000 },
    /* CUT */ { 0x0000, 0x1010, 0x1010, 0x0820, 0x0820, 0x0440, 0x0280, 0x0100, 0x0280, 0x3EF8, 0x4284, 0x4284, 0x4284, 0x3C78, 0x0000, 0x0000 },
    /* COPY */ { 0x0000, 0x7E00, 0x4300, 0x4100, 0x4FC0, 0x4860, 0x4820, 0x4820, 0x7820, 0x0820, 0x0820, 0x0FE0, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* PASTE */ { 0x0000, 0x0780, 0x3870, 0x2790, 0x2010, 0x27E0, 0x2430, 0x2410, 0x2410, 0x2410, 0x2410, 0x3C10, 0x07F0, 0x0000, 0x0000, 0x0000 },
    /* UNDO */ { 0x0000, 0x0000, 0x0800, 0x1800, 0x3F80, 0x7FE0, 0x3F90, 0x1808, 0x0808, 0x0008, 0x0010, 0x07E0, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* BOLD */ { 0x0000, 0x0000, 0x3FC0, 0x38E0, 0x38E0, 0x38E0, 0x3F80, 0x38E0, 0x3870, 0x3870, 0x38E0, 0x3FE0, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* ITALIC */ { 0x0000, 0x0000, 0x01F0, 0x0060, 0x00C0, 0x00C0, 0x0180, 0x0180, 0x0300, 0x0300, 0x0600, 0x0F80, 0x0000, 0x0000, 0x0000, 0x0000 },
    /* UNDERLINE */ { 0x0000, 0x0000, 0x1830, 0x1830, 0x1830, 0x1830, 0x1830, 0x1830, 0x1830, 0x0C60, 0x07C0, 0x0000, 0x3FF8, 0x0000, 0x0000, 0x0000 },
    /* LEFT */ { 0x0000, 0x0000, 0x7FF8, 0x0000, 0x7F80, 0x0000, 0x7FF8, 0x0000, 0x7F80, 0x0000, 0x7FF8, 0x0000, 0x7F80, 0x0000, 0x0000, 0x0000 },
    /* CENTER */ { 0x0000, 0x0000, 0x7FF8, 0x0000, 0x1FE0, 0x0000, 0x7FF8, 0x0000, 0x1FE0, 0x0000, 0x7FF8, 0x0000, 0x1FE0, 0x0000, 0x0000, 0x0000 },
    /* RIGHT */ { 0x0000, 0x0000, 0x7FF8, 0x0000, 0x07F8, 0x0000, 0x7FF8, 0x0000, 0x07F8, 0x0000, 0x7FF8, 0x0000, 0x07F8, 0x0000, 0x0000, 0x0000 },
    /* JUSTIFY */ { 0x0000, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x0000, 0x0000 },
    /* SINGLE */ { 0x0000, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0x0000 },
    /* ONEHALF */ { 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0x4E00, 0xC828, 0x4E10 },
    /* DOUBLE */ { 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0x0000, 0x7FF8, 0x7FF8, 0x0000, 0x0000, 0xE280, 0x6100 },
    /* INDENT_LESS */ { 0x0000, 0x0000, 0x7FF8, 0x0000, 0x03F8, 0x1000, 0x33F8, 0x7000, 0x33F8, 0x1000, 0x03F8, 0x0000, 0x7FF8, 0x0000, 0x0000, 0x0000 },
    /* INDENT_MORE */ { 0x0000, 0x0000, 0x7FF8, 0x0000, 0x03F8, 0x4000, 0x63F8, 0x7000, 0x63F8, 0x4000, 0x03F8, 0x0000, 0x7FF8, 0x0000, 0x0000, 0x0000 },
};

/* GENERATED: the 8-row icons for screens with tall pixels -- row pairs
   folded for the letter and object icons, drawn by hand for the line-art
   ones (alignment, spacing, indent), which fold into solid blocks. */
static const u16 g_icon_bits8[WP_ICON_COUNT][8] = {
    /* NEW */ { 0x1FC0, 0x1070, 0x1078, 0x1008, 0x1008, 0x1008, 0x1FF8, 0x0000 },
    /* OPEN */ { 0x0000, 0x7E00, 0x41F8, 0x4FFC, 0x5808, 0x7010, 0x7FE0, 0x0000 },
    /* SAVE */ { 0x7FF8, 0x502C, 0x5FE4, 0x4FE4, 0x4B24, 0x4B24, 0x7FFC, 0x0000 },
    /* CUT */ { 0x1010, 0x1830, 0x0C60, 0x0380, 0x3EF8, 0x4284, 0x7EFC, 0x0000 },
    /* COPY */ { 0x7E00, 0x4300, 0x4FE0, 0x4820, 0x7820, 0x0FE0, 0x0000, 0x0000 },
    /* PASTE */ { 0x0780, 0x3FF0, 0x27F0, 0x2430, 0x2410, 0x3C10, 0x07F0, 0x0000 },
    /* UNDO */ { 0x0000, 0x1800, 0x7FE0, 0x3F98, 0x0808, 0x07F0, 0x0000, 0x0000 },
    /* BOLD */ { 0x0000, 0x3FE0, 0x38E0, 0x3FE0, 0x3870, 0x3FE0, 0x0000, 0x0000 },
    /* ITALIC */ { 0x0000, 0x01F0, 0x00C0, 0x0180, 0x0300, 0x0F80, 0x0000, 0x0000 },
    /* UNDERLINE */ { 0x0000, 0x1830, 0x1830, 0x1830, 0x1C70, 0x07C0, 0x3FF8, 0x0000 },
    /* LEFT */ { 0x7FF8, 0x0000, 0x7F00, 0x0000, 0x7FF8, 0x0000, 0x7F00, 0x0000 },
    /* CENTER */ { 0x7FF8, 0x0000, 0x1FE0, 0x0000, 0x7FF8, 0x0000, 0x1FE0, 0x0000 },
    /* RIGHT */ { 0x7FF8, 0x0000, 0x03F8, 0x0000, 0x7FF8, 0x0000, 0x03F8, 0x0000 },
    /* JUSTIFY */ { 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000 },
    /* SINGLE */ { 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000 },
    /* ONEHALF */ { 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x7FF8, 0x0000, 0x5CA0, 0x5840 },
    /* DOUBLE */ { 0x7FF8, 0x0000, 0x0000, 0x7FF8, 0x0000, 0x0000, 0x7FF8, 0x0000 },
    /* INDENT_LESS */ { 0x7FF8, 0x03F8, 0x2000, 0x63F8, 0x2000, 0x03F8, 0x7FF8, 0x0000 },
    /* INDENT_MORE */ { 0x7FF8, 0x03F8, 0x4000, 0x63F8, 0x4000, 0x03F8, 0x7FF8, 0x0000 },
};

/* The buttons, left to right; a row with action WP_ACT_COUNT is a gap
   between groups. label is the DOS spelling, three cells with brackets. */
typedef struct {
    wp_action_id action;
    u8           icon;
    char         label;
} rb_button;

static const rb_button g_buttons[] = {
    { WP_ACT_FILE_NEW,          WP_ICON_NEW,         'N' },
    { WP_ACT_FILE_OPEN,         WP_ICON_OPEN,        'O' },
    { WP_ACT_FILE_SAVE,         WP_ICON_SAVE,        'S' },
    { WP_ACT_COUNT,             0,                   0   },
    { WP_ACT_EDIT_CUT,          WP_ICON_CUT,         'X' },
    { WP_ACT_EDIT_COPY,         WP_ICON_COPY,        'C' },
    { WP_ACT_EDIT_PASTE,        WP_ICON_PASTE,       'V' },
    { WP_ACT_EDIT_UNDO,         WP_ICON_UNDO,        'Z' },
    { WP_ACT_COUNT,             0,                   0   },
    { WP_ACT_FORMAT_BOLD,       WP_ICON_BOLD,        'B' },
    { WP_ACT_FORMAT_ITALIC,     WP_ICON_ITALIC,      'I' },
    { WP_ACT_FORMAT_UNDERLINE,  WP_ICON_UNDERLINE,   'U' },
    { WP_ACT_COUNT,             0,                   0   },
    { WP_ACT_PARA_LEFT,         WP_ICON_LEFT,        'L' },
    { WP_ACT_PARA_CENTER,       WP_ICON_CENTER,      'E' },
    { WP_ACT_PARA_RIGHT,        WP_ICON_RIGHT,       'R' },
    { WP_ACT_PARA_JUSTIFY,      WP_ICON_JUSTIFY,     'J' },
    { WP_ACT_COUNT,             0,                   0   },
    { WP_ACT_PARA_SINGLE,       WP_ICON_SINGLE,      '1' },
    { WP_ACT_PARA_ONEHALF,      WP_ICON_ONEHALF,     'H' },
    { WP_ACT_PARA_DOUBLE,       WP_ICON_DOUBLE,      '2' },
    { WP_ACT_COUNT,             0,                   0   },
    { WP_ACT_PARA_INDENT_LESS,  WP_ICON_INDENT_LESS, '<' },
    { WP_ACT_PARA_INDENT_MORE,  WP_ICON_INDENT_MORE, '>' }
};
#define RB_BUTTON_COUNT ((u32)(sizeof(g_buttons) / sizeof(g_buttons[0])))

static wp_bool text_mode(const wp_rulerbar *rb) { return (wp_bool)(rb->icon_rows == 0); }

/* A gap is half a button wide (a single cell in text mode). */
static i32 gap_w(const wp_rulerbar *rb) { return text_mode(rb) ? 1 : rb->button_w / 2; }

/* The gap between the button row and the ruler. */
static i32 row_gap(const wp_rulerbar *rb) { return text_mode(rb) ? 0 : 2; }

i32 wp_rulerbar_height(const wp_rulerbar *rb)
{
    return rb->button_h + row_gap(rb) + rb->scale_h;
}

wp_bool wp_rulerbar_has_action(wp_action_id id)
{
    u32 i;

    for (i = 0; i < RB_BUTTON_COUNT; i++) {
        if (g_buttons[i].action == id) return WP_TRUE;
    }
    return WP_FALSE;
}

/* The caret paragraph's style and the width it was laid out at. */
static const wp_para_style *caret_style(wp_app_ctx *ctx, i32 *wrap_out)
{
    wp_pos *cur;
    wp_section *sec;
    wp_paragraph *para;

    *wrap_out = 0;
    if (!ctx->doc_loaded) return NULL;
    cur = wp_keymap_cursor(ctx);
    sec = wp_document_section_get(ctx->doc, cur->section);
    if (sec == NULL) return NULL;
    para = wp_section_paragraph_get(sec, cur->paragraph);
    if (para == NULL) return NULL;
    if (para->layout != NULL) *wrap_out = ((const wp_paragraph_layout *)para->layout)->wrap_twips;
    if (*wrap_out <= 0) *wrap_out = wp_section_column_twips(sec);
    return wp_document_pstyle_get(ctx->doc, para->style);
}

static wp_bool button_active(wp_app_ctx *ctx, const wp_para_style *ps, wp_action_id id)
{
    u8 align = (ps != NULL) ? ps->align : (u8)WP_ALIGN_LEFT;
    i32 ls = (ps != NULL) ? ps->line_spacing : 0;

    switch (id) {
    case WP_ACT_FORMAT_BOLD:      return (wp_bool)((ctx->style_flags & WP_STYLE_BOLD) != 0);
    case WP_ACT_FORMAT_ITALIC:    return (wp_bool)((ctx->style_flags & WP_STYLE_ITALIC) != 0);
    case WP_ACT_FORMAT_UNDERLINE: return (wp_bool)((ctx->style_flags & WP_STYLE_UNDERLINE) != 0);
    case WP_ACT_PARA_LEFT:        return (wp_bool)(align == WP_ALIGN_LEFT);
    case WP_ACT_PARA_CENTER:      return (wp_bool)(align == WP_ALIGN_CENTER);
    case WP_ACT_PARA_RIGHT:       return (wp_bool)(align == WP_ALIGN_RIGHT);
    case WP_ACT_PARA_JUSTIFY:     return (wp_bool)(align == WP_ALIGN_JUSTIFY);
    case WP_ACT_PARA_SINGLE:      return (wp_bool)(ls == 0);
    case WP_ACT_PARA_ONEHALF:     return (wp_bool)(ls == 360);
    case WP_ACT_PARA_DOUBLE:      return (wp_bool)(ls == 480);
    default:                      return WP_FALSE;
    }
}

/* One icon, run by run: a row's set pixels go out as as few rectangles
   as it has runs (two or three for most rows), not one per pixel. At 8
   rows the 8-row set is used. */
static void draw_icon(plat_gc *gc, i32 x, i32 y, u8 icon, i32 rows, u8 c)
{
    i32 r;
    i32 n = (rows >= 16) ? 16 : 8;

    for (r = 0; r < n; r++) {
        u16 bits = (n == 16) ? g_icon_bits[icon][r] : g_icon_bits8[icon][r];
        i32 col = 0;

        while (col < 16) {
            i32 start;

            while (col < 16 && !(bits & (0x8000u >> col))) col++;
            if (col >= 16) break;
            start = col;
            while (col < 16 && (bits & (0x8000u >> col))) col++;
            (void)plat_fill_rect(gc, x + start, y + r, col - start, 1, c, c, c);
        }
    }
}

static void paint_buttons(plat_gc *gc, plat_font *font, i32 ascent,
                          const wp_rulerbar *rb, wp_app_ctx *ctx, const wp_para_style *ps)
{
    i32 x = rb->x;
    u32 i;

    for (i = 0; i < RB_BUTTON_COUNT; i++) {
        const rb_button *b = &g_buttons[i];
        wp_bool on;

        if (b->action == WP_ACT_COUNT) { x += gap_w(rb); continue; }
        if (x + rb->button_w > rb->x + rb->w) break;   /* out of room: the rest go */
        on = button_active(ctx, ps, b->action);

        if (text_mode(rb)) {
            char label[3];
            label[0] = '[';
            label[1] = b->label;
            label[2] = ']';
            /* Bright for a button that is on, the usual grey otherwise. */
            if (on) (void)plat_draw_text(gc, font, x, rb->y, (const u8 *)label, 3, 0xFF, 0xFF, 0xFF);
            else    (void)plat_draw_text(gc, font, x, rb->y, (const u8 *)label, 3, 0x80, 0x80, 0x80);
        } else {
            i32 ix = x + (rb->button_w - 16) / 2;
            i32 iy = rb->y + (rb->button_h - rb->icon_rows) / 2;

            /* An "on" button is drawn inverted; every button gets a frame. */
            if (on) {
                (void)plat_fill_rect(gc, x, rb->y, rb->button_w - 1, rb->button_h, 0, 0, 0);
                draw_icon(gc, ix, iy, b->icon, rb->icon_rows, 0xFF);
            } else {
                (void)plat_fill_rect(gc, x, rb->y, rb->button_w - 1, 1, 0x60, 0x60, 0x60);
                (void)plat_fill_rect(gc, x, rb->y + rb->button_h - 1, rb->button_w - 1, 1, 0x60, 0x60, 0x60);
                (void)plat_fill_rect(gc, x, rb->y, 1, rb->button_h, 0x60, 0x60, 0x60);
                (void)plat_fill_rect(gc, x + rb->button_w - 2, rb->y, 1, rb->button_h, 0x60, 0x60, 0x60);
                draw_icon(gc, ix, iy, b->icon, rb->icon_rows, 0);
            }
        }
        x += rb->button_w;
    }
    (void)ascent;
}

/* ---- The ruler -------------------------------------------------------- */

/* A small triangle, `size` wide at its base: pointing down (apex at the
   bottom) or up. Built from shrinking rows of rectangles. */
static void triangle(plat_gc *gc, i32 cx, i32 y, i32 size, wp_bool down)
{
    i32 rowi;
    i32 rows = (size + 1) / 2;

    for (rowi = 0; rowi < rows; rowi++) {
        i32 half = (down ? rows - 1 - rowi : rowi);
        (void)plat_fill_rect(gc, cx - half, y + rowi, half * 2 + 1, 1, 0, 0, 0);
    }
}

static void paint_ruler_px(plat_gc *gc, plat_font *font, i32 ascent,
                           const wp_rulerbar *rb, const wp_para_style *ps, i32 wrap)
{
    i32 top = rb->y + rb->button_h + row_gap(rb);
    i32 h = rb->scale_h;
    i32 base = top + h - 1;
    i32 x0 = rb->origin_x;
    i32 x1 = rb->origin_x + rb->twips_to_x(wrap);
    i32 eighth_px = rb->twips_to_x(180);
    i32 t;
    u8 i;

    /* The text column's extent: a baseline, and a bar at each margin. */
    (void)plat_fill_rect(gc, x0, base, x1 - x0 + 1, 1, 0, 0, 0);
    (void)plat_fill_rect(gc, x0, top, 1, h, 0, 0, 0);
    (void)plat_fill_rect(gc, x1, top, 1, h, 0, 0, 0);

    /* Ticks: inches tall and numbered, halves medium, quarters and
       eighths short (eighths only where there is room to see them). */
    for (t = 180; t < wrap; t += 180) {
        i32 x = x0 + rb->twips_to_x(t);
        i32 len;

        if (t % 1440 == 0) len = h / 2;
        else if (t % 720 == 0) len = h / 3;
        else if (t % 360 == 0 || eighth_px >= 5) len = h / 5 + 1;
        else continue;
        (void)plat_fill_rect(gc, x, base - len, 1, len, 0, 0, 0);
        if (t % 1440 == 0 && font != NULL) {
            char d[3];
            u32 n = 0;
            i32 inch = t / 1440;
            if (inch >= 10) d[n++] = (char)('0' + inch / 10);
            d[n++] = (char)('0' + inch % 10);
            (void)plat_draw_text(gc, font, x + 2, top + ascent, (const u8 *)d, n, 0, 0, 0);
        }
    }

    if (ps == NULL) return;

    /* Indents: first line above, left and right below, as on every ruler
       since Word 1.0 and Atari Works. */
    triangle(gc, x0 + rb->twips_to_x(ps->indent_left + ps->indent_first), top, 7, WP_TRUE);
    triangle(gc, x0 + rb->twips_to_x(ps->indent_left), base - 4, 7, WP_FALSE);
    triangle(gc, x0 + rb->twips_to_x(wrap - ps->indent_right), base - 4, 7, WP_FALSE);

    /* Tab stops: an L for a left tab, mirrored for right, an inverted T
       for centre, the T with a dot for decimal. */
    for (i = 0; i < ps->tab_count && i < WP_MAX_TABS; i++) {
        /* Two pixels thick, so a stop reads at a glance next to the
           one-pixel ticks it sits among. */
        i32 x = x0 + rb->twips_to_x(ps->tab_pos[i]);
        i32 th = (h > 10) ? 7 : 5;
        i32 sy = base - th;
        (void)plat_fill_rect(gc, x, sy, 2, th, 0, 0, 0);
        switch (ps->tab_kind[i]) {
        case WP_TAB_RIGHT:   (void)plat_fill_rect(gc, x - 4, base - 2, 6, 2, 0, 0, 0); break;
        case WP_TAB_CENTER:  (void)plat_fill_rect(gc, x - 3, base - 2, 8, 2, 0, 0, 0); break;
        case WP_TAB_DECIMAL: (void)plat_fill_rect(gc, x - 3, base - 2, 8, 2, 0, 0, 0);
                             (void)plat_fill_rect(gc, x + 4, sy, 2, 2, 0, 0, 0); break;
        default:             (void)plat_fill_rect(gc, x, base - 2, 6, 2, 0, 0, 0); break;
        }
    }
}

/* DOS: one row of characters, one per cell, from the column's left
   edge. Inches are digits, half inches ':', the rest '.'; the margins
   are [ and ], indents > (first line) < (right) and | (left), and tab
   stops L R C D -- each mark overwriting the scale under it. */
static void paint_ruler_text(plat_gc *gc, plat_font *font, const wp_rulerbar *rb,
                             const wp_para_style *ps, i32 wrap)
{
    char line[160];
    i32 cols = rb->twips_to_x(wrap) + 1;
    i32 room = rb->x + rb->w - rb->origin_x;
    i32 c;
    u8 i;

    if (cols > room) cols = room;
    if (cols > (i32)sizeof(line)) cols = (i32)sizeof(line);
    if (cols < 2) return;
    for (c = 0; c < cols; c++) {
        i32 t = rb->x_to_twips(c);
        i32 next = rb->x_to_twips(c + 1);
        /* A mark belongs to the cell its position falls in. */
        if (c > 0 && (t / 1440) != (next / 1440) && next % 1440 != 0) line[c] = (char)('0' + ((next / 1440) % 10));
        else if (c > 0 && t % 1440 == 0) line[c] = (char)('0' + ((t / 1440) % 10));
        else if (c > 0 && t % 720 == 0) line[c] = ':';
        else line[c] = '.';
    }
    if (ps != NULL) {
        for (i = 0; i < ps->tab_count && i < WP_MAX_TABS; i++) {
            i32 tc = rb->twips_to_x(ps->tab_pos[i]);
            if (tc > 0 && tc < cols) {
                line[tc] = (ps->tab_kind[i] == WP_TAB_RIGHT) ? 'R'
                         : (ps->tab_kind[i] == WP_TAB_CENTER) ? 'C'
                         : (ps->tab_kind[i] == WP_TAB_DECIMAL) ? 'D' : 'L';
            }
        }
        c = rb->twips_to_x(ps->indent_left);
        if (c >= 0 && c < cols) line[c] = '|';
        c = rb->twips_to_x(ps->indent_left + ps->indent_first);
        if (c >= 0 && c < cols) line[c] = '>';
        c = rb->twips_to_x(wrap - ps->indent_right);
        if (c >= 0 && c < cols) line[c] = '<';
    }
    line[0] = '[';
    if (rb->twips_to_x(wrap) < cols) line[rb->twips_to_x(wrap)] = ']';
    (void)plat_draw_text(gc, font, rb->origin_x, rb->y + rb->button_h, (const u8 *)line, (u32)cols,
                         0xAA, 0xAA, 0xAA);
}

void wp_rulerbar_paint(plat_gc *gc, plat_font *font, i32 ascent,
                       const wp_rulerbar *rb, wp_app_ctx *ctx)
{
    i32 wrap = 0;
    const wp_para_style *ps = caret_style(ctx, &wrap);

    if (wrap <= 0) wrap = 9360;
    if (!text_mode(rb)) {
        (void)plat_fill_rect(gc, rb->x, rb->y, rb->w, wp_rulerbar_height(rb), 0xFF, 0xFF, 0xFF);
    }
    paint_buttons(gc, font, ascent, rb, ctx, ps);
    if (text_mode(rb)) paint_ruler_text(gc, font, rb, ps, wrap);
    else paint_ruler_px(gc, font, ascent, rb, ps, wrap);
}

static u32 mix(u32 h, u32 v)
{
    /* FNV-1a over the value's four bytes. */
    u32 i;
    for (i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 16777619uL;
    }
    return h;
}

u32 wp_rulerbar_signature(const wp_rulerbar *rb, wp_app_ctx *ctx)
{
    i32 wrap = 0;
    const wp_para_style *ps = caret_style(ctx, &wrap);
    u32 h = 2166136261uL;
    u8 i;

    h = mix(h, (u32)rb->x); h = mix(h, (u32)rb->y); h = mix(h, (u32)rb->w);
    h = mix(h, (u32)rb->origin_x); h = mix(h, (u32)wrap);
    h = mix(h, (u32)ctx->style_flags); h = mix(h, (u32)ctx->doc_loaded);
    if (ps != NULL) {
        h = mix(h, (u32)ps->align); h = mix(h, (u32)ps->line_spacing);
        h = mix(h, (u32)ps->indent_left); h = mix(h, (u32)ps->indent_right);
        h = mix(h, (u32)ps->indent_first); h = mix(h, (u32)ps->tab_count);
        for (i = 0; i < ps->tab_count && i < WP_MAX_TABS; i++) {
            h = mix(h, (u32)ps->tab_pos[i]); h = mix(h, (u32)ps->tab_kind[i]);
        }
    }
    return h;
}

wp_bool wp_rulerbar_hit_test(const wp_rulerbar *rb, i32 x, i32 y, wp_rulerbar_hit *out)
{
    i32 bx = rb->x;
    u32 i;

    out->kind = (u8)WP_RULERBAR_MISS;
    out->action = WP_ACT_COUNT;
    out->twips = 0;

    if (y >= rb->y && y < rb->y + rb->button_h) {
        for (i = 0; i < RB_BUTTON_COUNT; i++) {
            const rb_button *b = &g_buttons[i];

            if (b->action == WP_ACT_COUNT) { bx += gap_w(rb); continue; }
            if (bx + rb->button_w > rb->x + rb->w) break;
            if (x >= bx && x < bx + rb->button_w) {
                out->kind = (u8)WP_RULERBAR_BUTTON;
                out->action = b->action;
                return WP_TRUE;
            }
            bx += rb->button_w;
        }
        return WP_FALSE;
    }
    if (y >= rb->y + rb->button_h && y < rb->y + wp_rulerbar_height(rb) && x >= rb->origin_x) {
        out->kind = (u8)WP_RULERBAR_RULER;
        out->twips = rb->x_to_twips(x - rb->origin_x);
        return WP_TRUE;
    }
    return WP_FALSE;
}
