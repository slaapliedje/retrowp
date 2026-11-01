/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "format_md.h"
#include "format_txt.h"

static void import_md(wp_document *doc, const char *text)
{
    ASSERT_TRUE(format_md_import((const u8 *)text, (u32)strlen(text), doc) == WP_OK);
}

static const wp_paragraph *para(const wp_document *doc, u32 pi)
{
    return &doc->sections[0].paras[pi];
}

static void run_text(const wp_document *doc, const wp_paragraph *p, u16 ri, char *out, u32 cap)
{
    wp_span span = p->runs[ri].text;
    u32 n = (span.length < cap - 1) ? span.length : cap - 1;

    memcpy(out, wp_strpool_get(&doc->strings, span), n);
    out[n] = '\0';
}

static u16 run_flags(const wp_document *doc, const wp_paragraph *p, u16 ri)
{
    const wp_char_style *cs = wp_document_cstyle_get(doc, p->runs[ri].style);
    return cs ? cs->flags : 0;
}

static i32 run_size(const wp_document *doc, const wp_paragraph *p, u16 ri)
{
    const wp_char_style *cs = wp_document_cstyle_get(doc, p->runs[ri].style);
    return cs ? cs->size_twips : 0;
}

static void export_md(const wp_document *doc, char *out, u32 cap)
{
    u32 len = 0;

    ASSERT_TRUE(format_md_export(doc, (u8 *)out, cap - 1, &len) == WP_OK);
    out[len] = '\0';
}

void test_format_md_headings_become_big_bold_paragraphs(void)
{
    wp_document doc;
    char text[64];

    import_md(&doc, "# Title\n\nBody text.\n\n## Second ##\n\n#### Deep\n");
    ASSERT_EQ_U32(doc.sections[0].para_count, 4);

    ASSERT_EQ_U32(para(&doc, 0)->run_count, 1);
    run_text(&doc, para(&doc, 0), 0, text, sizeof(text));
    ASSERT_EQ_STR(text, "Title");
    ASSERT_TRUE(run_flags(&doc, para(&doc, 0), 0) & WP_STYLE_BOLD);
    ASSERT_EQ_U32((u32)run_size(&doc, para(&doc, 0), 0), 480);

    run_text(&doc, para(&doc, 1), 0, text, sizeof(text));
    ASSERT_EQ_STR(text, "Body text.");
    ASSERT_EQ_U32(run_flags(&doc, para(&doc, 1), 0), 0);
    ASSERT_EQ_U32((u32)run_size(&doc, para(&doc, 1), 0), 240);

    run_text(&doc, para(&doc, 2), 0, text, sizeof(text));
    ASSERT_EQ_STR(text, "Second");   /* closing hashes are decoration */
    ASSERT_EQ_U32((u32)run_size(&doc, para(&doc, 2), 0), 360);

    run_text(&doc, para(&doc, 3), 0, text, sizeof(text));
    ASSERT_EQ_STR(text, "Deep");
    ASSERT_TRUE(run_flags(&doc, para(&doc, 3), 0) & WP_STYLE_BOLD);
    ASSERT_EQ_U32((u32)run_size(&doc, para(&doc, 3), 0), 240);   /* H4+: bold at body size */

    wp_document_free(&doc);
}

