/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "merge.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "layout.h"
#include "pageview.h"
#include "strpool.h"

/* Mail merge (merge.h). */

static void cell(const wp_merge_data *d, u32 row, u32 col, char *out)
{
    u32 k = row * d->fields + col, n = d->cell_len[k];
    memcpy(out, d->bytes + d->cell_at[k], n);
    out[n] = '\0';
}

void test_merge_finds_fields(void)
{
    static const char t[] = "Dear \xc2\xab" "First Name\xc2\xbb, \xc2\xab\xc2\xbb \xc2\xab" "a\nb\xc2\xbb \xc2\xabX\xc2\xbb";
    u32 s, e, na, nl;

    ASSERT_TRUE(wp_merge_next_field((const u8 *)t, (u32)strlen(t), 0, &s, &e, &na, &nl));
    ASSERT_EQ_U32(s, 5);
    ASSERT_EQ_U32(nl, 10);
    ASSERT_TRUE(memcmp(t + na, "First Name", 10) == 0);
    /* «» names nothing, a name never crosses a line: the next is «X». */
    ASSERT_TRUE(wp_merge_next_field((const u8 *)t, (u32)strlen(t), e, &s, &e, &na, &nl));
    ASSERT_EQ_U32(nl, 1);
    ASSERT_EQ_U32((u32)t[na], 'X');
    ASSERT_TRUE(!wp_merge_next_field((const u8 *)t, (u32)strlen(t), e, &s, &e, &na, &nl));
}

/* Commas; a quoted field holding a comma, a line break and a doubled
   quote; CRLF; a blank line skipped; a short row's missing cell empty;
   a byte-order mark before the names. */
void test_merge_reads_csv(void)
{
    static const char csv[] = "\xef\xbb\xbfName,Town,Note\r\n"
                              "Ann,\"Bath, Somerset\",\"says \"\"hi\"\"\nthen\"\r\n"
                              "\r\n"
                              "Bob,Leeds\r\n";
    wp_merge_data d;
    char c[64];

    ASSERT_TRUE(wp_merge_data_read((const u8 *)csv, (u32)strlen(csv), &d) == WP_OK);
    ASSERT_EQ_U32(d.fields, 3);
    ASSERT_EQ_U32(d.records, 2);
    cell(&d, 0, 0, c); ASSERT_EQ_STR(c, "Name");
    cell(&d, 1, 1, c); ASSERT_EQ_STR(c, "Bath, Somerset");
    cell(&d, 1, 2, c); ASSERT_EQ_STR(c, "says \"hi\"\nthen");
    cell(&d, 2, 0, c); ASSERT_EQ_STR(c, "Bob");
    cell(&d, 2, 2, c); ASSERT_EQ_STR(c, "");
    ASSERT_EQ_U32((u32)wp_merge_data_column(&d, (const u8 *)" town ", 6), 1);
    ASSERT_TRUE(wp_merge_data_column(&d, (const u8 *)"County", 6) < 0);
    wp_merge_data_free(&d);
}

/* Tabs where the first line has more tabs than commas; nothing but a
   blank file has no names. */
void test_merge_reads_tabs(void)
{
    static const char tsv[] = "Name\tPrice, each\nWidget\t2,50\n";
    wp_merge_data d;
    char c[64];

    ASSERT_TRUE(wp_merge_data_read((const u8 *)tsv, (u32)strlen(tsv), &d) == WP_OK);
    ASSERT_EQ_U32(d.fields, 2);
    ASSERT_EQ_U32(d.records, 1);
    cell(&d, 1, 1, c); ASSERT_EQ_STR(c, "2,50");
    wp_merge_data_free(&d);
    ASSERT_TRUE(wp_merge_data_read((const u8 *)"\n\n", 2, &d) == WP_BADFMT);
}

/* Two records: the document twice, the second a section of its own (a
   new page); each field its record's value, a name in any case; a
   field the data lacks left to be seen. */
