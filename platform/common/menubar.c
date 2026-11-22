/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "menubar.h"

#define BAR_FIRST_COL 2    /* "  File  Edit ...", as DOS Edit lays it out */
#define BAR_GAP       2

static u32 upper(u32 c)
{
    return (c >= 'a' && c <= 'z') ? c - ('a' - 'A') : c;
}

static wp_bool is_letter(u32 c)
{
    c = upper(c);
    return (wp_bool)((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
}

/* The first letter of `label` not already taken (taken[] is indexed by
   the uppercase byte), which it then takes. */
static u8 pick_letter(const char *label, u8 *taken)
{
    u32 i;

    for (i = 0; label[i] != '\0' && i < 255u; i++) {
        u32 c = upper((u8)label[i]);
        if (is_letter(c) && !taken[c]) {
            taken[c] = 1;
            return (u8)i;
        }
    }
    return WP_MENUBAR_NO_LETTER;
}

static const char *label_for(wp_menubar_text_fn label_of, const wp_action *a)
{
    const char *s = label_of != NULL ? label_of((wp_action_id)a->id) : NULL;
    return (s != NULL && s[0] != '\0') ? s : a->label_utf8;
}

static void add_item(wp_menubar *mb, const wp_action *a, wp_menubar_text_fn label_of, u8 rule)
{
    wp_menubar_item *it = &mb->items[mb->item_count++];

    it->action = (u8)a->id;
    it->label = label_for(label_of, a);
    it->letter = WP_MENUBAR_NO_LETTER;
    it->rule_before = rule;
}

void wp_menubar_build(wp_menubar *mb, const u8 *groups, const char *const *titles, u32 n,
                      wp_menubar_text_fn label_of)
{
    u8 taken[256];
    u32 g, a, col = BAR_FIRST_COL;

    memset(mb, 0, sizeof(*mb));
    memset(taken, 0, sizeof(taken));
    if (n > WP_MENUBAR_MAX_TITLES) n = WP_MENUBAR_MAX_TITLES;
    for (g = 0; g < n; g++) {
        wp_menubar_title *t = &mb->titles[mb->title_count];
        u8 item_taken[256];
        u32 i;

        t->first = mb->item_count;
        for (a = 0; a < (u32)WP_ACT_COUNT; a++) {
            const wp_action *act = wp_action_menu_order(a);
            if (act->group == groups[g] && act->id != WP_ACT_FILE_QUIT) add_item(mb, act, label_of, 0);
        }
        /* Quit last, under a rule, where a DOS program keeps Exit. */
        if (groups[g] == (u8)WP_MENU_FILE) {
            const wp_action *q = wp_action_find(WP_ACT_FILE_QUIT);
            if (q != NULL && q->group == (u8)WP_MENU_FILE) {
                add_item(mb, q, label_of, (u8)(mb->item_count > t->first ? 1 : 0));
            }
        }
        t->count = (u8)(mb->item_count - t->first);
        if (t->count == 0) continue;        /* an empty group: no title */

        t->group = groups[g];
        t->label = titles[g];
        t->letter = pick_letter(t->label, taken);
        t->col = (u8)col;
        col += (u32)strlen(t->label) + BAR_GAP;
        memset(item_taken, 0, sizeof(item_taken));
        for (i = 0; i < t->count; i++) {
            wp_menubar_item *it = &mb->items[t->first + i];
            it->letter = pick_letter(it->label, item_taken);
        }
        mb->title_count++;
    }
}

void wp_menubar_close(wp_menubar *mb)
{
    mb->mode = (u8)WP_MENUBAR_CLOSED;
}

void wp_menubar_toggle(wp_menubar *mb)
{
    if (mb->mode != (u8)WP_MENUBAR_CLOSED) {
        wp_menubar_close(mb);
        return;
    }
    if (mb->title_count == 0) return;
    mb->mode = (u8)WP_MENUBAR_BAR;
    mb->title = 0;
    mb->item = 0;
}

void wp_menubar_open(wp_menubar *mb, u32 t)
{
    if (t >= mb->title_count) return;
    mb->mode = (u8)WP_MENUBAR_OPEN;
    mb->title = (u8)t;
    mb->item = 0;
}

static i32 title_by_letter(const wp_menubar *mb, u32 ch)
{
    u32 t;

    ch = upper(ch);
    for (t = 0; t < mb->title_count; t++) {
        const wp_menubar_title *ti = &mb->titles[t];
        if (ti->letter != WP_MENUBAR_NO_LETTER && upper((u8)ti->label[ti->letter]) == ch) return (i32)t;
    }
    return -1;
}

wp_bool wp_menubar_open_letter(wp_menubar *mb, u32 ch)
{
    i32 t = title_by_letter(mb, ch);

    if (t < 0) return WP_FALSE;
    wp_menubar_open(mb, (u32)t);
    return WP_TRUE;
}

static wp_menubar_outcome outcome(wp_menubar_result r, u8 action)
{
    wp_menubar_outcome o;
    o.result = (u8)r;
    o.action = action;
    return o;
}

static wp_menubar_outcome choose(wp_menubar *mb, u32 i)
{
    u8 action = mb->items[mb->titles[mb->title].first + i].action;

    wp_menubar_close(mb);
    return outcome(WP_MENUBAR_CHOSEN, action);
}

wp_menubar_outcome wp_menubar_feed(wp_menubar *mb, wp_key_event ev)
{
    u32 n = mb->title_count;

    if (mb->mode == (u8)WP_MENUBAR_CLOSED || n == 0) return outcome(WP_MENUBAR_NOTHING, 0);
    switch ((wp_special_key)ev.special) {
    case WP_KEY_ESCAPE:
        wp_menubar_close(mb);
        return outcome(WP_MENUBAR_LEFT, 0);
    case WP_KEY_LEFT:
        mb->title = (u8)((mb->title + n - 1) % n);
        mb->item = 0;
        return outcome(WP_MENUBAR_REDRAW, 0);
    case WP_KEY_RIGHT:
        mb->title = (u8)((mb->title + 1) % n);
        mb->item = 0;
        return outcome(WP_MENUBAR_REDRAW, 0);
    case WP_KEY_UP:
    case WP_KEY_DOWN: {
        u32 c = mb->titles[mb->title].count;
        if (mb->mode == (u8)WP_MENUBAR_BAR) {
            mb->mode = (u8)WP_MENUBAR_OPEN;
            mb->item = (u8)(ev.special == (u16)WP_KEY_UP ? c - 1 : 0);
        } else {
            mb->item = (u8)(ev.special == (u16)WP_KEY_UP ? (mb->item + c - 1) % c : (mb->item + 1) % c);
        }
        return outcome(WP_MENUBAR_REDRAW, 0);
    }
    case WP_KEY_HOME:
        if (mb->mode != (u8)WP_MENUBAR_OPEN) return outcome(WP_MENUBAR_NOTHING, 0);
        mb->item = 0;
        return outcome(WP_MENUBAR_REDRAW, 0);
    case WP_KEY_END:
        if (mb->mode != (u8)WP_MENUBAR_OPEN) return outcome(WP_MENUBAR_NOTHING, 0);
        mb->item = (u8)(mb->titles[mb->title].count - 1);
        return outcome(WP_MENUBAR_REDRAW, 0);
    case WP_KEY_ENTER:
        if (mb->mode == (u8)WP_MENUBAR_BAR) {
            wp_menubar_open(mb, mb->title);
            return outcome(WP_MENUBAR_REDRAW, 0);
        }
        return choose(mb, mb->item);
    default:
        break;
    }
    if (ev.special != (u16)WP_KEY_NONE || !is_letter(ev.codepoint) ||
        (ev.mods & (u8)WP_MOD_CTRL) != 0) {
        return outcome(WP_MENUBAR_NOTHING, 0);
    }
    /* A letter: on the bar, a title's; in a menu, one of its items'. */
    if (mb->mode == (u8)WP_MENUBAR_BAR || (ev.mods & (u8)WP_MOD_ALT) != 0) {
        return wp_menubar_open_letter(mb, ev.codepoint) ? outcome(WP_MENUBAR_REDRAW, 0)
                                                        : outcome(WP_MENUBAR_NOTHING, 0);
    }
    {
        const wp_menubar_title *t = &mb->titles[mb->title];
        u32 i, ch = upper(ev.codepoint);

        for (i = 0; i < t->count; i++) {
            const wp_menubar_item *it = &mb->items[t->first + i];
            if (it->letter != WP_MENUBAR_NO_LETTER && upper((u8)it->label[it->letter]) == ch) {
                return choose(mb, i);
            }
        }
    }
    return outcome(WP_MENUBAR_NOTHING, 0);
}

static u32 text_len(const char *s)
{
    return s != NULL ? (u32)strlen(s) : 0u;
}

void wp_menubar_box_of(const wp_menubar *mb, u32 t, wp_menubar_text_fn hint_of, u32 cols,
                       wp_menubar_box *box)
{
    const wp_menubar_title *ti = &mb->titles[t];
    u32 i, label_w = 0, hint_w = 0, rules = 0, w, c;

    for (i = 0; i < ti->count; i++) {
        const wp_menubar_item *it = &mb->items[ti->first + i];
        u32 l = text_len(it->label);
        u32 h = hint_of != NULL ? text_len(hint_of((wp_action_id)it->action)) : 0u;
        if (l > label_w) label_w = l;
        if (h > hint_w) hint_w = h;
        if (it->rule_before) rules++;
    }
    /* "| Label    Hint |": a border and a space each side, three spaces
       between the longest label and the hints. */
    w = 2u + 1u + label_w + (hint_w > 0 ? 3u + hint_w : 0u) + 1u;
    if (w > cols) w = cols;
    c = ti->col >= 2u ? ti->col - 2u : 0u;
    if (c + w > cols) c = cols - w;
    box->col = (u8)c;
    box->row = 1;
    box->width = (u8)w;
    box->height = (u8)(2u + ti->count + rules);
    box->hint_col = (u8)(c + 2u + label_w + 3u);
}

u32 wp_menubar_item_row(const wp_menubar *mb, u32 t, u32 i, const wp_menubar_box *box)
{
    const wp_menubar_title *ti = &mb->titles[t];
    u32 k, row = (u32)box->row + 1u;

    for (k = 0; k <= i && k < ti->count; k++) {
        if (mb->items[ti->first + k].rule_before) row++;
        if (k < i) row++;
    }
    return row;
}

i32 wp_menubar_title_at(const wp_menubar *mb, u32 col)
{
    u32 t;

    for (t = 0; t < mb->title_count; t++) {
        const wp_menubar_title *ti = &mb->titles[t];
        /* The label and the space either side of it, which is lit with it. */
        if (col + 1u >= ti->col && col <= ti->col + text_len(ti->label)) return (i32)t;
    }
    return -1;
}

i32 wp_menubar_item_at(const wp_menubar *mb, u32 t, const wp_menubar_box *box, u32 col, u32 row)
{
    const wp_menubar_title *ti = &mb->titles[t];
    u32 i;

    if (col <= box->col || col + 1u >= (u32)box->col + box->width) return -1;
    for (i = 0; i < ti->count; i++) {
        if (wp_menubar_item_row(mb, t, i, box) == row) return (i32)i;
    }
    return -1;
}

wp_bool wp_menubar_selfcheck(const wp_menubar *mb, wp_menubar_text_fn hint_of, u32 cols, u32 rows)
{
    u8 seen[WP_ACT_COUNT];
    u32 a, t, i;

    memset(seen, 0, sizeof(seen));
    if (mb->title_count == 0) return WP_FALSE;
    for (t = 0; t < mb->title_count; t++) {
        const wp_menubar_title *ti = &mb->titles[t];
        wp_menubar_box box;
        u8 taken[256];

        if (ti->count == 0 || ti->letter == WP_MENUBAR_NO_LETTER) return WP_FALSE;
        if ((u32)ti->col + text_len(ti->label) + 1u > cols) return WP_FALSE;
        wp_menubar_box_of(mb, t, hint_of, cols, &box);
        if ((u32)box.col + box.width > cols || (u32)box.row + box.height > rows) return WP_FALSE;
        memset(taken, 0, sizeof(taken));
        for (i = 0; i < ti->count; i++) {
            const wp_menubar_item *it = &mb->items[ti->first + i];
            u32 c, h = hint_of != NULL ? text_len(hint_of((wp_action_id)it->action)) : 0u;

            /* A label and its hint inside the borders, not cut off. */
            if ((u32)box.col + 2u + text_len(it->label) + 1u > (u32)box.col + box.width) return WP_FALSE;
            if (h > 0 && (u32)box.hint_col + h + 2u > (u32)box.col + box.width) return WP_FALSE;
            if (it->action >= (u8)WP_ACT_COUNT || seen[it->action]) return WP_FALSE;
            seen[it->action] = 1;
            if (it->letter == WP_MENUBAR_NO_LETTER) return WP_FALSE;
            c = upper((u8)it->label[it->letter]);
            if (taken[c]) return WP_FALSE;
            taken[c] = 1;
            if (wp_menubar_item_at(mb, t, &box, (u32)box.col + 2u,
                                   wp_menubar_item_row(mb, t, i, &box)) != (i32)i) return WP_FALSE;
        }
    }
    /* Every action that is in a menu anywhere is in this bar. */
    for (a = 0; a < (u32)WP_ACT_COUNT; a++) {
        if (g_wp_actions[a].group != (u8)WP_MENU_NONE && !seen[a]) return WP_FALSE;
    }
    return WP_TRUE;
}