void test_format_md_emphasis_makes_styled_runs(void)
{
    wp_document doc;
    const wp_paragraph *p;
    char text[64];

    import_md(&doc, "a **b** *c* ***d*** _e_ __f__ g\n");
    p = para(&doc, 0);
    ASSERT_EQ_U32(p->run_count, 11);

    run_text(&doc, p, 1, text, sizeof(text)); ASSERT_EQ_STR(text, "b");
    ASSERT_EQ_U32(run_flags(&doc, p, 1), (u32)WP_STYLE_BOLD);
    run_text(&doc, p, 3, text, sizeof(text)); ASSERT_EQ_STR(text, "c");
    ASSERT_EQ_U32(run_flags(&doc, p, 3), (u32)WP_STYLE_ITALIC);
    run_text(&doc, p, 5, text, sizeof(text)); ASSERT_EQ_STR(text, "d");
    ASSERT_EQ_U32(run_flags(&doc, p, 5), (u32)(WP_STYLE_BOLD | WP_STYLE_ITALIC));
    run_text(&doc, p, 7, text, sizeof(text)); ASSERT_EQ_STR(text, "e");
    ASSERT_EQ_U32(run_flags(&doc, p, 7), (u32)WP_STYLE_ITALIC);
    run_text(&doc, p, 9, text, sizeof(text)); ASSERT_EQ_STR(text, "f");
    ASSERT_EQ_U32(run_flags(&doc, p, 9), (u32)WP_STYLE_BOLD);
    run_text(&doc, p, 10, text, sizeof(text)); ASSERT_EQ_STR(text, " g");
    ASSERT_EQ_U32(run_flags(&doc, p, 10), 0);

    wp_document_free(&doc);
}

void test_format_md_joins_wrapped_lines_and_splits_blocks(void)
{
    wp_document doc;
    char text[64];

    import_md(&doc, "one\r\ntwo\r\n\r\nthree\n- a\n- b\n1. c\n> q\n");
    ASSERT_EQ_U32(doc.sections[0].para_count, 6);
    run_text(&doc, para(&doc, 0), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "one two");
    run_text(&doc, para(&doc, 1), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "three");
    run_text(&doc, para(&doc, 2), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "- a");
    run_text(&doc, para(&doc, 3), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "- b");
    run_text(&doc, para(&doc, 4), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "1. c");
    run_text(&doc, para(&doc, 5), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "> q");

    wp_document_free(&doc);
}

void test_format_md_leaves_text_that_only_looks_like_markup(void)
{
    wp_document doc;
    char text[64];

    import_md(&doc, "5 * 3 = 15 and snake_case_name\n\n\\*not em\\* `**code**`\n\n```\n**raw**\n```\n\n5*3 and **unclosed\n");
    ASSERT_EQ_U32(doc.sections[0].para_count, 6);
    ASSERT_EQ_U32(para(&doc, 0)->run_count, 1);
    run_text(&doc, para(&doc, 0), 0, text, sizeof(text));
    ASSERT_EQ_STR(text, "5 * 3 = 15 and snake_case_name");
    ASSERT_EQ_U32(para(&doc, 1)->run_count, 1);
    run_text(&doc, para(&doc, 1), 0, text, sizeof(text));
    ASSERT_EQ_STR(text, "*not em* `**code**`");
    run_text(&doc, para(&doc, 2), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "```");
    ASSERT_EQ_U32(para(&doc, 3)->run_count, 1);
    run_text(&doc, para(&doc, 3), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "**raw**");
    ASSERT_EQ_U32(run_flags(&doc, para(&doc, 3), 0), 0);
    /* Openers nothing closes were never markup. */
    ASSERT_EQ_U32(para(&doc, 5)->run_count, 1);
    run_text(&doc, para(&doc, 5), 0, text, sizeof(text)); ASSERT_EQ_STR(text, "5*3 and **unclosed");
    ASSERT_EQ_U32(run_flags(&doc, para(&doc, 5), 0), 0);

    wp_document_free(&doc);
}

