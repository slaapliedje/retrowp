/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_MENUCHECK_H
#define WP_MENUCHECK_H

#include "types.h"
#include "actions.h"

/* Shared menu-construction rules + the portable half of every backend's
   on-target menu selfcheck (M8.5 follow-up).
   ------------------------------------------------------------------
   Three of the four backends build their native menu by folding
   g_wp_actions[] over a toolkit-specific hierarchy (Atari's GEM OBJECT
   tree is the exception: a static, hand-built array). Each of those
   folds independently re-derived the same two rules -- "which byte in
   .shortcut_ascii becomes a displayable accelerator letter" and "is
   this label safe to hand to the native menu call" -- and each got one
   of them wrong in a different way:

     - Amiga computed the accelerator as shortcut_ascii + 0x40
       unconditionally, which is correct for 0x01..0x1A but turned
       WP_ACT_FORMAT_SIZE's 0x1D into ']'.
     - Mac never derived an accelerator at all, so MenuKey() returned 0
       for every item and the cmdKey branch swallowed the keystroke
       before the editor could see it.

   Two copies of one rule is what made both possible, so the rule lives
   here once and the backends call it. wp_menu_model_selfcheck() then
   checks the table itself against every invariant those folds depend
   on, so a backend's on-target menu check gets the portable half for
   free and only has to add its own native specifics.

   Build discipline: platform/common/, so this may call platform.h but
   must never include a native header -- see actions.h's own comment. */

/* The accelerator letter a native menu should display for this action,
   or 0 for "this action has no accelerator".

   Only a real Ctrl+<letter> control code (0x01..0x1A) maps onto a
   letter. actions.h explicitly allows plain printable ASCII in the same
   byte space (WP_ACT_FORMAT_SIZE uses 0x1D, WP_ACT_FORMAT_ALIGN 0x0C),
   so the range must be CHECKED, never assumed -- returning 0 for those
   is the correct answer, not a fallback. */
u8 wp_menu_accel_letter(u8 shortcut_ascii);

/* WP_TRUE if label is safe to pass to a native menu call that parses
   metacharacters out of the string it is given.

   The Mac Menu Manager's AppendMenu/InsertMenuItem is the strict case:
   ';' (and Return) splits one call into TWO items, '/' introduces a
   command key, '(' disables the item, '^'/'!'/'<' set icon/mark/style,
   and a leading '-' makes a divider. A backend that maps a chosen item
   back to an action by counting ordinal position -- which Mac's
   do_menu_command does -- would silently fire the WRONG action for
   every item below a label containing one of these. The Mac builder
   avoids the parsing entirely (placeholder + SetMenuItemText), so this
   is defence in depth for a future backend that does not, and a
   tripwire on the labels themselves. */
wp_bool wp_menu_label_is_literal_safe(const char *label);

/* Validates g_wp_actions[] against every invariant the four menu
   builders depend on: row order matches .id (backends index
   per-action arrays by .id), every action has a usable label, labels
   are literal-safe, shortcut bytes are in a defined range and unique,
   groups are in range, and no menu group is empty (an empty group
   makes Amiga/Mac silently drop a whole title).

   Platform-free and identical on every target, so each backend's own
   wp_*_menu_selfcheck() calls this first and then adds its native
   half. Also run on the host (host/tests/test_menucheck.c) -- the
   on-target copies exist to prove the SAME table survives that
   target's own compiler and word size, which the host cannot show. */
wp_bool wp_menu_model_selfcheck(void);

#endif /* WP_MENUCHECK_H */
