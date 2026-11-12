/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "format_rtf.h"
#include "format_prn.h"
#include "cisummary.h"
#include "pageprint.h"
#include "pageview.h"
#include "psdev.h"

/* The worked example (T16.6): host/tests/fixtures/CHARSHT.RTF, an
   original character sheet using everything M16 built. The gates open
   it on each machine and check the same summary line this test does
   (host/tools/ci_charsheet.sh); here it is also printed through the page
   printer and to PostScript, and saved and read back. */

#define CS_PATH "host/tests/fixtures/CHARSHT.RTF"
#define CS_SUMMARY "doc paras=87 pictures=0 drawings=2 rows=16 frames=1\n"

static u8 g_cs[65536];

static u32 read_sheet(void)
{
    FILE *f = fopen(CS_PATH, "rb");
    u32 n;
    if (f == NULL) return 0;
    n = (u32)fread(g_cs, 1, sizeof(g_cs), f);
    fclose(f);
    return n;
}

static i32 cs_same(i32 t) { return t; }
static wp_status cs_page(void *u) { (void)u; return WP_OK; }

typedef struct { char buf[262144]; u32 n; } cs_out;
static cs_out g_ps;
static wp_status cs_put(void *user, const u8 *b, u32 n)
{
    cs_out *o = (cs_out *)user;
    if (o->n + n < sizeof(o->buf)) { memcpy(o->buf + o->n, b, n); o->n += n; }
    return WP_OK;
}

void test_charsheet_opens_whole(void)
{
    wp_document doc, back;
    char sum[128];
    u32 len, n;
    static u8 out[65536];

    len = read_sheet();
    ASSERT_TRUE(len > 1000);
    ASSERT_TRUE(memcmp(g_cs, "{\\rtf1\\ansi", 11) == 0);   /* RTF as Word writes and reads it */
    ASSERT_TRUE(format_rtf_import(g_cs, len, &doc) == WP_OK);
    n = wp_ci_doc_summary(&doc, sum, (u32)sizeof(sum) - 1);
    sum[n] = '\0';
    ASSERT_EQ_STR(sum, CS_SUMMARY);

    /* Saved and read back, it is the same document. */
    ASSERT_TRUE(format_rtf_export(&doc, out, sizeof(out), &n) == WP_OK);
    ASSERT_TRUE(format_rtf_import(out, n, &back) == WP_OK);
    n = wp_ci_doc_summary(&back, sum, (u32)sizeof(sum) - 1);
    sum[n] = '\0';
    ASSERT_EQ_STR(sum, CS_SUMMARY);
    wp_document_free(&back);
    wp_document_free(&doc);
}

/* One page through the page printer; and to PostScript, its headings in
   the text and its two drawings as paths. */
void test_charsheet_prints(void)
{
    wp_document doc;
    wp_page_device dev;
    wp_prn_options opt;
    u32 pages = 0, len = read_sheet(), drawings = 0;
    const char *p;

    ASSERT_TRUE(format_rtf_import(g_cs, len, &doc) == WP_OK);
    memset(&dev, 0, sizeof(dev));
    dev.gc = (plat_gc *)&dev;
    dev.x_px = cs_same;
    dev.y_px = cs_same;
    dev.end_page = cs_page;
    ASSERT_TRUE(wp_pageprint(&doc, &dev, &pages) == WP_OK);
    ASSERT_EQ_U32(pages, 1);

    memset(&opt, 0, sizeof(opt));
    opt.driver = WP_PRN_POSTSCRIPT;
    g_ps.n = 0;
    ASSERT_TRUE(format_prn_export(&doc, &opt, cs_put, &g_ps, &pages) == WP_OK);
    g_ps.buf[g_ps.n] = '\0';
    ASSERT_TRUE(strstr(g_ps.buf, "ATTRIBUTES AND SKILLS") != NULL);
    ASSERT_TRUE(strstr(g_ps.buf, "BACKGROUND") != NULL);
    for (p = g_ps.buf; (p = strstr(p, "translate 0.1 -0.1 scale")) != NULL; p++) drawings++;
    ASSERT_EQ_U32(drawings, 2);
    wp_document_free(&doc);
}

/* PostScript that matches the screen (psdev.h): one page, the text set
   word by word at the page printer's positions, the rules and the two
   drawings as paths. Left in build/charsheet.ps for a look with gs. */
void test_charsheet_postscript(void)
{
    wp_document doc;
    u32 pages = 0, len = read_sheet();
    FILE *f;

    ASSERT_TRUE(format_rtf_import(g_cs, len, &doc) == WP_OK);
    g_ps.n = 0;
    ASSERT_TRUE(wp_ps_print(&doc, cs_put, &g_ps, 0, &pages) == WP_OK);
    g_ps.buf[g_ps.n] = '\0';
    ASSERT_EQ_U32(pages, 1);
    ASSERT_TRUE(strncmp(g_ps.buf, "%!PS-Adobe-3.0", 14) == 0);
    ASSERT_TRUE(strstr(g_ps.buf, "%%Page: 1 1") != NULL);
    ASSERT_TRUE(strstr(g_ps.buf, "showpage") != NULL);
    ASSERT_TRUE(strstr(g_ps.buf, "%%Pages: 1") != NULL);
    ASSERT_TRUE(strstr(g_ps.buf, "(BACKGROUND)") != NULL);   /* a word, drawn where it was laid out */
    ASSERT_TRUE(strstr(g_ps.buf, "eofill") != NULL);          /* the drawings */
    ASSERT_TRUE(strstr(g_ps.buf, " R\n") != NULL || strstr(g_ps.buf, "R\n") != NULL);   /* rules */
    f = fopen("build/charsheet.ps", "wb");
    if (f != NULL) {
        (void)fwrite(g_ps.buf, 1, g_ps.n, f);
        fclose(f);
    }
    wp_document_free(&doc);
}
