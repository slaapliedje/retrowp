/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_GROWTH_INTERNAL_H
#define WP_GROWTH_INTERNAL_H

#include "types.h"

/* Private to engine/src -- not installed under engine/include, not part
   of any public surface. Shared 1.5x geometric growth policy, used for
   both the string pool's byte capacity and every document-model array's
   element capacity (the arithmetic doesn't care what unit it counts).
   1.5x rather than 2x specifically because of the 1MB Atari ST budget:
   doubling a 300KB pool to fit 310KB wastes 290KB immediately, whereas
   1.5x keeps the same amortized O(1) append cost with a much smaller
   worst-case overshoot.
   Defined `static` so each including translation unit gets its own
   private copy -- this header has no matching .c file. */
static u32 wp_grow_capacity(u32 current_cap, u32 needed_cap, u32 min_cap)
{
    u32 new_cap = current_cap + (current_cap / 2);
    if (new_cap < needed_cap) new_cap = needed_cap;
    if (new_cap < min_cap)    new_cap = min_cap;
    return new_cap;
}

#endif /* WP_GROWTH_INTERNAL_H */
