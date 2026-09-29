/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "menucheck.h"
#include "help_doc.h"
#include "help_retrowp.h"

/* Host-side coverage for platform/common/menucheck.c -- the shared menu
   rules the four backends fold g_wp_actions[] through.

   The same wp_menu_model_selfcheck() also runs on all four targets
   inside their own wp_*_menu_selfcheck(). That is not redundant: this
   file proves the LOGIC, the on-target copies prove the same table
   still holds under each cross-compiler's own word sizes and packing.
   What can only be tested here is the negative space -- feeding the
   helpers inputs the real table does not contain. */

void test_menucheck_accel_letter_maps_only_ctrl_letters(void)
{
    /* The whole Ctrl-letter range maps to 'A'..'Z'. */
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x01), (u32)'A');
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x02), (u32)'B');
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x0F), (u32)'O');
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x1A), (u32)'Z');

    /* 0 means "no shortcut" and must not become '@'. */
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x00), 0);

    /* The two real non-letter bytes in the live table. Amiga's old
       shortcut_ascii + 0x40 turned 0x1D into ']' and 0x0C into 'L' --
       the first a nonsense glyph, the second a plausible-looking but
       wrong accelerator, which is worse. */
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x1D), 0);
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x1B), 0);
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x1F), 0);

    /* Printable ASCII is a legal value in this byte space per actions.h
       and is not a Ctrl-letter, so it has no accelerator letter. */
    ASSERT_EQ_U32((u32)wp_menu_accel_letter((u8)'q'), 0);
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x7F), 0);
}

/* 0x0C IS a Ctrl-letter (Ctrl+L) and so does map -- pinned separately so
   the line above is not misread as "every non-letter byte returns 0". */
void test_menucheck_accel_letter_0x0c_is_ctrl_l(void)
{
    ASSERT_EQ_U32((u32)wp_menu_accel_letter(0x0C), (u32)'L');
}

void test_menucheck_label_rejects_menu_metacharacters(void)
{
    /* Each of these is a real Mac Menu Manager metacharacter. */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Save;Quit")); /* splits into TWO items */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Save/S"));    /* command key */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Save("));     /* disables the item */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Save^1"));    /* icon */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Save!x"));    /* mark */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Save<B"));    /* style */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("-Divider"));  /* leading '-' */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe("Two\rLines"));

    /* Empty and NULL are unusable as menu text. */
    ASSERT_TRUE(!wp_menu_label_is_literal_safe(""));
    ASSERT_TRUE(!wp_menu_label_is_literal_safe(NULL));

    /* Everything the real table actually uses stays safe, including an
       interior '-' and the "..." dialog convention. */
    ASSERT_TRUE(wp_menu_label_is_literal_safe("Open"));
    ASSERT_TRUE(wp_menu_label_is_literal_safe("Size..."));
    ASSERT_TRUE(wp_menu_label_is_literal_safe("Grammar Check"));
    ASSERT_TRUE(wp_menu_label_is_literal_safe("Well-Formed"));
}

/* The live table must satisfy every invariant the four menu builders
   depend on. This is the same call each backend makes on-target. */
void test_menucheck_live_action_table_is_menu_safe(void)
{
    ASSERT_TRUE(wp_menu_model_selfcheck());
}

/* Every label in the shipped table is individually literal-safe. Stated
   separately from the aggregate check above so a failure names the
   labels as the cause rather than "something in the model". */
void test_menucheck_every_live_label_is_literal_safe(void)
{
    u32 i;

    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        ASSERT_TRUE(wp_menu_label_is_literal_safe(g_wp_actions[i].label_utf8));
    }
}

/* No two actions may share a shortcut byte: wp_action_find_by_shortcut
   returns the FIRST match, so a duplicate silently makes one action
   unreachable by keyboard -- and on DOS, which has no menu, unreachable
   entirely. */
void test_menucheck_shortcut_bytes_are_unique(void)
{
    u32 i, j;

    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        if (g_wp_actions[i].shortcut_ascii == 0) continue;
        ASSERT_TRUE(wp_action_find_by_shortcut(g_wp_actions[i].shortcut_ascii)
                    == &g_wp_actions[i]);
        for (j = i + 1; j < (u32)WP_ACT_COUNT; j++) {
            ASSERT_TRUE(g_wp_actions[i].shortcut_ascii != g_wp_actions[j].shortcut_ascii);
        }
    }
}

/* Each menu group a backend builds a title for must contain at least one
   action. Amiga's and Mac's folds both `continue` past an empty group,
   which drops the entire title with no diagnostic anywhere -- the user
   just sees a menu bar missing "Format". */
void test_menucheck_no_menu_group_is_empty(void)
{
    static const wp_menu_group groups[] = {
        WP_MENU_FILE, WP_MENU_EDIT, WP_MENU_FORMAT, WP_MENU_TOOLS, WP_MENU_HELP
    };
    u32 g, i;

    for (g = 0; g < (u32)(sizeof(groups) / sizeof(groups[0])); g++) {
        wp_bool any = WP_FALSE;

        for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
            if (g_wp_actions[i].group == groups[g]) any = WP_TRUE;
        }
        ASSERT_TRUE(any);
    }
}

/* Every WP_ACT_HELP_* action must name a node that really exists in the
   shipped help document, and no other action may claim one.

   This is the check that makes wp_action_help_node safe to share across
   four backends. wp_help_doc_find_node falls back to node 0 on a miss,
   so a typo or a renamed @node does not crash and does not log -- every
   Help menu item just quietly opens the index page instead of the topic
   it names. That is invisible in a screenshot unless you already know
   which node you asked for, and invisible to every on-target selfcheck,
   because nothing about it is structurally wrong. Pinning the names
   against the real parsed document is the only place it can be caught. */
void test_menucheck_help_actions_name_real_help_nodes(void)
{
    wp_help_doc doc;
    u32 i;
    u32 help_actions = 0;

    ASSERT_TRUE(wp_help_doc_parse(g_help_source_retrowp,
                                   (u32)g_help_source_retrowp_len, &doc) == WP_OK);

    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        u32 len = 0;
        const char *node = wp_action_help_node((wp_action_id)i, &len);

        if (g_wp_actions[i].group == WP_MENU_HELP) {
            /* Every Help-group action resolves... */
            ASSERT_TRUE(node != NULL);
            ASSERT_TRUE(len == (u32)strlen(node));
            ASSERT_TRUE(wp_help_doc_find_node(&doc, (const u8 *)node, len)
                        != WP_HELP_NODE_NONE);
            help_actions++;
        } else {
            /* ...and nothing else does, so a non-Help action can never
               be silently swallowed by the Help arm that every backend's
               invoke_backend_action now runs BEFORE its own switch. */
            ASSERT_TRUE(node == NULL);
        }
    }

    /* Guards against the whole loop passing vacuously if the Help group
       were ever emptied out. */
    ASSERT_TRUE(help_actions >= 3);

    wp_help_doc_free(&doc);
}

/* A NULL len out-param is legal -- the backends that only need the
   pointer shouldn't have to invent a variable to throw away. */
void test_menucheck_help_node_tolerates_null_len(void)
{
    ASSERT_TRUE(wp_action_help_node(WP_ACT_HELP_CONTENTS, NULL) != NULL);
    ASSERT_TRUE(wp_action_help_node(WP_ACT_FILE_OPEN, NULL) == NULL);
    /* Out of range must be NULL, not a wild read: every backend feeds
       this the same u8 that came out of a keymap outcome. */
    ASSERT_TRUE(wp_action_help_node((wp_action_id)WP_ACT_COUNT, NULL) == NULL);
}
