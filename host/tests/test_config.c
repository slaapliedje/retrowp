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

void test_config_parses_keymap_wordstar(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=WordStar\n"), (u32)WP_KEYMAP_WORDSTAR);
}

void test_config_parses_keymap_emacs(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=Emacs\n"), (u32)WP_KEYMAP_EMACS);
}

void test_config_parses_keymap_wordperfect(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=WordPerfect\n"), (u32)WP_KEYMAP_WORDPERFECT);
}

void test_config_parses_keymap_msword(void)
{
    ASSERT_EQ_U32((u32)parse("keymap=MSWord\n"), (u32)WP_KEYMAP_MSWORD);
    ASSERT_EQ_U32((u32)parse("keymap=Word\n"), (u32)WP_KEYMAP_MSWORD);
}

void test_config_unrecognized_value_falls_back_to_default(void)
{
    /* No scheme this project implements or plans to is named this --
       must not produce an invalid/unclamped id, see wp_config_parse's
       own leniency contract. */
    ASSERT_EQ_U32((u32)parse("keymap=nonexistentscheme\n"), (u32)WP_KEYMAP_VI);
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

/* The SHIPPED sample (docs/retrowp.cfg) must actually work. It is the
   file copied onto a real machine's boot drive, and the one a person
   hand-edits, so "it parses" is not something to assume.
 *
   This is a regression test with a specific history: the sample is ~3.5KB
   of documentation and its single live `keymap=` line sits at byte 1435.
   wp_config_load used to read into a 512-byte STACK buffer and silently
   truncate, so this exact file would have loaded, reported success, and
   applied nothing -- a config that looks correct and does nothing, with
   no error anywhere. If someone shrinks the read window again, or lets
   the comments grow past it, this fails instead of the user finding out
   on a Falcon. */
void test_config_shipped_sample_parses(void)
{
    wp_config cfg;

    /* Run from the repo root, as `make test` does. */
    ASSERT_TRUE(wp_config_load("docs/retrowp.cfg", &cfg) == WP_OK);
    ASSERT_EQ_U32((u32)cfg.keymap_id, (u32)WP_KEYMAP_VI);
}

/* Every commented-out key in the sample must stay inert. The "not yet
   implemented" block is only safe to ship because unknown keys are
   skipped -- if a future parser started honouring one of those names
   while the sample still had it commented with a placeholder value, the
   file would quietly change behaviour. Proven here on the real bytes:
   uncommenting nothing must leave every default standing. */
void test_config_sample_inert_keys_do_not_alter_defaults(void)
{
    static const char inert[] =
        "#page_width=12240\r\n"
        "#font_size=240\r\n"
        "#align=center\r\n"
        "offload=serial\r\n"      /* not a comment, but not a live key */
        "font_size=999\r\n"
        "keymap=emacs\r\n";
    wp_config cfg;

    ASSERT_TRUE(wp_config_parse((const u8 *)inert,
                                (u32)(sizeof(inert) - 1), &cfg) == WP_OK);
    /* The one real key still takes effect, past all the noise. */
    ASSERT_EQ_U32((u32)cfg.keymap_id, (u32)WP_KEYMAP_EMACS);
}