void test_merge_document(void)
{
    static const char tmpl[] = "Dear \xc2\xabname\xc2\xbb,\nYour \xc2\xab" "COLOUR\xc2\xbb \xc2\xabsize\xc2\xbb hat.\n";
    static const char csv[] = "Name,Colour\nAnn,red\nBob,blue\n";
    wp_document doc;
    wp_merge_data d;
    u32 copies = 0;
    char out[256];
    u32 n = 0;

    ASSERT_TRUE(format_txt_import((const u8 *)tmpl, (u32)strlen(tmpl), &doc) == WP_OK);
    ASSERT_TRUE(wp_merge_data_read((const u8 *)csv, (u32)strlen(csv), &d) == WP_OK);
    ASSERT_TRUE(wp_merge_document(&doc, &d, &copies) == WP_OK);
    ASSERT_EQ_U32(copies, 2);
    ASSERT_EQ_U32(doc.section_count, 2);
    ASSERT_TRUE(format_txt_export(&doc, (u8 *)out, sizeof(out) - 1, &n) == WP_OK);
    out[n] = '\0';
    ASSERT_EQ_STR(out, "Dear Ann,\nYour red \xc2\xabsize\xc2\xbb hat.\n"
                       "Dear Bob,\nYour blue \xc2\xabsize\xc2\xbb hat.\n");
    wp_merge_data_free(&d);
    wp_document_free(&doc);
}

/* In RTF a field is Word's MERGEFIELD, its shown result «Name» -- and
   read back, the same text. */
void test_merge_fields_in_rtf(void)
{
    static const char tmpl[] = "Dear \xc2\xab" "First Name\xc2\xbb and \xc2\xabx\xc2\xbb.\n";
    static u8 rtf[4096];
    wp_document doc, back;
    u32 n = 0;
    char out[128];

    ASSERT_TRUE(format_txt_import((const u8 *)tmpl, (u32)strlen(tmpl), &doc) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, rtf, sizeof(rtf) - 1, &n) == WP_OK);
    rtf[n] = '\0';
    ASSERT_TRUE(strstr((const char *)rtf, "{\\field{\\*\\fldinst MERGEFIELD \"First Name\" }{\\fldrslt \\'abFirst Name\\'bb}}") != NULL);
    ASSERT_TRUE(strstr((const char *)rtf, "MERGEFIELD x }") != NULL);
    ASSERT_TRUE(format_rtf_import(rtf, n, &back) == WP_OK);
    ASSERT_TRUE(format_txt_export(&back, (u8 *)out, sizeof(out) - 1, &n) == WP_OK);
    out[n] = '\0';
    ASSERT_EQ_STR(out, tmpl);
    wp_document_free(&back);
    wp_document_free(&doc);
}

static i32 same_px(i32 t) { return t; }

/* Labels: five records, two across and three down on a 6 x 9 inch
   column (an 8.5 x 11 page, margins 1.25 and 1) -- a table of three rows,
   each label 4320 twips wide and exactly 4320 tall, the last row
   filled out with an empty label; each row laid out at that pitch. */
