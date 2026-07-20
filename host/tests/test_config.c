#include <string.h>
#include "test.h"
#include "config.h"

static wp_keymap_id parse(const char *src)
{
    wp_config cfg;
    wp_config_parse((const u8 *)src, (u32)strlen(src), &cfg);
    return (wp_keymap_id)cfg.keymap_id;
}

void test_config_defaults_to_vi_on_empty_input(void)
{
    ASSERT_EQ_U32((u32)parse(""), (u32)WP_KEYMAP_VI);
}

void test_config_parses_keymap_vim(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=vim\n"), (u32)WP_KEYMAP_VI);
}

void test_config_key_and_value_are_case_insensitive(void)
{
    ASSERT_EQ_U32((u32)parse("KeyMap=ViM\n"), (u32)WP_KEYMAP_VI);
}

void test_config_tolerates_surrounding_whitespace(void)
{
    ASSERT_EQ_U32((u32)parse("  keymap = vim  \n"), (u32)WP_KEYMAP_VI);
}

void test_config_skips_comment_and_blank_lines(void)
{
    static const char src[] =
        "# a comment\n"
        "\n"
        "; another comment style\n"
        "keymap=vim\n";
    ASSERT_EQ_U32((u32)parse(src), (u32)WP_KEYMAP_VI);
}

void test_config_unrecognized_value_falls_back_to_default(void)
{
    /* Not yet an implemented scheme this phase -- must not produce an
       invalid/unclamped id, see wp_config_parse's own leniency contract. */
    ASSERT_EQ_U32((u32)parse("keymap=wordstar\n"), (u32)WP_KEYMAP_VI);
}

void test_config_unrecognized_key_is_ignored(void)
{
    ASSERT_EQ_U32((u32)parse("nonsense=vim\n"), (u32)WP_KEYMAP_VI);
}

void test_config_malformed_line_without_equals_is_skipped(void)
{
    static const char src[] =
        "this is not a key value line\n"
        "keymap=vim\n";
    ASSERT_EQ_U32((u32)parse(src), (u32)WP_KEYMAP_VI);
}

void test_config_tolerates_crlf_line_endings(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=vim\r\n"), (u32)WP_KEYMAP_VI);
}

void test_config_last_line_without_trailing_newline_is_parsed(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=vim"), (u32)WP_KEYMAP_VI);
}

void test_config_load_missing_file_yields_default(void)
{
    wp_config cfg;
    ASSERT_TRUE(wp_config_load("/no/such/path/retrowp.cfg.does.not.exist", &cfg) == WP_OK);
    ASSERT_EQ_U32((u32)cfg.keymap_id, (u32)WP_KEYMAP_VI);
}
