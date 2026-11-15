/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "labels.h"

/* Thirty-seconds of an inch (Avery's US figures are), and millimetres
   in hundredths, to twips. */
#define IN32(x) ((i32)((x) * 45L))
#define MM(x) ((i32)(((x) * 1440L + 1270L) / 2540L))

#define LETTER 12240, 15840
#define A4     MM(21000), MM(29700)

/* Each sheet's figures are the maker's template's; the margins they
   leave come out even either side.

   Checked 2026-11-15. The Letter sheets' label sizes, counts and
   pitches match Microsoft's table of Avery products (KB Q146892, for
   Word 95; 5167 through its equivalents 5267 and 8167), and their
   margins are what centres those on the sheet. gLabels' templates
   agree, except that they give 5160 a 2.781-inch pitch to Microsoft's
   2.75. The A4 sheets' label sizes and counts match gLabels' too, but
   it puts the gap between columns up to 0.9 mm wider on L7160-L7163
   and L7651, with the margins uneven, and neither could be checked
   against Avery's own pages. L7173 has no second source yet. */
static const wp_label_sheet g_sheets[] = {
    /* Letter (Avery US); a third of an inch is 480 twips */
    { "5160 Address (30)",  LETTER, IN32(6), IN32(16), IN32(84),  IN32(32), IN32(88),  IN32(32), 3, 10 },
    { "5161 Address (20)",  LETTER, IN32(5), IN32(16), IN32(128), IN32(32), IN32(134), IN32(32), 2, 10 },
    { "5162 Address (14)",  LETTER, IN32(5), 1200,     IN32(128), 1920,     IN32(134), 1920,     2, 7 },
    { "5163 Shipping (10)", LETTER, IN32(5), IN32(16), IN32(128), IN32(64), IN32(134), IN32(64), 2, 5 },
    { "5164 Shipping (6)",  LETTER, IN32(5), IN32(16), IN32(128), 4800,     IN32(134), 4800,     2, 3 },
    { "5167 Return (80)",   LETTER, IN32(9), IN32(16), IN32(56),  IN32(16), IN32(66),  IN32(16), 4, 20 },
    { "5371 Bus. card (10)", LETTER, IN32(24), IN32(16), IN32(112), IN32(64), IN32(112), IN32(64), 2, 5 },
    /* A4 (Avery UK) */
    { "L7160 Address (21)", A4, MM(725), MM(1515), MM(6350), MM(3810), MM(6604), MM(3810), 3, 7 },
    { "L7161 Address (18)", A4, MM(725), MM(880),  MM(6350), MM(4660), MM(6604), MM(4660), 3, 6 },
    { "L7162 Address (16)", A4, MM(465), MM(1290), MM(9910), MM(3390), MM(10160), MM(3390), 2, 8 },
    { "L7163 Address (14)", A4, MM(465), MM(1515), MM(9910), MM(3810), MM(10160), MM(3810), 2, 7 },
    { "L7165 Parcel (8)",   A4, MM(465), MM(1310), MM(9910), MM(6770), MM(10160), MM(6770), 2, 4 },
    { "L7173 Shipping (10)", A4, MM(465), MM(600), MM(9910), MM(5700), MM(10160), MM(5700), 2, 5 },
    { "L7651 Mini (65)",    A4, MM(475), MM(1070), MM(3810), MM(2120), MM(4064), MM(2120), 5, 13 }
};

u32 wp_label_sheet_count(void)
{
    return (u32)(sizeof(g_sheets) / sizeof(g_sheets[0]));
}

const wp_label_sheet *wp_label_sheet_get(u32 index)
{
    return index < wp_label_sheet_count() ? &g_sheets[index] : NULL;
}

/* The cells a pitch wide, starting half the space between two labels
   left of the first: each label then sits in the middle of its cell. A
   row is a pitch tall from the first label's top. */
i32 wp_label_sheet_apply(wp_section *sec, const wp_label_sheet *t)
{
    i32 half;

    if (sec == NULL || t == NULL) return WP_LABEL_INSET;
    half = (t->pitch_x - t->label_w) / 2;
    if (half < 0) half = 0;
    sec->page_width = t->sheet_w;
    sec->page_height = t->sheet_h;
    sec->margin_left = t->left - half;
    if (sec->margin_left < 0) sec->margin_left = 0;
    sec->margin_right = t->sheet_w - sec->margin_left - (i32)t->across * t->pitch_x;
    if (sec->margin_right < 0) sec->margin_right = 0;
    sec->margin_top = t->top;
    sec->margin_bottom = t->sheet_h - t->top - (i32)t->down * t->pitch_y;
    if (sec->margin_bottom < 0) sec->margin_bottom = 0;
    sec->columns = 1;
    return WP_LABEL_INSET + half;
}
