#include <string.h>
#include <gem.h>
#include "platform.h"

/* Real GEM form_do()-based "choose one of N" dialog (M8.5), replacing
   the M8.5-Phase-6 placeholder that always picked the default option.
   form_do() is used nowhere else in this codebase -- genuinely new
   work, not a thin wrapper, though it reuses the array-index ob_next/
   ob_head/ob_tail conventions the hand-built menu tree in app_shell.c
   already established (see docs/atari-platform.md's "Hand-built menu
   OBJECT tree" section). No resource-file compiler exists in this
   project's toolchain (same reason the menu tree is hand-built, not
   loaded from a .rsc), so this OBJECT tree is built at runtime, sized
   to the caller's actual option_count. */

#define WP_DIALOG_MAX_OPTIONS 9
#define WP_DIALOG_OBJ_COUNT   (WP_DIALOG_MAX_OPTIONS + 4) /* root, title, options..., OK, Cancel */

static OBJECT g_dialog_tree[WP_DIALOG_OBJ_COUNT];
static char   g_dialog_title_buf[40];
static char   g_dialog_option_bufs[WP_DIALOG_MAX_OPTIONS][32];
static char   g_dialog_ok_buf[] = "  OK  ";
static char   g_dialog_cancel_buf[] = "Cancel";

/* ASCII-only, same limitation plat_utf8_to_native already documents
   elsewhere in this backend (draw_atari.c) -- a real ISO-8859/ATASCII
   transliteration table is future work, not attempted here. */
