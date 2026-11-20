/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_MENUBAR_H
#define WP_MENUBAR_H

/* A text-mode menu bar (M21 T21.5), in the DOS convention -- MS-DOS
 * Edit, Works for DOS, QBasic: a bar of titles on the top row, each with
 * a highlighted letter; Alt (pressed and let go) or F10 lights the bar,
 * Alt+letter opens a menu, the arrows walk it, Enter or an item's letter
 * chooses, Esc leaves. A dropdown is a box under its title.
 *
 * This file is the model: which items, in which menus, which letters,
 * where everything sits on a grid of character cells, and what a key or
 * a click does. Drawing and reading the keyboard are the shell's, so the
 * DOS shell draws it into video memory and the host tests it here.
 *
 * The menus are folded from g_wp_actions[] by group, the way the Amiga's
 * and the Mac's are, so anything another port has in a menu is in one
 * here too. Two DOS habits on top: the File menu's Quit goes last under
 * a rule (a shell may call it "Exit", as DOS programs do), and each
 * menu's letters are picked so no two in one menu are the same.
 *
 * Coordinates are cells, the bar on row 0. */

#include "types.h"
#include "keyevent.h"
#include "actions.h"

#define WP_MENUBAR_MAX_TITLES 8
#define WP_MENUBAR_NO_LETTER  0xFFu

/* A label or hint for an action, as this shell shows it; NULL or "" for
   none. For labels, NULL means the registry's own. */
typedef const char *(*wp_menubar_text_fn)(wp_action_id id);

typedef struct {
    u8 group;              /* wp_menu_group */
    const char *label;     /* "File" */
    u8 letter;             /* index into label of its letter, or NO_LETTER */
    u8 col;                /* the label's first cell on the bar */
    u8 first, count;       /* its items, in items[] */
} wp_menubar_title;

typedef struct {
    u8 action;             /* wp_action_id */
    const char *label;
    u8 letter;             /* index into label, or NO_LETTER */
    u8 rule_before;        /* a separator line above it */
} wp_menubar_item;

typedef enum {
    WP_MENUBAR_CLOSED = 0, /* the bar is drawn but nothing is lit */
    WP_MENUBAR_BAR,        /* a title is lit, no menu down (Alt, F10) */
    WP_MENUBAR_OPEN        /* a menu is down, an item lit */
} wp_menubar_mode;

typedef struct {
    wp_menubar_title titles[WP_MENUBAR_MAX_TITLES];
    u8 title_count;
    wp_menubar_item items[WP_ACT_COUNT];
    u8 item_count;
    u8 mode;               /* wp_menubar_mode */
    u8 title;              /* the lit title */
    u8 item;               /* the lit item, within that title's */
} wp_menubar;

/* A dropdown's box, borders included. */
typedef struct {
    u8 col, row, width, height;
    u8 hint_col;           /* where the hints start, inside the box */
} wp_menubar_box;

typedef enum {
    WP_MENUBAR_NOTHING = 0, /* the key meant nothing here */
    WP_MENUBAR_REDRAW,      /* something moved */
    WP_MENUBAR_CHOSEN,      /* .action was chosen; the bar is closed */
    WP_MENUBAR_LEFT         /* the bar was left without a choice */
} wp_menubar_result;

typedef struct {
    u8 result;              /* wp_menubar_result */
    u8 action;              /* with CHOSEN */
} wp_menubar_outcome;

/* The bar: one title per group in `groups` (n of them, at most
   WP_MENUBAR_MAX_TITLES), named by `titles`; a group with no actions is
   left out. label_of may be NULL. Closed afterwards. */
void wp_menubar_build(wp_menubar *mb, const u8 *groups, const char *const *titles, u32 n,
                      wp_menubar_text_fn label_of);

/* Light the bar (Alt let go, F10) at the first title, or leave it if it
   was lit. */
void wp_menubar_toggle(wp_menubar *mb);

/* Open the title whose letter is `ch` (either case). WP_FALSE if none. */
wp_bool wp_menubar_open_letter(wp_menubar *mb, u32 ch);

/* Open title t (Alt+letter, a click on it). */
void wp_menubar_open(wp_menubar *mb, u32 t);

void wp_menubar_close(wp_menubar *mb);

/* A key while the bar is lit or a menu is down. */
wp_menubar_outcome wp_menubar_feed(wp_menubar *mb, wp_key_event ev);

/* Title t's dropdown on a screen `cols` wide, with hint_of's hints
   (NULL for none) right of the labels; kept on the screen. */
void wp_menubar_box_of(const wp_menubar *mb, u32 t, wp_menubar_text_fn hint_of, u32 cols,
                       wp_menubar_box *box);

/* The row item i (within title t's) is drawn on. */
u32 wp_menubar_item_row(const wp_menubar *mb, u32 t, u32 i, const wp_menubar_box *box);

/* The title under bar column col, or -1. */
i32 wp_menubar_title_at(const wp_menubar *mb, u32 col);

/* The item of title t at (col, row) inside its box, or -1 (a border,
   a rule, outside). */
i32 wp_menubar_item_at(const wp_menubar *mb, u32 t, const wp_menubar_box *box, u32 col, u32 row);

/* Every action in a menu group, once, in the bar; letters unique per
   menu; every box fits a cols x rows screen under the bar. The DOS
   shell's on-target menu check and the host tests both call it. */
wp_bool wp_menubar_selfcheck(const wp_menubar *mb, wp_menubar_text_fn hint_of, u32 cols, u32 rows);

#endif
