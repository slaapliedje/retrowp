#include <string.h>
#include "test.h"
#include "help_doc.h"
#include "mem_host.h"

static wp_bool span_eq(const wp_help_doc *doc, wp_span span, const char *expected)
{
    u32 expected_len = (u32)strlen(expected);
    if (span.length != expected_len) return WP_FALSE;
    if (expected_len == 0) return WP_TRUE;
    return (wp_bool)(memcmp(wp_strpool_get(&doc->text, span), expected, expected_len) == 0);
}

void test_help_doc_parses_single_node(void)
{
    static const char src[] =
        "@node Main \"Getting Started\"\n"
        "Welcome to RetroWP.\n"
        "@endnode\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.node_count, 1);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].name, "Main"));
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].title, "Getting Started"));
    ASSERT_EQ_U32(doc.nodes[0].line_count, 1);
    ASSERT_TRUE(doc.nodes[0].lines[0].kind == (u8)WP_HELP_LINE_TEXT);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].lines[0].text, "Welcome to RetroWP."));

    wp_help_doc_free(&doc);
}

void test_help_doc_node_without_title_falls_back_to_name(void)
{
    static const char src[] =
        "@node Main\n"
        "Body.\n"
        "@endnode\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.node_count, 1);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].name, "Main"));
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].title, "Main"));

    wp_help_doc_free(&doc);
}

void test_help_doc_parses_link_line(void)
{
    static const char src[] =
        "@node Main \"Main\"\n"
        "See also:\n"
        "{Editing Basics|Editing}\n"
        "@endnode\n"
        "@node Editing \"Editing Basics\"\n"
        "Some text.\n"
        "@endnode\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.node_count, 2);
    ASSERT_EQ_U32(doc.nodes[0].line_count, 2);
    ASSERT_TRUE(doc.nodes[0].lines[0].kind == (u8)WP_HELP_LINE_TEXT);
    ASSERT_TRUE(doc.nodes[0].lines[1].kind == (u8)WP_HELP_LINE_LINK);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].lines[1].text, "Editing Basics"));
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].lines[1].target, "Editing"));

    wp_help_doc_free(&doc);
}

void test_help_doc_braces_without_bar_are_plain_text(void)
{
    static const char src[] =
        "@node Main \"Main\"\n"
        "{not a link}\n"
        "@endnode\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.nodes[0].line_count, 1);
    ASSERT_TRUE(doc.nodes[0].lines[0].kind == (u8)WP_HELP_LINE_TEXT);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].lines[0].text, "{not a link}"));

    wp_help_doc_free(&doc);
}

void test_help_doc_find_node_by_name(void)
{
    static const char src[] =
        "@node One \"First\"\nA.\n@endnode\n"
        "@node Two \"Second\"\nB.\n@endnode\n";
    wp_help_doc doc;
    u32 idx;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);

    idx = wp_help_doc_find_node(&doc, (const u8 *)"Two", 3);
    ASSERT_EQ_U32(idx, 1);
    idx = wp_help_doc_find_node(&doc, (const u8 *)"One", 3);
    ASSERT_EQ_U32(idx, 0);
    idx = wp_help_doc_find_node(&doc, (const u8 *)"NoSuchNode", 10);
    ASSERT_EQ_U32(idx, WP_HELP_NODE_NONE);

    wp_help_doc_free(&doc);
}

void test_help_doc_missing_endnode_at_eof_implicitly_closes(void)
{
    static const char src[] = "@node Main \"Main\"\nUnterminated.\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.node_count, 1);
    ASSERT_EQ_U32(doc.nodes[0].line_count, 1);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].lines[0].text, "Unterminated."));

    wp_help_doc_free(&doc);
}

void test_help_doc_skips_content_before_first_node(void)
{
    static const char src[] =
        "@database RetroWP Help\n"
        "\n"
        "@node Main \"Main\"\n"
        "Body.\n"
        "@endnode\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.node_count, 1);
    ASSERT_TRUE(span_eq(&doc, doc.nodes[0].name, "Main"));

    wp_help_doc_free(&doc);
}

void test_help_doc_blank_line_preserved_as_empty_text_line(void)
{
    static const char src[] =
        "@node Main \"Main\"\n"
        "First paragraph.\n"
        "\n"
        "Second paragraph.\n"
        "@endnode\n";
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    ASSERT_EQ_U32(doc.nodes[0].line_count, 3);
    ASSERT_TRUE(doc.nodes[0].lines[1].kind == (u8)WP_HELP_LINE_TEXT);
    ASSERT_EQ_U32(doc.nodes[0].lines[1].text.length, 0);

    wp_help_doc_free(&doc);
}

void test_help_doc_empty_source_yields_zero_nodes(void)
{
    wp_help_doc doc;

    ASSERT_TRUE(wp_help_doc_parse((const u8 *)"", 0, &doc) == WP_OK);
    ASSERT_EQ_U32(doc.node_count, 0);

    wp_help_doc_free(&doc);
}

void test_help_doc_no_leaks(void)
{
    static const char src[] =
        "@node One \"First\"\nA.\n{link one|Two}\n@endnode\n"
        "@node Two \"Second\"\nB.\n@endnode\n"
        "@node Three \"Third\"\n"
        "Trailing node with no endnode.\n";
    wp_help_doc doc;

    mem_host_reset_counters();
    ASSERT_TRUE(wp_help_doc_parse((const u8 *)src, (u32)strlen(src), &doc) == WP_OK);
    wp_help_doc_free(&doc);
    ASSERT_EQ_U32(mem_host_live_count(), 0);
}
