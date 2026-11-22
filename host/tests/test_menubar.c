/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The text-mode menu bar (menubar.h, M21 T21.5): what is in it, its
   letters, the keys and clicks, and where its boxes sit. */
#include <string.h>
#include "test.h"
#include "menubar.h"

static const u8 k_groups[] = {
    WP_MENU_FILE, WP_MENU_EDIT, WP_MENU_VIEW, WP_MENU_FORMAT, WP_MENU_TOOLS, WP_MENU_HELP
};
static const char *const k_titles[] = { "File", "Edit", "View", "Format", "Tools", "Help" };

static const char *dos_label(wp_action_id id)
{
    return id == WP_ACT_FILE_QUIT ? "Exit" : NULL;
}

static const char *dos_hint(wp_action_id id)
{
    const wp_action *a = wp_action_find(id);
    if (id == WP_ACT_FILE_QUIT) return "Ctrl+X Ctrl+C";   /* the widest a scheme gives */
    return a != NULL ? a->key_hint_utf8 : NULL;
}

static void build(wp_menubar *mb)
{
    wp_menubar_build(mb, k_groups, k_titles, 6u, dos_label);
}

static wp_key_event special(wp_special_key k)
{
    wp_key_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.special = (u16)k;
    return ev;
}

static wp_key_event letter(u32 c, u8 mods)
{
    wp_key_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.codepoint = c;
    ev.mods = mods;
    return ev;
}

static char title_letter(const wp_menubar *mb, u32 t)
{
    return mb->titles[t].label[mb->titles[t].letter];
}

void test_menubar_has_every_menu_action_once(void)
{
    wp_menubar mb;
    const wp_menubar_title *file;
    const wp_menubar_item *last;

    build(&mb);
    ASSERT_EQ_U32(mb.title_count, 6u);
    ASSERT_TRUE(wp_menubar_selfcheck(&mb, dos_hint, 80u, 25u));
    /* Letters: the first free one, so Format takes its o. */
    ASSERT_TRUE(title_letter(&mb, 0) == 'F');
    ASSERT_TRUE(title_letter(&mb, 1) == 'E');
    ASSERT_TRUE(title_letter(&mb, 2) == 'V');
    ASSERT_TRUE(title_letter(&mb, 3) == 'o');
    ASSERT_TRUE(title_letter(&mb, 4) == 'T');
    ASSERT_TRUE(title_letter(&mb, 5) == 'H');
    /* Exit last in File, under a rule, by the shell's own name. */
    file = &mb.titles[0];
    last = &mb.items[file->first + file->count - 1u];
    ASSERT_EQ_U32(last->action, (u32)WP_ACT_FILE_QUIT);
    ASSERT_TRUE(last->rule_before == 1);
    ASSERT_EQ_STR(last->label, "Exit");
    ASSERT_TRUE(last->label[last->letter] == 'x');   /* Settings took the e: E_x_it, as DOS Edit has it */
}

void test_menubar_keys_walk_and_choose(void)
{
    wp_menubar mb;
    wp_menubar_outcome o;
    const wp_menubar_title *edit;

    build(&mb);
    ASSERT_EQ_U32(wp_menubar_feed(&mb, special(WP_KEY_DOWN)).result, (u32)WP_MENUBAR_NOTHING);
    wp_menubar_toggle(&mb);                       /* Alt let go, or F10 */
    ASSERT_EQ_U32(mb.mode, (u32)WP_MENUBAR_BAR);
    ASSERT_EQ_U32(wp_menubar_feed(&mb, special(WP_KEY_LEFT)).result, (u32)WP_MENUBAR_REDRAW);
    ASSERT_EQ_U32(mb.title, 5u);                  /* wraps to Help */
    (void)wp_menubar_feed(&mb, special(WP_KEY_RIGHT));
    (void)wp_menubar_feed(&mb, special(WP_KEY_RIGHT));
    ASSERT_EQ_U32(mb.title, 1u);                  /* Edit */
    (void)wp_menubar_feed(&mb, special(WP_KEY_DOWN));
    ASSERT_EQ_U32(mb.mode, (u32)WP_MENUBAR_OPEN);
    ASSERT_EQ_U32(mb.item, 0u);
    (void)wp_menubar_feed(&mb, special(WP_KEY_UP));
    edit = &mb.titles[1];
    ASSERT_EQ_U32(mb.item, (u32)edit->count - 1u);   /* wraps to the last */
    (void)wp_menubar_feed(&mb, special(WP_KEY_HOME));
    o = wp_menubar_feed(&mb, special(WP_KEY_ENTER));
    ASSERT_EQ_U32(o.result, (u32)WP_MENUBAR_CHOSEN);
    ASSERT_EQ_U32(o.action, mb.items[edit->first].action);
    ASSERT_EQ_U32(mb.mode, (u32)WP_MENUBAR_CLOSED);
}

