#include "test.h"
#include "editing.h"
#include "format_txt.h"

static wp_bool pos_eq(wp_pos a, wp_pos b)
{
    return (wp_bool)(a.section == b.section && a.paragraph == b.paragraph &&
                      a.run == b.run && a.offset == b.offset);
}

void test_cursor_forward_crosses_paragraph_boundary(void)
{
    wp_document doc;
    static const char *text = "First paragraph.\nSecond paragraph.\nThird.\n";
    wp_section *sec;
    wp_paragraph *para0;
    wp_pos p;

    ASSERT_TRUE(format_txt_import((const u8 *)text, 43, &doc) == WP_OK);

    sec = wp_document_section_get(&doc, 0);
    para0 = wp_section_paragraph_get(sec, 0);

    p.section = 0;
    p.paragraph = 0;
    p.run = (u32)(para0->run_count - 1);
    p.offset = para0->runs[p.run].text.length; /* end of paragraph 0 */

    p = wp_pos_next(&doc, p);
    ASSERT_EQ_U32(p.paragraph, 1);
    ASSERT_EQ_U32(p.run, 0);
    ASSERT_EQ_U32(p.offset, 0);

    wp_document_free(&doc);
}

void test_cursor_forward_backward_step_counts_match(void)
{
    wp_document doc;
    static const char *text = "First paragraph.\nSecond paragraph.\nThird.\n";
    wp_pos start, end, pos;
    int forward_steps = 0, backward_steps = 0;

    ASSERT_TRUE(format_txt_import((const u8 *)text, 43, &doc) == WP_OK);

    start = wp_pos_doc_start(&doc);
    end = wp_pos_doc_end(&doc);

    pos = start;
    while (!pos_eq(pos, end) && forward_steps < 1000) {
        wp_pos next = wp_pos_next(&doc, pos);
        ASSERT_TRUE(!pos_eq(next, pos)); /* must always make progress */
        pos = next;
        forward_steps++;
    }
    ASSERT_TRUE(forward_steps < 1000);

    pos = end;
    while (!pos_eq(pos, start) && backward_steps < 1000) {
        pos = wp_pos_prev(&doc, pos);
        backward_steps++;
    }
    ASSERT_TRUE(backward_steps < 1000);

    ASSERT_EQ_U32(forward_steps, backward_steps);

    /* Clamping: next() at doc end / prev() at doc start are no-ops. */
    ASSERT_TRUE(pos_eq(wp_pos_next(&doc, end), end));
    ASSERT_TRUE(pos_eq(wp_pos_prev(&doc, start), start));

    wp_document_free(&doc);
}

/* Builds a document with the traversal edge cases format_txt never
   produces: a multi-run paragraph including a zero-length middle run, a
   wholly empty paragraph (zero runs), and a multi-byte codepoint --
   then proves wp_pos_next/wp_pos_prev are true inverses of each other
   at every step, both directions, over that whole sequence. */
void test_cursor_next_prev_are_true_inverses(void)
{
    wp_document doc;
    u32 sec_idx, p0, p1, p2;
    wp_section *sec;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;
    wp_pos start, end, pos;
    wp_pos visited[32];
    int n = 0, i;

    ASSERT_TRUE(wp_document_init(&doc) == WP_OK);
    ASSERT_TRUE(wp_document_section_add(&doc, &sec_idx) == WP_OK);
    sec = wp_document_section_get(&doc, sec_idx);

    /* para 0: "Hi" + "" (empty run) + "Yo" */
    ASSERT_TRUE(wp_section_paragraph_add(sec, &p0) == WP_OK);
    para = wp_section_paragraph_get(sec, p0);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Hi", 2, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"", 0, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    ASSERT_TRUE(wp_strpool_append(&doc.strings, (const u8 *)"Yo", 2, &span) == WP_OK);
    ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);

    /* para 1: wholly empty, zero runs */
    ASSERT_TRUE(wp_section_paragraph_add(sec, &p1) == WP_OK);

    /* para 2: "A" + euro sign (a 3-byte codepoint) */
    ASSERT_TRUE(wp_section_paragraph_add(sec, &p2) == WP_OK);
    para = wp_section_paragraph_get(sec, p2);
    {
        u8 bytes[4];
        bytes[0] = 'A';
        bytes[1] = 0xE2u; bytes[2] = 0x82u; bytes[3] = 0xACu;
        ASSERT_TRUE(wp_strpool_append(&doc.strings, bytes, 4, &span) == WP_OK);
        ASSERT_TRUE(wp_paragraph_run_add(para, span, doc.default_cstyle, &run_idx) == WP_OK);
    }

    start = wp_pos_doc_start(&doc);
    end = wp_pos_doc_end(&doc);

    pos = start;
    visited[n++] = pos;
    while (!pos_eq(pos, end) && n < 32) {
        wp_pos next = wp_pos_next(&doc, pos);
        ASSERT_TRUE(!pos_eq(next, pos));
        pos = next;
        visited[n++] = pos;
    }
    ASSERT_EQ_U32(n, 9);

    for (i = 0; i + 1 < n; i++) {
        ASSERT_TRUE(pos_eq(wp_pos_prev(&doc, visited[i + 1]), visited[i]));
        ASSERT_TRUE(pos_eq(wp_pos_next(&doc, visited[i]), visited[i + 1]));
    }

    pos = end;
    for (i = n - 1; i >= 0; i--) {
        ASSERT_TRUE(pos_eq(pos, visited[i]));
        if (i > 0) pos = wp_pos_prev(&doc, pos);
    }

    wp_document_free(&doc);
}
