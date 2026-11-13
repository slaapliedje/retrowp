/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "papermetrics.h"
#include "platform.h"
#include "format_txt.h"
#include "layout.h"
#include "pageview.h"
#include "config.h"

/* Paper metrics (retroplat papermetrics.h): text measured as it prints. */

/* Adobe's published widths: "Hello" at 12 point is 2222/1000 em in
   Times, 2278 in Helvetica, 3000 in Courier. */
void test_papermetrics_widths(void)
{
    plat_paper_face f;
    plat_font_metrics m;
    i32 w = 0;

    plat_paper_face_init(&f, (const u8 *)"Times New Roman", 15, 240, 0);
    ASSERT_TRUE(plat_paper_measure(&f, (const u8 *)"Hello", 5, &w) == WP_OK);
    ASSERT_EQ_U32((u32)w, 533);   /* 2222 * 240 / 1000 */
    plat_paper_face_init(&f, (const u8 *)"Arial", 5, 240, 0);
    ASSERT_TRUE(plat_paper_measure(&f, (const u8 *)"Hello", 5, &w) == WP_OK);
    ASSERT_EQ_U32((u32)w, 547);
    plat_paper_face_init(&f, (const u8 *)"", 0, 240, (u16)(PLAT_FAMILY_MODERN << PLAT_FAMILY_SHIFT));
    ASSERT_TRUE(plat_paper_measure(&f, (const u8 *)"Hello", 5, &w) == WP_OK);
    ASSERT_EQ_U32((u32)w, 720);
    /* Bold Times is wider: H 778, e 444, l 278, l 278, o 500. */
    plat_paper_face_init(&f, (const u8 *)"Times", 5, 240, PLAT_STYLE_BOLD);
    ASSERT_TRUE(plat_paper_measure(&f, (const u8 *)"Hello", 5, &w) == WP_OK);
    ASSERT_EQ_U32((u32)w, 547);
    /* An en dash (Windows-1252's slot) is 500 in Times; a character the
       faces lack takes an n's width. */
    plat_paper_face_init(&f, (const u8 *)"Times", 5, 1000, 0);
    ASSERT_TRUE(plat_paper_measure(&f, (const u8 *)"\xE2\x80\x93", 3, &w) == WP_OK);
    ASSERT_EQ_U32((u32)w, 500);
    ASSERT_TRUE(plat_paper_measure(&f, (const u8 *)"\xE2\x82\xBD", 3, &w) == WP_OK);   /* a rouble sign */
    ASSERT_EQ_U32((u32)w, 500);
    /* A line is 1.2 em. */
    plat_paper_face_init(&f, (const u8 *)"Times", 5, 240, 0);
    plat_paper_metrics(&f, &m);
    ASSERT_EQ_U32((u32)(m.ascent + m.descent), 288);
}

/* Switched on, layout measures with them: a line of 12-point text is
   12 * 1.2 points high whatever the backend's own fonts are; the config
   key turns it on and off. */
void test_papermetrics_layout_and_config(void)
{
    wp_document doc;
    const wp_paragraph_layout *l;
    wp_config cfg;

    ASSERT_TRUE(format_txt_import((const u8 *)"Some words here.\n", 17, &doc) == WP_OK);
    plat_metrics_set_paper(WP_TRUE);
    (void)wp_pageview_relayout(&doc, 0);
    l = (const wp_paragraph_layout *)doc.sections[0].paras[0].layout;
    ASSERT_TRUE(l != NULL && l->line_count == 1);
    if (l != NULL) ASSERT_EQ_U32((u32)l->lines[0].text_height_twips, 288);
    plat_metrics_set_paper(WP_FALSE);
    wp_document_free(&doc);

    ASSERT_TRUE(wp_config_parse((const u8 *)"keymap=vim\n", 11, &cfg) == WP_OK);
    ASSERT_EQ_U32(cfg.paper_metrics, 1);
    ASSERT_TRUE(wp_config_parse((const u8 *)"metrics=screen\n", 15, &cfg) == WP_OK);
    ASSERT_EQ_U32(cfg.paper_metrics, 0);
}
