#include <stddef.h>
#include "menucheck.h"

u8 wp_menu_accel_letter(u8 shortcut_ascii)
{
    if (shortcut_ascii >= 0x01 && shortcut_ascii <= 0x1A) {
        return (u8)('A' + shortcut_ascii - 1);
    }
    return 0;
}

wp_bool wp_menu_label_is_literal_safe(const char *label)
{
    u32 i;

    if (label == NULL) return WP_FALSE;
    if (label[0] == '\0') return WP_FALSE;
    if (label[0] == '-') return WP_FALSE; /* leading '-' = divider */

    for (i = 0; label[i] != '\0'; i++) {
        char c = label[i];

        if (c == ';' || c == '/' || c == '(' || c == '^' ||
            c == '!' || c == '<' || c == '\r' || c == '\n') {
            return WP_FALSE;
        }
    }
    return WP_TRUE;
}

wp_bool wp_menu_model_selfcheck(void)
{
    static const wp_menu_group groups[] = {
        WP_MENU_FILE, WP_MENU_EDIT, WP_MENU_FORMAT, WP_MENU_TOOLS, WP_MENU_HELP
    };
    const u32 group_count = (u32)(sizeof(groups) / sizeof(groups[0]));
    u32 i, j;

    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        const wp_action *act = &g_wp_actions[i];

        /* Row order IS the contract, not a coincidence: platform/amiga's
           g_menu_shortcut[act->id] indexes a per-action array by id, so a
           table whose rows drift out of enum order would index the wrong
           slot rather than fail to compile. */
        if ((u32)act->id != i) return WP_FALSE;
        if (wp_action_find((wp_action_id)i) != act) return WP_FALSE;

        if (!wp_menu_label_is_literal_safe(act->label_utf8)) return WP_FALSE;

        /* Group must be one this project actually builds a menu for, or
           the explicit "keyboard only" sentinel. A stray value would make
           the action invisible on every backend without any diagnostic. */
        if (act->group != WP_MENU_NONE) {
            wp_bool known = WP_FALSE;

            for (j = 0; j < group_count; j++) {
                if (act->group == groups[j]) known = WP_TRUE;
            }
            if (!known) return WP_FALSE;
        }

        if (act->shortcut_ascii != 0) {
            /* Either a Ctrl-letter control code or plain printable
               ASCII -- actions.h's documented byte space. A byte outside
               both (a stray 0x1B-0x1F other than the two deliberate
               ones, say) means some backend will render a garbage
               accelerator glyph. */
            wp_bool ctrl_letter =
                (wp_bool)(act->shortcut_ascii >= 0x01 && act->shortcut_ascii <= 0x1A);
            wp_bool printable =
                (wp_bool)(act->shortcut_ascii >= 0x20 && act->shortcut_ascii <= 0x7E);
            wp_bool named_nonletter =
                (wp_bool)(act->shortcut_ascii == 0x1D || act->shortcut_ascii == 0x0C);

            if (!ctrl_letter && !printable && !named_nonletter) return WP_FALSE;

            /* wp_action_find_by_shortcut returns the FIRST match, so a
               duplicate byte would make one of the two actions
               permanently unreachable by keyboard -- and on DOS, which
               has no menu at all, permanently unreachable full stop. */
            if (wp_action_find_by_shortcut(act->shortcut_ascii) == NULL) return WP_FALSE;
            for (j = 0; j < (u32)WP_ACT_COUNT; j++) {
                if (j == i) continue;
                if (g_wp_actions[j].shortcut_ascii == act->shortcut_ascii) return WP_FALSE;
            }
        }

        /* A NULL handler means "the backend supplies this one" (file
           pickers, offload round trips). That is legitimate, but the
           action must at least be reachable somehow, or it is dead
           weight in every menu that shows it. */
        if (act->handler == NULL && act->group == WP_MENU_NONE &&
            act->shortcut_ascii == 0) {
            return WP_FALSE;
        }
    }

    /* Every group this project builds a title for must have at least one
       item. Amiga's and Mac's folds both `continue` past an empty group,
       which drops the whole title silently -- the user sees a menu bar
       that is simply missing "Format" with nothing logged anywhere. */
    for (j = 0; j < group_count; j++) {
        wp_bool any = WP_FALSE;

        for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
            if (g_wp_actions[i].group == groups[j]) any = WP_TRUE;
        }
        if (!any) return WP_FALSE;
    }

    /* Out-of-range lookups must be NULL, not a wild row -- every
       backend's invoke_action() feeds this a u8 that came out of a
       keymap outcome or a menu id. */
    if (wp_action_find((wp_action_id)WP_ACT_COUNT) != NULL) return WP_FALSE;
    if (wp_action_find_by_shortcut(0) != NULL) return WP_FALSE;

    return WP_TRUE;
}
