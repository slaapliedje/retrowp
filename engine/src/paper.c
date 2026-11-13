/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "paper.h"

/* Millimetres to twips, rounded: 1440 / 25.4. */
#define MM(x) ((i32)(((x) * 14400L + 127L) / 254L))

static const wp_paper g_paper[] = {
    { "Letter",                    12240, 15840, WP_PAPER_SHEET },
    { "Legal",                     12240, 20160, WP_PAPER_SHEET },
    { "Executive",                 10440, 15120, WP_PAPER_SHEET },
    { "Half Letter",                7920, 12240, WP_PAPER_SHEET },
    { "Tabloid",                   15840, 24480, WP_PAPER_SHEET },
    { "A3",                        MM(297), MM(420), WP_PAPER_SHEET },
    { "A4",                        MM(210), MM(297), WP_PAPER_SHEET },
    { "A5",                        MM(148), MM(210), WP_PAPER_SHEET },
    { "A6",                        MM(105), MM(148), WP_PAPER_SHEET },
    { "B5",                        MM(176), MM(250), WP_PAPER_SHEET },
    { "Envelope #10",               5940, 13680, WP_PAPER_ENVELOPE },
    { "Envelope DL",               MM(110), MM(220), WP_PAPER_ENVELOPE },
    { "Envelope C5",               MM(162), MM(229), WP_PAPER_ENVELOPE },
    { "Envelope C6",               MM(114), MM(162), WP_PAPER_ENVELOPE },
    { "Envelope Monarch",           5580, 10800, WP_PAPER_ENVELOPE },
    { "Index card 3x5",             4320,  7200, WP_PAPER_CARD },
    { "Index card 4x6",             5760,  8640, WP_PAPER_CARD },
    { "Index card 5x8",             7200, 11520, WP_PAPER_CARD },
    { "CD/DVD label",              WP_DISC_TWIPS, WP_DISC_TWIPS, WP_PAPER_DISC },
    { "CD booklet",                MM(120), MM(121), WP_PAPER_CARD },
    { "CD tray card",              MM(118), MM(150), WP_PAPER_CARD }
};
#define PAPERS ((u32)(sizeof(g_paper) / sizeof(g_paper[0])))

u32 wp_paper_count(void) { return PAPERS; }
const wp_paper *wp_paper_get(u32 index) { return index < PAPERS ? &g_paper[index] : NULL; }

static wp_bool near(i32 a, i32 b) { return (wp_bool)(a - b <= 20 && b - a <= 20); }

const wp_paper *wp_paper_match(i32 w, i32 h, wp_bool *landscape)
{
    u32 i;
    for (i = 0; i < PAPERS; i++) {
        if (near(w, g_paper[i].w) && near(h, g_paper[i].h)) { if (landscape) *landscape = WP_FALSE; return &g_paper[i]; }
        if (near(w, g_paper[i].h) && near(h, g_paper[i].w)) { if (landscape) *landscape = WP_TRUE; return &g_paper[i]; }
    }
    if (landscape) *landscape = WP_FALSE;
    return NULL;
}