void test_menubar_letters_open_and_choose(void)
{
    wp_menubar mb;
    wp_menubar_outcome o;

    build(&mb);
    ASSERT_TRUE(wp_menubar_open_letter(&mb, 'o'));   /* Alt+O: Format */
    ASSERT_EQ_U32(mb.title, 3u);
    /* Alt+another letter inside a menu moves to that menu. */
    o = wp_menubar_feed(&mb, letter('f', (u8)WP_MOD_ALT));
    ASSERT_EQ_U32(o.result, (u32)WP_MENUBAR_REDRAW);
    ASSERT_EQ_U32(mb.title, 0u);
    /* An item's letter, either case, chooses it. */
    o = wp_menubar_feed(&mb, letter('X', 0));
    ASSERT_EQ_U32(o.result, (u32)WP_MENUBAR_CHOSEN);
    ASSERT_EQ_U32(o.action, (u32)WP_ACT_FILE_QUIT);
    /* A letter nothing uses, and Ctrl+letter, do nothing. */
    wp_menubar_open(&mb, 0);
    ASSERT_EQ_U32(wp_menubar_feed(&mb, letter('z', 0)).result, (u32)WP_MENUBAR_NOTHING);
    ASSERT_EQ_U32(wp_menubar_feed(&mb, letter('o', (u8)WP_MOD_CTRL)).result, (u32)WP_MENUBAR_NOTHING);
    ASSERT_EQ_U32(mb.mode, (u32)WP_MENUBAR_OPEN);
    ASSERT_EQ_U32(wp_menubar_feed(&mb, special(WP_KEY_ESCAPE)).result, (u32)WP_MENUBAR_LEFT);
    ASSERT_EQ_U32(mb.mode, (u32)WP_MENUBAR_CLOSED);
    ASSERT_TRUE(!wp_menubar_open_letter(&mb, 'q'));
}

void test_menubar_clicks_land_on_items(void)
{
    wp_menubar mb;
    wp_menubar_box box;
    const wp_menubar_title *file;
    u32 i, rule_row = 0;

    build(&mb);
    file = &mb.titles[0];
    ASSERT_EQ_U32(wp_menubar_title_at(&mb, mb.titles[1].col) , 1u);
    ASSERT_EQ_U32(wp_menubar_title_at(&mb, mb.titles[1].col + 3u), 1u);
    ASSERT_TRUE(wp_menubar_title_at(&mb, 79u) == -1);
    wp_menubar_box_of(&mb, 0, dos_hint, 80u, &box);
    ASSERT_EQ_U32(box.row, 1u);
    ASSERT_EQ_U32(box.height, (u32)file->count + 3u);   /* borders and Exit's rule */
    for (i = 0; i < file->count; i++) {
        u32 row = wp_menubar_item_row(&mb, 0, i, &box);
        ASSERT_EQ_U32(wp_menubar_item_at(&mb, 0, &box, (u32)box.col + 3u, row), i);
        if (mb.items[file->first + i].rule_before) rule_row = row - 1u;
    }
    ASSERT_TRUE(rule_row > box.row);
    ASSERT_TRUE(wp_menubar_item_at(&mb, 0, &box, (u32)box.col + 3u, rule_row) == -1);
    ASSERT_TRUE(wp_menubar_item_at(&mb, 0, &box, box.col, box.row + 1u) == -1);   /* the border */
    ASSERT_TRUE(wp_menubar_item_at(&mb, 0, &box, (u32)box.col + 3u, box.row) == -1);
}

void test_menubar_boxes_stay_on_screen(void)
{
    wp_menubar mb;
    wp_menubar_box box;

    build(&mb);
    /* Help is the rightmost title; on a narrow screen its box is pulled
       left rather than run off the edge. */
    wp_menubar_box_of(&mb, 5u, dos_hint, 46u, &box);
    ASSERT_TRUE((u32)box.col + box.width <= 46u);
    ASSERT_TRUE(!wp_menubar_selfcheck(&mb, dos_hint, 80u, 10u));   /* Format is taller */
}

/* The menus' order (wp_action_menu_order): every action once, the
   table's order kept, and each Pi command right under the one it
   extends -- here as the File menu the DOS bar builds from it. */
void test_menubar_pi_commands_sit_by_their_partners(void)
{
    static const wp_action_id want[] = {
        WP_ACT_FILE_NEW, WP_ACT_FILE_OPEN, WP_ACT_FILE_OPEN_FROM_PI,
        WP_ACT_FILE_SAVE, WP_ACT_FILE_SAVE_AS, WP_ACT_FILE_SAVE_TO_PI
    };
    u8 seen[WP_ACT_COUNT];
    wp_menubar mb;
    u32 i;

    memset(seen, 0, sizeof(seen));
    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        const wp_action *a = wp_action_menu_order(i);
        ASSERT_TRUE(a != NULL);
        ASSERT_TRUE(seen[a->id] == 0);
        seen[a->id] = 1;
    }
    ASSERT_TRUE(wp_action_menu_order((u32)WP_ACT_COUNT) == NULL);

    build(&mb);
    ASSERT_TRUE(mb.titles[0].group == WP_MENU_FILE);
    for (i = 0; i < (u32)(sizeof(want) / sizeof(want[0])); i++) {
        ASSERT_TRUE(mb.items[mb.titles[0].first + i].action == (u8)want[i]);
    }
    /* Quit still last. */
    ASSERT_TRUE(mb.items[mb.titles[0].first + mb.titles[0].count - 1].action == (u8)WP_ACT_FILE_QUIT);
}
