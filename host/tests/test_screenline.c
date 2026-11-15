/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "test.h"
#include "screenline.h"

/* A line's words drawn at their screen widths, its spare room placed by
   its alignment (screenline.h). Three words of paper width 1000, each
   600 wide on screen, a 200 space between: slack 400 a word. */
static void three_words(wp_screenline *sl, u8 align, wp_bool justified)
{
    wp_screenline_reset(sl);
    wp_screenline_add(sl, 0, 1000, 600, 0, 0, 0, 0);
    wp_screenline_add(sl, 1200, 1000, 600, 1, 0, 0, 10);
    wp_screenline_add(sl, 2400, 1000, 600, 2, 0, 0, 20);
    wp_screenline_place(sl, align, justified);
}

void test_screenline_alignments(void)
{
    static wp_screenline sl;

    /* Left: closed up from the start, the spare 1200 after the line. */
    three_words(&sl, WP_ALIGN_LEFT, WP_FALSE);
    ASSERT_TRUE(!sl.identity);
    ASSERT_EQ_U32((u32)sl.w[0].screen_x, 0);
    ASSERT_EQ_U32((u32)sl.w[1].screen_x, 800);    /* 600 + the 200 space */
    ASSERT_EQ_U32((u32)sl.w[2].screen_x, 1600);
    /* Right: the same, 1200 along -- the last word ends where paper's does. */
    three_words(&sl, WP_ALIGN_RIGHT, WP_FALSE);
    ASSERT_EQ_U32((u32)sl.w[0].screen_x, 1200);
    ASSERT_EQ_U32((u32)(sl.w[2].screen_x + sl.w[2].ink_w), 3400);
    /* Centred: half each side. */
    three_words(&sl, WP_ALIGN_CENTER, WP_FALSE);
    ASSERT_EQ_U32((u32)sl.w[0].screen_x, 600);
    ASSERT_EQ_U32((u32)sl.w[1].screen_x, 1400);
    /* Justified: both ends where paper's are, the room shared between
       the two gaps. */
    three_words(&sl, WP_ALIGN_JUSTIFY, WP_TRUE);
    ASSERT_EQ_U32((u32)sl.w[0].screen_x, 0);
    ASSERT_EQ_U32((u32)sl.w[1].screen_x, 1400);
    ASSERT_EQ_U32((u32)(sl.w[2].screen_x + sl.w[2].ink_w), 3400);
    /* A justified paragraph's last line is ranged left. */
    three_words(&sl, WP_ALIGN_JUSTIFY, WP_FALSE);
    ASSERT_EQ_U32((u32)sl.w[1].screen_x, 800);
}

void test_screenline_mapping(void)
{
    static wp_screenline sl;
    i32 x;

    three_words(&sl, WP_ALIGN_CENTER, WP_FALSE);
    /* A word's ends map to its drawn ends; its middle to its middle. */
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 1200), 1400);
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 2200), 2000);
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 1700), 1700);
    /* Inside a gap, across it. */
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 1100), 1300);
    /* Past the end, the shift the last word had. */
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 3500), 2900);
    /* And back: every x a round trip, within a twip of rounding. */
    for (x = 0; x <= 3600; x += 50) {
        i32 back = wp_screenline_paper_x(&sl, wp_screenline_x(&sl, x));
        i32 d = back - x;
        ASSERT_TRUE(d >= -2 && d <= 2);
    }
    /* Monotonic. */
    {
        i32 prev = -1;
        for (x = 0; x <= 3600; x += 10) {
            i32 s = wp_screenline_x(&sl, x);
            ASSERT_TRUE(s >= prev);
            prev = s;
        }
    }
}

void test_screenline_tabs_identity_and_cap(void)
{
    static wp_screenline sl;
    u16 i, k = 0;

    /* A tab: the part after it keeps its start, whatever the alignment. */
    wp_screenline_reset(&sl);
    wp_screenline_add(&sl, 0, 1000, 600, 0, 0, 0, 0);
    wp_screenline_add(&sl, 3000, 1000, 600, 0, 1, 0, 5);
    wp_screenline_add(&sl, 4200, 1000, 600, 1, 1, 0, 9);
    wp_screenline_place(&sl, WP_ALIGN_CENTER, WP_FALSE);
    ASSERT_EQ_U32((u32)sl.w[0].screen_x, 0);
    ASSERT_EQ_U32((u32)sl.w[1].screen_x, 3000);
    ASSERT_EQ_U32((u32)sl.w[2].screen_x, 3800);
    /* Found again by fragment. */
    ASSERT_TRUE(wp_screenline_word(&sl, &k, 0, 5) == &sl.w[1]);
    ASSERT_TRUE(wp_screenline_word(&sl, &k, 0, 0) == NULL);   /* already past it */

    /* Screen as wide as paper (paper metrics off, or a wider face held
       to its width): nothing moves. */
    wp_screenline_reset(&sl);
    wp_screenline_add(&sl, 0, 1000, 1000, 0, 0, 0, 0);
    wp_screenline_add(&sl, 1200, 1000, 1400, 1, 0, 0, 10);
    wp_screenline_place(&sl, WP_ALIGN_LEFT, WP_FALSE);
    ASSERT_TRUE(sl.identity);
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 1500), 1500);

    /* Past the cap: laid out as the layout did, not half placed. */
    wp_screenline_reset(&sl);
    for (i = 0; i < WP_SCREENLINE_MAX + 3; i++)
        wp_screenline_add(&sl, (i32)i * 100, 80, 40, i, 0, 0, i);
    wp_screenline_place(&sl, WP_ALIGN_LEFT, WP_FALSE);
    ASSERT_TRUE(sl.identity);
    ASSERT_EQ_U32((u32)sl.n, 0);
    ASSERT_EQ_U32((u32)wp_screenline_x(&sl, 777), 777);
}
