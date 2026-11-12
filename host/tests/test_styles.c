/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "styles.h"
#include "editing.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "strpool.h"

/* Named paragraph styles (styles.h). */

static const wp_para_style *pfmt(wp_document *doc, u32 pi)
{
    return wp_document_pstyle_get(doc, wp_section_paragraph_get(wp_document_section_get(doc, 0), pi)->style);
}

static const wp_char_style *cfmt(wp_document *doc, u32 pi, u16 ri)
{
    wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(doc, 0), pi);
    return wp_document_cstyle_get(doc, wp_paragraph_run_get(p, ri)->style);
}

/* A built-in comes into the document the first time it is used, and a
   paragraph put in it takes its formats; nothing is added before. */
void test_styles_builtin_and_apply(void)
{
    wp_document doc;
    u16 h1 = 0, again = 0;

    ASSERT_TRUE(format_txt_import((const u8 *)"Intro\nBody text\n", 16, &doc) == WP_OK);
    ASSERT_EQ_U32(doc.named_count, 0);
    ASSERT_TRUE(wp_style_ensure(&doc, (const u8 *)"heading 1", 9, &h1) == WP_OK);   /* any case */
    ASSERT_TRUE(h1 != 0);
    ASSERT_TRUE(wp_style_ensure(&doc, (const u8 *)"Heading 1", 9, &again) == WP_OK);
    ASSERT_EQ_U32(again, h1);
    ASSERT_TRUE(wp_style_ensure(&doc, (const u8 *)"Heading 9", 9, &again) == WP_RANGE);
    ASSERT_TRUE(wp_style_apply(&doc, 0, 0, h1) == WP_OK);
    ASSERT_EQ_U32(pfmt(&doc, 0)->style_id, h1);
    ASSERT_EQ_U32((u32)pfmt(&doc, 0)->space_before, 240);
    ASSERT_TRUE((cfmt(&doc, 0, 0)->flags & WP_STYLE_BOLD) != 0);
    ASSERT_EQ_U32((u32)cfmt(&doc, 0, 0)->size_twips, 320);
    ASSERT_EQ_U32(pfmt(&doc, 1)->style_id, 0);
    wp_document_free(&doc);
}

/* Define Style: a new style from a paragraph's look; redefined, every
   paragraph in it takes the new look. */
void test_styles_define_and_redefine(void)
{
    wp_document doc;
    wp_para_style ps;
    wp_char_style cs;
    u16 id = 0, id2 = 0;

    ASSERT_TRUE(format_txt_import((const u8 *)"A\nB\nC\n", 6, &doc) == WP_OK);
    ps = *pfmt(&doc, 0);
    cs = *cfmt(&doc, 0, 0);
    ps.align = WP_ALIGN_CENTER;
    cs.flags = WP_STYLE_ITALIC;
    ASSERT_TRUE(wp_style_define(&doc, (const u8 *)"Caption", 7, &ps, &cs, &id) == WP_OK);
    ASSERT_TRUE(wp_style_apply(&doc, 0, 0, id) == WP_OK);
    ASSERT_TRUE(wp_style_apply(&doc, 0, 2, id) == WP_OK);
    ps.align = WP_ALIGN_RIGHT;
    cs.flags = WP_STYLE_BOLD;
    ASSERT_TRUE(wp_style_define(&doc, (const u8 *)" caption ", 9, &ps, &cs, &id2) == WP_OK);
    ASSERT_EQ_U32(id2, id);                         /* the same style, redefined */
    ASSERT_EQ_U32(pfmt(&doc, 0)->align, WP_ALIGN_RIGHT);
    ASSERT_EQ_U32(pfmt(&doc, 2)->align, WP_ALIGN_RIGHT);
    ASSERT_EQ_U32(cfmt(&doc, 2, 0)->flags, WP_STYLE_BOLD);
    ASSERT_EQ_U32(pfmt(&doc, 1)->align, WP_ALIGN_LEFT);   /* not in it */
    wp_document_free(&doc);
}

/* Enter at the end of a heading starts a Normal paragraph; in the
   middle it splits the heading; text typed in an empty paragraph in a
   style takes the style's character format. */