void test_merge_labels(void)
{
    static const char tmpl[] = "\xc2\xabName\xc2\xbb\n\xc2\xabTown\xc2\xbb\n";
    static const char csv[] = "Name,Town\nAnn,Bath\nBob,Leeds\nCy,York\nDee,Ely\nEd,Hull\n";
    wp_document doc;
    wp_merge_data d;
    wp_section *sec;
    const wp_table_row *row;
    u32 copies = 0, pi;
    wp_paragraph *p;

    ASSERT_TRUE(format_txt_import((const u8 *)tmpl, (u32)strlen(tmpl), &doc) == WP_OK);
    sec = wp_document_section_get(&doc, 0);
    sec->page_width = 12240; sec->page_height = 15840;
    sec->margin_left = sec->margin_right = 1800;
    sec->margin_top = sec->margin_bottom = 1440;
    ASSERT_TRUE(wp_merge_data_read((const u8 *)csv, (u32)strlen(csv), &d) == WP_OK);
    ASSERT_TRUE(wp_merge_labels(&doc, &d, 2, 3, &copies) == WP_OK);
    ASSERT_EQ_U32(copies, 5);
    sec = wp_document_section_get(&doc, 0);
    ASSERT_EQ_U32(sec->para_count, 5 * 2 + 1);   /* five labels of two lines, one empty */
    p = wp_section_paragraph_get(sec, 0);
    row = wp_document_trow_get(&doc, p->trow);
    ASSERT_TRUE(row != NULL);
    if (row != NULL) {
        ASSERT_EQ_U32(row->cell_count, 2);
        ASSERT_EQ_U32((u32)row->cell_right[0], 4320);
        ASSERT_EQ_U32((u32)row->cell_right[1], 8640);
        ASSERT_EQ_U32((u32)-row->height, 4320);
    }
    /* Ann's label: two paragraphs, the second ends the cell; Bob's ends
       the row; the empty last label ends the third. */
    ASSERT_EQ_U32(wp_section_paragraph_get(sec, 0)->tflags, 0);
    ASSERT_EQ_U32(wp_section_paragraph_get(sec, 1)->tflags, WP_TCELL_END);
    ASSERT_EQ_U32(wp_section_paragraph_get(sec, 3)->tflags, WP_TCELL_END | WP_TROW_END);
    ASSERT_EQ_U32(wp_section_paragraph_get(sec, 10)->tflags, WP_TCELL_END | WP_TROW_END);
    {
        wp_run *r = wp_paragraph_run_get(wp_section_paragraph_get(sec, 8), 0);
        ASSERT_TRUE(r != NULL && r->text.length == 2 && memcmp(wp_strpool_get(&doc.strings, r->text), "Ed", 2) == 0);
    }
    /* Laid out: every row exactly the label's height. */
    for (pi = 0; pi < sec->para_count; pi++) wp_section_paragraph_get(sec, pi)->layout_dirty = WP_TRUE;
    (void)wp_pageview_relayout(&doc, 12240);
    for (pi = 0; pi < sec->para_count; pi += 4) {
        const wp_paragraph_layout *l = (const wp_paragraph_layout *)wp_section_paragraph_get(sec, pi)->layout;
        ASSERT_TRUE(l != NULL && l->line_count > 0 && l->lines[0].row_h_twips == 4320);
    }
    /* Walked as every painter and the printer walk it: the second row
       starts a label's height below the first, whatever its text. */
    {
        wp_line_walk w;
        i32 cy_top = -1;
        wp_line_walk_start(&w, 0);
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph_layout *l = (const wp_paragraph_layout *)wp_section_paragraph_get(sec, pi)->layout;
            u32 li;
            for (li = 0; l != NULL && li < l->line_count; li++) {
                i32 top = wp_line_walk_next(&w, &l->lines[li], same_px);
                if (pi == 4 && li == 0) cy_top = top;
            }
        }
        ASSERT_EQ_U32((u32)cy_top, 4320);
        ASSERT_EQ_U32((u32)wp_table_row_extent(sec, 0, same_px), 4320);
    }
    wp_merge_data_free(&d);
    wp_document_free(&doc);

    /* A label holding a table is refused, nothing changed; so are 17 across. */
    ASSERT_TRUE(format_txt_import((const u8 *)tmpl, (u32)strlen(tmpl), &doc) == WP_OK);
    ASSERT_TRUE(wp_merge_data_read((const u8 *)csv, (u32)strlen(csv), &d) == WP_OK);
    ASSERT_TRUE(wp_merge_labels(&doc, &d, 17, 3, &copies) == WP_RANGE);
    wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0)->trow = 1;
    ASSERT_TRUE(wp_merge_labels(&doc, &d, 2, 3, &copies) == WP_UNSUPPORTED);
    ASSERT_EQ_U32(wp_document_section_get(&doc, 0)->para_count, 2);
    wp_merge_data_free(&d);
    wp_document_free(&doc);
}

/* A row's height goes to RTF as \trrh and comes back. */
void test_merge_row_height_in_rtf(void)
{
    static const char tmpl[] = "\xc2\xabName\xc2\xbb\n";
    static const char csv[] = "Name\nAnn\n";
    static u8 rtf[8192];
    wp_document doc, back;
    wp_merge_data d;
    u32 copies, n = 0;

    ASSERT_TRUE(format_txt_import((const u8 *)tmpl, (u32)strlen(tmpl), &doc) == WP_OK);
    ASSERT_TRUE(wp_merge_data_read((const u8 *)csv, (u32)strlen(csv), &d) == WP_OK);
    ASSERT_TRUE(wp_merge_labels(&doc, &d, 3, 10, &copies) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, rtf, sizeof(rtf) - 1, &n) == WP_OK);
    rtf[n] = '\0';
    ASSERT_TRUE(strstr((const char *)rtf, "\\trrh-") != NULL);
    ASSERT_TRUE(format_rtf_import(rtf, n, &back) == WP_OK);
    {
        const wp_paragraph *p = wp_section_paragraph_get(wp_document_section_get(&back, 0), 0);
        const wp_table_row *row = wp_document_trow_get(&back, p->trow);
        const wp_table_row *orig = wp_document_trow_get(&doc, wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0)->trow);
        ASSERT_TRUE(row != NULL && orig != NULL && row->height == orig->height && row->height < 0);
    }
    wp_merge_data_free(&d);
    wp_document_free(&back);
    wp_document_free(&doc);
}