static void copy_bounded(char *out, u32 out_cap, const u8 *utf8, u32 len)
{
    u32 i, n;

    n = (len < out_cap - 1) ? len : out_cap - 1;
    for (i = 0; i < n; i++) {
        u8 c = utf8[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[n] = '\0';
}

static void set_obj(short index, short next, short head, short tail,
                     unsigned short type, unsigned short flags, unsigned short state,
                     char *text, short x, short y, short w, short h)
{
    g_dialog_tree[index].ob_next = next;
    g_dialog_tree[index].ob_head = head;
    g_dialog_tree[index].ob_tail = tail;
    g_dialog_tree[index].ob_type = type;
    g_dialog_tree[index].ob_flags = flags;
    g_dialog_tree[index].ob_state = state;
    g_dialog_tree[index].ob_spec.free_string = text;
    g_dialog_tree[index].ob_x = x;
    g_dialog_tree[index].ob_y = y;
    g_dialog_tree[index].ob_width = w;
    g_dialog_tree[index].ob_height = h;
}

/* Builds g_dialog_tree for a "choose one of N" dialog and computes its
   final screen position via form_center() -- everything
   plat_dialog_choice needs EXCEPT the genuinely interactive part
   (form_dial/objc_draw/form_do). Split out so wp_atari_dialog_selfcheck
   below can exercise the tree-construction logic (the part with real
   index arithmetic that can go wrong -- see child_chain_ok()'s own
   rationale in app_shell.c) without blocking on user input, the same
   boundary every other wp_atari_*_selfcheck in this codebase already
   respects. form_center() itself is a pure screen-geometry computation,
   not an interactive call -- safe to run here. */
static wp_status build_dialog_tree(const u8 *title_utf8, u32 title_len,
                                    const u8 * const *option_labels_utf8,
                                    const u32 *option_label_lens,
                                    u32 option_count, u32 default_index,
                                    short *out_first_option, short *out_ok_obj,
                                    short *out_cancel_obj,
                                    short *out_cx, short *out_cy, short *out_cw, short *out_ch)
{
    short root = 0, title_obj = 1, first_option = 2;
    short ok_obj, cancel_obj;
    short char_w, char_h, box_w, box_h;
    short content_w, y;
    u32 i;

    if (option_count == 0 || option_count > WP_DIALOG_MAX_OPTIONS) return WP_RANGE;

    /* Real character cell size for the current resolution -- the same
       call app_shell_run() already makes for the screen-width check
       (see the "80 column" startup warning), here used for real this
       time instead of discarded into a dummy. Unlike document text
       layout (which needs plat_measure_text's real DPI-correct
       metrics), a picker dialog's own box sizing only needs to be
       "roughly right," so the system font's own cell size is good
       enough without opening a real font handle. */
    graf_handle(&char_w, &char_h, &box_w, &box_h);
    WP_UNUSED(box_w);
    WP_UNUSED(box_h);

    copy_bounded(g_dialog_title_buf, (u32)sizeof(g_dialog_title_buf), title_utf8, title_len);

    ok_obj = (short)(first_option + (short)option_count);
    cancel_obj = (short)(ok_obj + 1);

    content_w = (short)(((short)strlen(g_dialog_title_buf) + 4) * char_w);
    for (i = 0; i < option_count; i++) {
        short w;

        copy_bounded(g_dialog_option_bufs[i], (u32)sizeof(g_dialog_option_bufs[i]),
                     option_labels_utf8[i], option_label_lens[i]);
        w = (short)(((short)strlen(g_dialog_option_bufs[i]) + 6) * char_w);
        if (w > content_w) content_w = w;
    }
    if (content_w < 16 * char_w) content_w = (short)(16 * char_w);

    set_obj(root, -1, title_obj, cancel_obj, G_BOX, OF_NONE, OS_OUTLINED, NULL,
            0, 0, content_w, 0 /* height finalized below, once y is known */);

    y = (short)(char_h / 2);
    set_obj(title_obj, first_option, -1, -1, G_STRING, OF_NONE, OS_NORMAL,
            g_dialog_title_buf, char_w, y, (short)(content_w - 2 * char_w), char_h);
    y = (short)(y + char_h + char_h / 2);

    for (i = 0; i < option_count; i++) {
        short obj = (short)(first_option + (short)i);
        short next = (short)((i + 1 < option_count) ? obj + 1 : ok_obj);
        unsigned short state = (i == default_index) ? OS_SELECTED : OS_NORMAL;

        set_obj(obj, next, -1, -1, G_BUTTON, (unsigned short)(OF_SELECTABLE | OF_RBUTTON), state,
                g_dialog_option_bufs[i], (short)(2 * char_w), y, (short)(content_w - 4 * char_w), char_h);
        y = (short)(y + char_h);
    }
    y = (short)(y + char_h / 2);

    set_obj(ok_obj, cancel_obj, -1, -1, G_BUTTON,
            (unsigned short)(OF_SELECTABLE | OF_DEFAULT | OF_EXIT), OS_NORMAL,
            g_dialog_ok_buf, (short)(2 * char_w), y, (short)(8 * char_w), char_h);
    set_obj(cancel_obj, root, -1, -1, G_BUTTON,
            (unsigned short)(OF_SELECTABLE | OF_EXIT), OS_NORMAL,
            g_dialog_cancel_buf, (short)(content_w - 10 * char_w), y, (short)(8 * char_w), char_h);

    g_dialog_tree[root].ob_height = (short)(y + char_h + char_h / 2);

    form_center(g_dialog_tree, out_cx, out_cy, out_cw, out_ch);
    g_dialog_tree[root].ob_x = *out_cx;
    g_dialog_tree[root].ob_y = *out_cy;
    g_dialog_tree[root].ob_width = *out_cw;
    g_dialog_tree[root].ob_height = *out_ch;

    *out_first_option = first_option;
    *out_ok_obj = ok_obj;
    *out_cancel_obj = cancel_obj;
    return WP_OK;
}

wp_status plat_dialog_choice(plat_window *parent,
                              const u8 *title_utf8, u32 title_len,
                              const u8 * const *option_labels_utf8,
                              const u32 *option_label_lens,
                              u32 option_count, u32 default_index,
                              wp_bool *out_cancelled, u32 *out_index)
{
    short first_option, ok_obj, cancel_obj;
    short cx, cy, cw, ch;
    short chosen_obj;
    wp_status st;
    u32 i;

    WP_UNUSED(parent);

    st = build_dialog_tree(title_utf8, title_len, option_labels_utf8, option_label_lens,
                            option_count, default_index,
                            &first_option, &ok_obj, &cancel_obj, &cx, &cy, &cw, &ch);
    if (st != WP_OK) return st;

    form_dial(FMD_START, cx, cy, 0, 0, cx, cy, cw, ch);
    objc_draw(g_dialog_tree, 0, MAX_DEPTH, cx, cy, cw, ch);

    chosen_obj = (short)(form_do(g_dialog_tree, first_option) & 0x7FFF);

    form_dial(FMD_FINISH, cx, cy, 0, 0, cx, cy, cw, ch);

    if (out_cancelled) *out_cancelled = (wp_bool)(chosen_obj == cancel_obj);
    if (chosen_obj != cancel_obj && out_index) {
        for (i = 0; i < option_count; i++) {
            if (g_dialog_tree[first_option + i].ob_state & OS_SELECTED) {
                *out_index = i;
                break;
            }
        }
    }

    return WP_OK;
}

/* Proves build_dialog_tree's index arithmetic for a representative
   option_count (the largest real caller, Font Size's 7 options) without
   ever calling the blocking, interactive form_do() -- same boundary
   every other selfcheck in this codebase respects. Bounded-step walk
   (never more than WP_DIALOG_OBJ_COUNT hops) matches
   wp_menu_tree_selfcheck's own child_chain_ok() rationale: catches an
   accidental ob_next cycle or broken chain on host/CI, before ever
   booting real Hatari. */
wp_bool wp_atari_dialog_selfcheck(void)
{
    static const char *const labels_c[] = { "8 pt", "10 pt", "12 pt", "14 pt", "18 pt", "24 pt", "36 pt" };
    const u8 *labels[7];
    u32 lens[7];
    short first_option, ok_obj, cancel_obj;
    short cx, cy, cw, ch;
    u32 i;
    short steps;
    short cur;

    for (i = 0; i < 7; i++) {
        labels[i] = (const u8 *)labels_c[i];
        lens[i] = (u32)strlen(labels_c[i]);
    }

    if (build_dialog_tree((const u8 *)"Font Size", 9, labels, lens, 7, 2,
                           &first_option, &ok_obj, &cancel_obj, &cx, &cy, &cw, &ch) != WP_OK) {
        return WP_FALSE;
    }

    if (first_option != 2) return WP_FALSE;
    if (ok_obj != 9) return WP_FALSE;      /* first_option(2) + 7 options */
    if (cancel_obj != 10) return WP_FALSE;
    if (g_dialog_tree[0].ob_next != -1) return WP_FALSE;   /* true root */
    if (g_dialog_tree[0].ob_head != 1) return WP_FALSE;    /* title */
    if (g_dialog_tree[0].ob_tail != cancel_obj) return WP_FALSE;
    if (cw <= 0 || ch <= 0) return WP_FALSE;

    /* exactly one option starts OS_SELECTED (the default_index=2 seed) */
    {
        u32 selected_count = 0;
        for (i = 0; i < 7; i++) {
            if (g_dialog_tree[first_option + i].ob_state & OS_SELECTED) selected_count++;
        }
        if (selected_count != 1) return WP_FALSE;
        if (!(g_dialog_tree[first_option + 2].ob_state & OS_SELECTED)) return WP_FALSE;
    }

    /* bounded-step ob_next walk from the root's head to its tail --
       must terminate by wrapping back to the root within a bounded
       number of hops, never looping forever on a bad index */
    cur = g_dialog_tree[0].ob_head;
    for (steps = 0; steps < WP_DIALOG_OBJ_COUNT; steps++) {
        if (cur == g_dialog_tree[0].ob_tail) {
            return (wp_bool)(g_dialog_tree[cur].ob_next == 0);
        }
        cur = g_dialog_tree[cur].ob_next;
    }
    return WP_FALSE; /* did not terminate within a bounded number of steps */
}