void test_styles_next_style_and_typing(void)
{
    wp_document doc;
    wp_undo_journal j;
    u16 h2 = 0;
    wp_pos at, out;
    u32 nlen = 0;
    const u8 *nm;

    ASSERT_TRUE(format_txt_import((const u8 *)"Heading\n", 8, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    ASSERT_TRUE(wp_style_ensure(&doc, (const u8 *)"Heading 2", 9, &h2) == WP_OK);
    ASSERT_TRUE(wp_style_apply(&doc, 0, 0, h2) == WP_OK);
    memset(&at, 0, sizeof(at));
    at.offset = 7;
    ASSERT_TRUE(wp_edit_split_paragraph(&doc, &j, at, &out) == WP_OK);
    nm = wp_style_name(&doc, pfmt(&doc, 1)->style_id, &nlen);
    ASSERT_TRUE(nm != NULL && nlen == 6 && memcmp(nm, "Normal", 6) == 0);
    at.offset = 3;
    ASSERT_TRUE(wp_edit_split_paragraph(&doc, &j, at, &out) == WP_OK);
    ASSERT_EQ_U32(pfmt(&doc, 1)->style_id, h2);    /* mid-heading: both halves headings */

    /* An empty Heading 2 paragraph: typed text is bold, 14 pt. */
    ASSERT_TRUE(wp_style_apply(&doc, 0, 2, h2) == WP_OK);
    memset(&at, 0, sizeof(at));
    at.paragraph = 2;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &j, at, (const u8 *)"x", 1, &out) == WP_OK);
    ASSERT_TRUE((cfmt(&doc, 2, 0)->flags & WP_STYLE_BOLD) != 0);
    ASSERT_EQ_U32((u32)cfmt(&doc, 2, 0)->size_twips, 280);
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}

/* RTF: a \stylesheet and each paragraph's \sN -- and read back, the same
   styles on the same paragraphs, their formats from their paragraphs. */
void test_styles_rtf_round_trip(void)
{
    static u8 rtf[8192];
    wp_document doc, back;
    u16 t = 0, q = 0;
    u32 n = 0, len;
    const u8 *nm;

    ASSERT_TRUE(format_txt_import((const u8 *)"My Title\nSaid she.\nPlain\n", 25, &doc) == WP_OK);
    ASSERT_TRUE(wp_style_ensure(&doc, (const u8 *)"Title", 5, &t) == WP_OK);
    ASSERT_TRUE(wp_style_ensure(&doc, (const u8 *)"Quotation", 9, &q) == WP_OK);
    ASSERT_TRUE(wp_style_apply(&doc, 0, 0, t) == WP_OK);
    ASSERT_TRUE(wp_style_apply(&doc, 0, 1, q) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, rtf, sizeof(rtf) - 1, &n) == WP_OK);
    rtf[n] = '\0';
    ASSERT_TRUE(strstr((const char *)rtf, "{\\stylesheet{\\s0 Normal;}") != NULL);
    ASSERT_TRUE(strstr((const char *)rtf, " Title;}") != NULL);
    ASSERT_TRUE(strstr((const char *)rtf, "\\pard\\s1\\qc") != NULL);
    ASSERT_TRUE(format_rtf_import(rtf, n, &back) == WP_OK);
    nm = wp_style_name(&back, pfmt(&back, 0)->style_id, &len);
    ASSERT_TRUE(nm != NULL && len == 5 && memcmp(nm, "Title", 5) == 0);
    ASSERT_EQ_U32(pfmt(&back, 0)->align, WP_ALIGN_CENTER);
    nm = wp_style_name(&back, pfmt(&back, 1)->style_id, &len);
    ASSERT_TRUE(nm != NULL && len == 9 && memcmp(nm, "Quotation", 9) == 0);
    ASSERT_EQ_U32((u32)pfmt(&back, 1)->indent_left, 720);
    ASSERT_EQ_U32(pfmt(&back, 2)->style_id, 0);
    wp_document_free(&back);
    wp_document_free(&doc);
}

/* Word's own stylesheet: entries with \snext and \sbasedon, a character
   style (\cs) that is no paragraph style, names in lower case. */
void test_styles_rtf_from_word(void)
{
    static const char rtf[] =
        "{\\rtf1\\ansi{\\fonttbl{\\f0 Times;}}"
        "{\\stylesheet{\\ql\\fs24 \\snext0 Normal;}{\\s1\\sb240\\b\\fs32 \\sbasedon0\\snext0 heading 1;}"
        "{\\*\\cs10 \\additive Default Paragraph Font;}}"
        "\\pard\\plain\\s1\\sb240\\b\\fs32 Chapter\\par"
        "\\pard\\plain\\fs24 Text\\par}";
    wp_document doc;
    u32 len;
    const u8 *nm;

    ASSERT_TRUE(format_rtf_import((const u8 *)rtf, (u32)strlen(rtf), &doc) == WP_OK);
    nm = wp_style_name(&doc, pfmt(&doc, 0)->style_id, &len);
    ASSERT_TRUE(nm != NULL && len == 9 && memcmp(nm, "heading 1", 9) == 0);
    ASSERT_EQ_U32((u32)pfmt(&doc, 0)->space_before, 240);
    ASSERT_EQ_U32(pfmt(&doc, 1)->style_id, 0);
    ASSERT_EQ_U32(wp_style_find(&doc, (const u8 *)"Default Paragraph Font", 22), 0);
    wp_document_free(&doc);
}