void test_format_md_nested_and_adjacent_emphasis(void)
{
    wp_document doc;
    const wp_paragraph *p;
    char text[64];

    /* The forms people write, and the adjacent markers the exporter writes. */
    import_md(&doc, "*em **both** em* and **bo***it* x\n");
    p = para(&doc, 0);
    ASSERT_EQ_U32(p->run_count, 7);
    run_text(&doc, p, 0, text, sizeof(text)); ASSERT_EQ_STR(text, "em ");
    ASSERT_EQ_U32(run_flags(&doc, p, 0), (u32)WP_STYLE_ITALIC);
    run_text(&doc, p, 1, text, sizeof(text)); ASSERT_EQ_STR(text, "both");
    ASSERT_EQ_U32(run_flags(&doc, p, 1), (u32)(WP_STYLE_BOLD | WP_STYLE_ITALIC));
    run_text(&doc, p, 2, text, sizeof(text)); ASSERT_EQ_STR(text, " em");
    ASSERT_EQ_U32(run_flags(&doc, p, 2), (u32)WP_STYLE_ITALIC);
    run_text(&doc, p, 3, text, sizeof(text)); ASSERT_EQ_STR(text, " and ");
    ASSERT_EQ_U32(run_flags(&doc, p, 3), 0);
    run_text(&doc, p, 4, text, sizeof(text)); ASSERT_EQ_STR(text, "bo");
    ASSERT_EQ_U32(run_flags(&doc, p, 4), (u32)WP_STYLE_BOLD);
    run_text(&doc, p, 5, text, sizeof(text)); ASSERT_EQ_STR(text, "it");
    ASSERT_EQ_U32(run_flags(&doc, p, 5), (u32)WP_STYLE_ITALIC);
    run_text(&doc, p, 6, text, sizeof(text)); ASSERT_EQ_STR(text, " x");

    wp_document_free(&doc);
}

void test_format_md_export_round_trips_what_the_model_holds(void)
{
    /* Everything the model can hold, in the spelling the exporter uses:
       list markers stay text (a reader still sees a list), a lone star
       and an underscore inside a word need no escape, an empty paragraph
       is the &nbsp; idiom, a fence encloses lines kept one per
       paragraph, and markers next to each other read back nested. */
    static const char canonical[] =
        "# Title\n\n"
        "Plain **bold** *em* ***both*** end.\n\n"
        "- item one\n\n"
        "1. a list\n\n"
        "&nbsp;\n\n"
        "\\# not a heading\n\n"
        "a\\*b_c and 5 * 3 and snake_case\n\n"
        "**bo***ld* un**bo**ld\n\n"
        "```\nraw *code*\n\n  more\n```\n\n"
        "after\n";
    wp_document doc;
    char out[400];
    u32 needed = 0;

    import_md(&doc, canonical);
    export_md(&doc, out, sizeof(out));
    ASSERT_EQ_STR(out, canonical);

    /* Two-call sizing, like every exporter. */
    ASSERT_TRUE(format_md_export(&doc, NULL, 0, &needed) == WP_RANGE);
    ASSERT_EQ_U32(needed, (u32)strlen(canonical));
    ASSERT_TRUE(format_md_export(&doc, (u8 *)out, needed - 1, &needed) == WP_RANGE);

    wp_document_free(&doc);
}

void test_format_md_export_of_a_plain_document_is_its_paragraphs(void)
{
    wp_document doc;
    char out[128];

    ASSERT_TRUE(format_txt_import((const u8 *)"One\n\nTwo\n", 9, &doc) == WP_OK);
    export_md(&doc, out, sizeof(out));
    ASSERT_EQ_STR(out, "One\n\n&nbsp;\n\nTwo\n");   /* the empty paragraph survives as the idiom */
    wp_document_free(&doc);

    /* ...and comes back as an empty paragraph, not as text. */
    ASSERT_TRUE(format_md_import((const u8 *)"One\n\n&nbsp;\n\nTwo\n", 17, &doc) == WP_OK);
    ASSERT_EQ_U32(doc.sections[0].para_count, 3);
    ASSERT_EQ_U32(doc.sections[0].paras[1].run_count, 0);
    wp_document_free(&doc);

    ASSERT_TRUE(format_txt_import((const u8 *)"", 0, &doc) == WP_OK);
    export_md(&doc, out, sizeof(out));
    ASSERT_EQ_STR(out, "");
    wp_document_free(&doc);
}
