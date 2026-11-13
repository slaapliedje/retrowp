/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "notes.h"
#include "editing.h"
#include "format_txt.h"
#include "strpool.h"
#include "layout.h"
#include "pageview.h"
#include "format_rtf.h"

/* Footnotes (notes.h). */

static wp_paragraph *para_at(wp_document *doc, u32 pi)
{
    return wp_section_paragraph_get(wp_document_section_get(doc, 0), pi);
}

static void ptext(wp_document *doc, u32 pi, char *out, u32 cap)
{
    wp_paragraph *p = para_at(doc, pi);
    u32 n = 0;
    u16 ri;
    for (ri = 0; ri < p->run_count; ri++) {
        wp_run *r = wp_paragraph_run_get(p, ri);
        if (n + r->text.length + 1 >= cap) break;
        memcpy(out + n, wp_strpool_get(&doc->strings, r->text), r->text.length);
        n += r->text.length;
    }
    out[n] = '\0';
}

/* A footnote at the caret: a raised "1" there, and after the paragraph
   a note in the Footnote Text style starting "1 ", the caret after it;
   text typed there is the note's. */
void test_notes_insert_and_type(void)
{
    wp_document doc;
    wp_undo_journal j;
    wp_pos at, note, out;
    char t[64];
    const wp_char_style *cs;

    ASSERT_TRUE(format_txt_import((const u8 *)"Alpha beta.\nNext.\n", 18, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 5;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Alpha1 beta.");
    cs = wp_document_cstyle_get(&doc, wp_paragraph_run_get(para_at(&doc, 0), 1)->style);
    ASSERT_TRUE(cs != NULL && cs->note != 0 && (cs->flags & WP_STYLE_SUPER));
    ASSERT_EQ_U32(doc.sections[0].para_count, 3);
    ASSERT_TRUE(para_at(&doc, 1)->note == cs->note);
    ASSERT_EQ_U32(note.paragraph, 1);
    ASSERT_TRUE(wp_edit_insert_text(&doc, &j, note, (const u8 *)"See p. 4.", 9, &out) == WP_OK);
    ptext(&doc, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "1 See p. 4.");
    ptext(&doc, 2, t, sizeof(t));
    ASSERT_EQ_STR(t, "Next.");

    /* Undone: the note and its reference gone. */
    while (wp_edit_undo(&doc, &j) == WP_OK) {}
    ASSERT_EQ_U32(doc.sections[0].para_count, 2);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Alpha beta.");
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}

/* Numbered by where their references stand: a second note before the
   first makes it 1 and the first 2, in the text and in the notes; a
   note whose reference is deleted shows "*". */
void test_notes_renumber(void)
{
    wp_document doc;
    wp_undo_journal j;
    wp_pos at, note;
    char t[64];
    u32 pi;

    ASSERT_TRUE(format_txt_import((const u8 *)"One two three.\n", 15, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 9;   /* after "One two t" -- inside "three" is fine */
    at.offset = 7;   /* after "One two" */
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    memset(&at, 0, sizeof(at));
    at.offset = 3;   /* after "One" */
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "One1 two2 three.");
    /* the notes in their paragraphs: the newer, referred to first, is 1 */
    for (pi = 1; pi < doc.sections[0].para_count; pi++) {
        ptext(&doc, pi, t, sizeof(t));
        ASSERT_TRUE(t[0] == '1' || t[0] == '2');
    }
    /* The first reference deleted: its note shows "*". */
    /* runs: "One", "1", " two", "2", " three." -- the "1" is run 1 */
    ASSERT_TRUE(wp_edit_delete_range(&doc, &j, 0, 0, 1, 0, 1) == WP_OK);
    ASSERT_TRUE(wp_notes_renumber(&doc) == WP_OK);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "One two1 three.");
    {
        u32 stars = 0;
        for (pi = 1; pi < doc.sections[0].para_count; pi++) {
            ptext(&doc, pi, t, sizeof(t));
            if (t[0] == '*') stars++;
        }
        ASSERT_EQ_U32(stars, 1);
    }
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}

/* Laid out, a note goes to the foot of its page -- the text's room
   ending above it -- and is drawn there like a frame. */
void test_notes_at_page_foot(void)
{
    wp_document doc;
    wp_undo_journal j;
    wp_pos at, note;
    wp_section *sec;
    const wp_paragraph_layout *l;
    i32 content;

    ASSERT_TRUE(format_txt_import((const u8 *)"Text\nMore\n", 10, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 4;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    (void)wp_pageview_relayout(&doc, 0);
    sec = wp_document_section_get(&doc, 0);
    content = sec->page_height - sec->margin_top - sec->margin_bottom;
    l = (const wp_paragraph_layout *)para_at(&doc, 1)->layout;
    ASSERT_TRUE(l != NULL && l->line_count > 0);
    if (l != NULL && l->line_count > 0) {
        ASSERT_TRUE((l->lines[0].flags & WP_LINE_FRAME) != 0);
        ASSERT_EQ_U32(l->lines[0].advance_twips, 0);
        ASSERT_EQ_U32((u32)(l->lines[0].page_y_twips + l->lines[0].height_twips), (u32)content);
    }
    /* "More" follows the text, not the note. */
    l = (const wp_paragraph_layout *)para_at(&doc, 2)->layout;
    ASSERT_TRUE(l != NULL && l->line_count > 0 && l->lines[0].page_y_twips < content / 2);
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}

/* RTF: Word's form -- the mark, then its \footnote group -- and read
   back, the same reference and note, numbered. */
void test_notes_rtf_round_trip(void)
{
    static u8 rtf[8192];
    wp_document doc, back;
    wp_undo_journal j;
    wp_pos at, note, out;
    u32 n = 0;
    char t[64];

    ASSERT_TRUE(format_txt_import((const u8 *)"Claim.\nAfter.\n", 14, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 5;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    ASSERT_TRUE(wp_edit_insert_text(&doc, &j, note, (const u8 *)"A source.", 9, &out) == WP_OK);
    ASSERT_TRUE(format_rtf_export(&doc, rtf, sizeof(rtf) - 1, &n) == WP_OK);
    rtf[n] = '\0';
    ASSERT_TRUE(strstr((const char *)rtf, "{\\super\\chftn}{\\footnote") != NULL);
    ASSERT_TRUE(strstr((const char *)rtf, "A source.") != NULL);
    ASSERT_TRUE(format_rtf_import(rtf, n, &back) == WP_OK);
    ASSERT_TRUE(wp_notes_renumber(&back) == WP_OK);
    ASSERT_EQ_U32(back.sections[0].para_count, 3);
    ptext(&back, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Claim1.");
    ASSERT_TRUE(para_at(&back, 1)->note != 0);
    ptext(&back, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "1 A source.");
    ptext(&back, 2, t, sizeof(t));
    ASSERT_EQ_STR(t, "After.");
    wp_undo_journal_free(&j);
    wp_document_free(&back);
    wp_document_free(&doc);
}

/* A footnote as Word writes one: its own paragraph formatting and mark
   inside, a paragraph break in it -- read as one plain note. */
void test_notes_rtf_from_word(void)
{
    static const char rtf[] =
        "{\\rtf1\\ansi{\\fonttbl{\\f0 Times;}}"
        "\\pard\\plain Fact{\\cs15\\super\\chftn}{\\footnote\\pard\\plain\\s16\\fs20 "
        "{\\cs15\\super\\chftn} First part.\\par Second.}"
        " stands.\\par}";
    wp_document doc;
    char t[64];

    ASSERT_TRUE(format_rtf_import((const u8 *)rtf, (u32)strlen(rtf), &doc) == WP_OK);
    ASSERT_TRUE(wp_notes_renumber(&doc) == WP_OK);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Fact1 stands.");
    ptext(&doc, 1, t, sizeof(t));
    ASSERT_EQ_STR(t, "1 First part. Second.");
    wp_document_free(&doc);
}

/* Text typed right after a reference, or with the caret inside its
   number, is the text's own: plain, after the number, and renumbering
   leaves it be. */
void test_notes_typing_beside_reference(void)
{
    wp_document doc;
    wp_undo_journal j;
    wp_pos at, note, out, in;
    char t[64];
    const wp_char_style *cs;

    ASSERT_TRUE(format_txt_import((const u8 *)"Word end.\n", 10, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 4;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    /* right after the "1" (run 1, its end) */
    memset(&in, 0, sizeof(in));
    in.run = 1;
    in.offset = 1;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &j, in, (const u8 *)"s", 1, &out) == WP_OK);
    ASSERT_TRUE(wp_notes_renumber(&doc) == WP_OK);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Word1s end.");
    cs = wp_document_cstyle_get(&doc, wp_paragraph_run_get(para_at(&doc, 0), (u16)out.run)->style);
    ASSERT_TRUE(cs != NULL && cs->note == 0 && (cs->flags & WP_STYLE_SUPER) == 0);
    /* inside the number (offset 0 of run 1 is the boundary; mid needs two
       digits, so the caret at its start types before it, as plain text) */
    in.run = 1;
    in.offset = 0;
    ASSERT_TRUE(wp_edit_insert_text(&doc, &j, in, (const u8 *)"x", 1, &out) == WP_OK);
    ASSERT_TRUE(wp_notes_renumber(&doc) == WP_OK);
    ptext(&doc, 0, t, sizeof(t));
    ASSERT_EQ_STR(t, "Wordx1s end.");
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}

/* Over two pages: a note referred to on page 1 sits at page 1's foot,
   the text before the break kept clear of it, and the text that does
   not fit goes on page 2. */
void test_notes_two_pages(void)
{
    static u8 src[16000];
    wp_document doc;
    wp_undo_journal j;
    wp_pos at, note;
    wp_section *sec;
    const wp_paragraph_layout *nl;
    i32 content, note_top;
    u32 n = 0, k, pi, pages;

    for (k = 0; k < 120 && n + 40 < sizeof(src); k++) {
        memcpy(src + n, "A line of body text that fills pages.\n", 38);
        n += 38;
    }
    ASSERT_TRUE(format_txt_import(src, n, &doc) == WP_OK);
    wp_undo_journal_init(&j);
    memset(&at, 0, sizeof(at));
    at.offset = 6;
    ASSERT_TRUE(wp_notes_insert(&doc, &j, at, &note) == WP_OK);
    pages = wp_pageview_relayout(&doc, 0);
    ASSERT_TRUE(pages >= 2);
    sec = wp_document_section_get(&doc, 0);
    content = sec->page_height - sec->margin_top - sec->margin_bottom;
    nl = (const wp_paragraph_layout *)para_at(&doc, 1)->layout;
    ASSERT_TRUE(nl != NULL && nl->line_count > 0);
    if (nl == NULL || nl->line_count == 0) { wp_document_free(&doc); return; }
    ASSERT_EQ_U32(nl->lines[0].page, 0);
    note_top = nl->lines[0].page_y_twips;
    ASSERT_EQ_U32((u32)(note_top + nl->lines[0].height_twips), (u32)content);
    /* every body line on page 0 ends above the note */
    for (pi = 2; pi < sec->para_count; pi++) {
        const wp_paragraph_layout *l = (const wp_paragraph_layout *)para_at(&doc, pi)->layout;
        if (l == NULL) continue;
        for (k = 0; k < l->line_count; k++) {
            if (l->lines[k].page == 0) ASSERT_TRUE(l->lines[k].page_y_twips + l->lines[k].height_twips <= note_top);
        }
    }
    wp_undo_journal_free(&j);
    wp_document_free(&doc);
}
