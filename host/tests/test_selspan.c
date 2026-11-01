/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "selspan.h"
#include "layout.h"
#include "format_txt.h"

/* The same four-paragraph, seven-line document test_viewport.c lays out
   (see there for the picture): paragraph 0 wraps into three lines,
   paragraph 1 is blank, paragraph 2 wraps into two, paragraph 3 is one. */
#define SS_TEXT "aaaa bbbb cccc dddd eeee ffff\n\ngggg hhhh iiii jjjj\nkkkk\n"
#define SS_WIDTH_TWIPS 1200

static void build_laid_out_doc(wp_document *doc)
{
    wp_section *sec;
    u32 pi;

    ASSERT_TRUE(format_txt_import((const u8 *)SS_TEXT, (u32)strlen(SS_TEXT), doc) == WP_OK);
    sec = wp_document_section_get(doc, 0);
    for (pi = 0; pi < sec->para_count; pi++) {
        ASSERT_TRUE(wp_paragraph_relayout(doc, wp_section_paragraph_get(sec, pi), SS_WIDTH_TWIPS) == WP_OK);
    }
    ASSERT_EQ_U32(((wp_paragraph_layout *)wp_section_paragraph_get(sec, 0)->layout)->line_count, 3);
}

static const wp_line *line_of(wp_document *doc, u32 paragraph, u32 li)
{
    wp_section *sec = wp_document_section_get(doc, 0);
    wp_paragraph *para = wp_section_paragraph_get(sec, paragraph);

    return &((wp_paragraph_layout *)para->layout)->lines[li];
}

static wp_selection sel_between(u32 p0, u32 o0, u32 p1, u32 o1)
{
    wp_selection s;

    s.anchor.section = 0; s.anchor.paragraph = p0; s.anchor.run = 0; s.anchor.offset = o0;
    s.caret.section = 0;  s.caret.paragraph = p1;  s.caret.run = 0;  s.caret.offset = o1;
    return s;
}

void test_selspan_marks_the_lines_a_selection_crosses(void)
{
    wp_document doc;
    wp_selection sel;
    wp_pos from, to;
    const wp_line *l;
    int pass;

    build_laid_out_doc(&doc);

    /* Forwards and backwards must agree. */
    for (pass = 0; pass < 2; pass++) {
        sel = (pass == 0) ? sel_between(0, 2, 2, 2) : sel_between(2, 2, 0, 2);

        /* First line: from the selection's start to the line's end. */
        l = line_of(&doc, 0, 0);
        ASSERT_TRUE(wp_selspan_for_line(sel, 0, 0, l, &from, &to));
        ASSERT_EQ_U32(from.offset, 2);
        ASSERT_EQ_U32(to.offset, l->end_offset);
        ASSERT_EQ_U32(to.paragraph, 0);

        /* The wrapped lines in between: whole. */
        l = line_of(&doc, 0, 1);
        ASSERT_TRUE(wp_selspan_for_line(sel, 0, 0, l, &from, &to));
        ASSERT_EQ_U32(from.offset, l->start_offset);
        ASSERT_EQ_U32(to.offset, l->end_offset);
        ASSERT_TRUE(l->start_offset > 2);

        l = line_of(&doc, 0, 2);
        ASSERT_TRUE(wp_selspan_for_line(sel, 0, 0, l, &from, &to));
        ASSERT_EQ_U32(from.offset, l->start_offset);
        ASSERT_EQ_U32(to.offset, l->end_offset);

        /* The blank paragraph has nothing to paint. */
        ASSERT_TRUE(!wp_selspan_for_line(sel, 0, 1, line_of(&doc, 1, 0), &from, &to));

        /* Last line: from its start to the selection's end. */
        l = line_of(&doc, 2, 0);
        ASSERT_TRUE(wp_selspan_for_line(sel, 0, 2, l, &from, &to));
        ASSERT_EQ_U32(from.offset, 0);
        ASSERT_EQ_U32(to.offset, 2);
        ASSERT_EQ_U32(to.paragraph, 2);

        /* Beyond it: nothing. */
        ASSERT_TRUE(!wp_selspan_for_line(sel, 0, 2, line_of(&doc, 2, 1), &from, &to));
        ASSERT_TRUE(!wp_selspan_for_line(sel, 0, 3, line_of(&doc, 3, 0), &from, &to));
        /* A different section is never selected. */
        ASSERT_TRUE(!wp_selspan_for_line(sel, 1, 0, line_of(&doc, 0, 0), &from, &to));
    }

    wp_document_free(&doc);
}

void test_selspan_within_one_line(void)
{
    wp_document doc;
    wp_selection sel = sel_between(0, 2, 0, 5);
    wp_pos from, to;

    build_laid_out_doc(&doc);

    ASSERT_TRUE(wp_selspan_for_line(sel, 0, 0, line_of(&doc, 0, 0), &from, &to));
    ASSERT_EQ_U32(from.offset, 2);
    ASSERT_EQ_U32(to.offset, 5);
    ASSERT_TRUE(!wp_selspan_for_line(sel, 0, 0, line_of(&doc, 0, 1), &from, &to));
    ASSERT_TRUE(!wp_selspan_for_line(sel, 0, 0, line_of(&doc, 0, 2), &from, &to));

    wp_document_free(&doc);
}

/* A selection that begins exactly where a line ends has nothing on that
   line: the first selected character is on the next one. */
void test_selspan_starting_at_a_line_end_shows_nothing_there(void)
{
    wp_document doc;
    const wp_line *l0, *l1;
    wp_selection sel;
    wp_pos from, to;

    build_laid_out_doc(&doc);
    l0 = line_of(&doc, 0, 0);
    l1 = line_of(&doc, 0, 1);
    sel = sel_between(0, l0->end_offset, 0, l1->start_offset + 3);

    ASSERT_TRUE(!wp_selspan_for_line(sel, 0, 0, l0, &from, &to));
    ASSERT_TRUE(wp_selspan_for_line(sel, 0, 0, l1, &from, &to));
    ASSERT_EQ_U32(from.offset, l1->start_offset);
    ASSERT_EQ_U32(to.offset, l1->start_offset + 3);

    wp_document_free(&doc);
}
