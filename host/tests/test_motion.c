#include <string.h>
#include "test.h"
#include "editing.h"

/* Builds a single-section, single-paragraph, single-run document. */
static void build_single_run_doc(wp_document *doc, const char *text, wp_paragraph **out_para)
{
    u32 sec_idx, para_idx;
    wp_section *sec;
    wp_span span;
    u16 run_idx;

    ASSERT_TRUE(wp_document_init(doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(doc, sec_idx);
    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    *out_para = wp_section_paragraph_get(sec, para_idx);

    ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(*out_para, span, doc->default_cstyle, &run_idx) == WP_OK);
}

/* Adds a second paragraph with the given text (possibly empty, if text
   is ""). */
static void add_paragraph(wp_document *doc, const char *text)
{
    wp_section *sec = wp_document_section_get(doc, 0);
    u32 para_idx;
    wp_paragraph *para;

    ASSERT_TRUE(wp_section_paragraph_add(sec, &para_idx) == WP_OK);
    if (text[0] != '\0') {
        wp_span span;
        u16 run_idx;

        para = wp_section_paragraph_get(sec, para_idx);
        ASSERT_TRUE(wp_strpool_append(&doc->strings, (const u8 *)text, (u32)strlen(text), &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx) == WP_OK);
    }
}

static wp_pos pos_at(u32 section, u32 paragraph, u32 run, u32 offset)
{
    wp_pos p;
    p.section = section; p.paragraph = paragraph; p.run = run; p.offset = offset;
    return p;
}

void test_motion_word_forward_skips_word_then_blank(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "Hello World", &para);

    pos = wp_pos_word_forward(&doc, pos_at(0, 0, 0, 0), WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 6); /* start of "World" */

    pos = wp_pos_word_forward(&doc, pos, WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 11); /* doc end, clamped */

    wp_document_free(&doc);
}

void test_motion_word_forward_stops_at_punctuation_boundary(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "foo.bar baz", &para);

    /* small w: word/punct/blank are three distinct classes */
    pos = wp_pos_word_forward(&doc, pos_at(0, 0, 0, 0), WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 3); /* stops right at "." */

    pos = wp_pos_word_forward(&doc, pos, WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 4); /* "." is its own punct run -> lands on "bar" */

    wp_document_free(&doc);
}

void test_motion_word_forward_big_word_merges_punct(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "foo.bar baz", &para);

    /* big W: WORD/PUNCT merge, only blanks separate -- jumps straight to "baz" */
    pos = wp_pos_word_forward(&doc, pos_at(0, 0, 0, 0), WP_TRUE);
    ASSERT_EQ_U32(pos.offset, 8);

    wp_document_free(&doc);
}

void test_motion_word_backward_returns_to_word_starts(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "Hello World", &para);

    pos = wp_pos_word_backward(&doc, pos_at(0, 0, 0, 11), WP_FALSE); /* from doc end */
    ASSERT_EQ_U32(pos.offset, 6); /* start of "World" */

    pos = wp_pos_word_backward(&doc, pos, WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 0); /* start of "Hello" */

    pos = wp_pos_word_backward(&doc, pos, WP_FALSE); /* already at doc start */
    ASSERT_EQ_U32(pos.offset, 0);

    wp_document_free(&doc);
}

void test_motion_word_end_lands_after_last_char_of_word(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "Hello World", &para);

    pos = wp_pos_word_end(&doc, pos_at(0, 0, 0, 0), WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 5); /* right after "Hello" */

    pos = wp_pos_word_end(&doc, pos, WP_FALSE);
    ASSERT_EQ_U32(pos.offset, 11); /* right after "World" -- doc end */

    wp_document_free(&doc);
}

void test_motion_word_forward_crosses_paragraph_boundary(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "Hello", &para);
    add_paragraph(&doc, "World");

    /* a bare word motion (unlike an operator+motion) DOES cross into
       the next paragraph -- the dw-doesn't-eat-the-newline clipping
       rule belongs to the vi-mode operator layer, not this primitive */
    pos = wp_pos_word_forward(&doc, pos_at(0, 0, 0, 0), WP_FALSE);
    ASSERT_EQ_U32(pos.paragraph, 1);
    ASSERT_EQ_U32(pos.offset, 0);

    wp_document_free(&doc);
}

void test_motion_line_start_end_first_nonblank(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos mid, start, end, fnb;

    build_single_run_doc(&doc, "   Hello", &para);
    mid = pos_at(0, 0, 0, 5);

    start = wp_pos_line_start(&doc, mid);
    ASSERT_EQ_U32(start.offset, 0);

    end = wp_pos_line_end(&doc, mid);
    ASSERT_EQ_U32(end.offset, 8);

    fnb = wp_pos_line_first_nonblank(&doc, mid);
    ASSERT_EQ_U32(fnb.offset, 3); /* right after the three leading spaces */

    wp_document_free(&doc);
}

void test_motion_line_first_nonblank_on_all_blank_line_lands_at_end(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos fnb, end;

    build_single_run_doc(&doc, "   ", &para);

    fnb = wp_pos_line_first_nonblank(&doc, pos_at(0, 0, 0, 0));
    end = wp_pos_line_end(&doc, pos_at(0, 0, 0, 0));
    ASSERT_EQ_U32(fnb.offset, end.offset);

    wp_document_free(&doc);
}

void test_motion_paragraph_down_up(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "First", &para);
    add_paragraph(&doc, "Second");
    add_paragraph(&doc, "Third");

    pos = wp_pos_paragraph_down(&doc, pos_at(0, 0, 0, 3));
    ASSERT_EQ_U32(pos.paragraph, 1);
    ASSERT_EQ_U32(pos.offset, 0); /* always lands at paragraph start, not column-preserving */

    pos = wp_pos_paragraph_down(&doc, pos);
    ASSERT_EQ_U32(pos.paragraph, 2);

    pos = wp_pos_paragraph_down(&doc, pos); /* already last -- clamps */
    ASSERT_EQ_U32(pos.paragraph, 2);

    pos = wp_pos_paragraph_up(&doc, pos);
    ASSERT_EQ_U32(pos.paragraph, 1);

    wp_document_free(&doc);
}

void test_motion_para_block_forward_backward_finds_empty_paragraphs(void)
{
    wp_document doc;
    wp_paragraph *para;
    wp_pos pos;

    build_single_run_doc(&doc, "First", &para);
    add_paragraph(&doc, "");       /* paragraph 1: empty */
    add_paragraph(&doc, "Third");

    pos = wp_pos_para_block_forward(&doc, pos_at(0, 0, 0, 0));
    ASSERT_EQ_U32(pos.paragraph, 1);

    pos = wp_pos_para_block_forward(&doc, pos_at(0, 2, 0, 0)); /* none ahead -- doc end */
    ASSERT_EQ_U32(pos.paragraph, 2);

    pos = wp_pos_para_block_backward(&doc, pos_at(0, 2, 0, 0));
    ASSERT_EQ_U32(pos.paragraph, 1);

    pos = wp_pos_para_block_backward(&doc, pos_at(0, 1, 0, 0)); /* none behind -- doc start */
    ASSERT_EQ_U32(pos.paragraph, 0);
    ASSERT_EQ_U32(pos.offset, 0);

    wp_document_free(&doc);
}
