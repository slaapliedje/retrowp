#include "test.h"
#include "help_doc.h"
#include "help_retrowp.h"

/* Proves the REAL shipped help content (docs/help/retrowp.help,
   compiled in via the generated header -- see host/tools/
   gen_help_header.c) actually parses cleanly and every link target
   resolves to a real node, not just the synthetic fixtures
   test_help_doc.c exercises the parser's own grammar rules against.
   Catches a real authoring mistake (a typo'd link target, a missing
   @endnode) the same way a broken build would, rather than leaving it
   to be discovered only by clicking through the real help viewer. */
void test_help_content_retrowp_help_parses_and_all_links_resolve(void)
{
    wp_help_doc doc;
    u32 i, li;

    ASSERT_TRUE(wp_help_doc_parse(g_help_source_retrowp, (u32)g_help_source_retrowp_len, &doc) == WP_OK);
    ASSERT_TRUE(doc.node_count > 0);

    for (i = 0; i < doc.node_count; i++) {
        const wp_help_node *node = &doc.nodes[i];
        ASSERT_TRUE(node->name.length > 0);
        for (li = 0; li < node->line_count; li++) {
            if (node->lines[li].kind == (u8)WP_HELP_LINE_LINK) {
                wp_span target_span = node->lines[li].target;
                u32 target = wp_help_doc_find_node(&doc,
                    wp_strpool_get(&doc.text, target_span), target_span.length);
                ASSERT_TRUE(target != WP_HELP_NODE_NONE);
            }
        }
    }

    ASSERT_EQ_U32(wp_help_doc_find_node(&doc, (const u8 *)"Main", 4), 0);

    wp_help_doc_free(&doc);
}
