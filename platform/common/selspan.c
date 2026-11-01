/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "selspan.h"
#include "editing.h"

/* (run, offset) order within one paragraph. */
static int in_para_compare(u32 run_a, u32 off_a, u32 run_b, u32 off_b)
{
    if (run_a != run_b) return (run_a < run_b) ? -1 : 1;
    if (off_a != off_b) return (off_a < off_b) ? -1 : 1;
    return 0;
}

wp_bool wp_selspan_for_line(wp_selection sel, u32 section, u32 paragraph,
                            const wp_line *line, wp_pos *from_out, wp_pos *to_out)
{
    wp_pos lo, hi;
    u32 a_run, a_off, b_run, b_off;
    wp_bool lo_here, hi_here;

    if (wp_pos_compare(sel.anchor, sel.caret) <= 0) { lo = sel.anchor; hi = sel.caret; }
    else                                             { lo = sel.caret;  hi = sel.anchor; }

    /* Outside the selected paragraphs entirely. */
    if (section < lo.section || section > hi.section) return WP_FALSE;
    if (section == lo.section && paragraph < lo.paragraph) return WP_FALSE;
    if (section == hi.section && paragraph > hi.paragraph) return WP_FALSE;

    lo_here = (wp_bool)(section == lo.section && paragraph == lo.paragraph);
    hi_here = (wp_bool)(section == hi.section && paragraph == hi.paragraph);

    /* Start at the line's own start, or later if the selection begins
       on this line; end at the line's end, or earlier if it ends here. */
    a_run = line->start_run;
    a_off = line->start_offset;
    if (lo_here && in_para_compare(lo.run, lo.offset, a_run, a_off) > 0) {
        a_run = lo.run;
        a_off = lo.offset;
    }
    b_run = line->end_run;
    b_off = line->end_offset;
    if (hi_here && in_para_compare(hi.run, hi.offset, b_run, b_off) < 0) {
        b_run = hi.run;
        b_off = hi.offset;
    }

    if (in_para_compare(a_run, a_off, b_run, b_off) >= 0) return WP_FALSE;

    if (from_out != NULL) {
        from_out->section = section;
        from_out->paragraph = paragraph;
        from_out->run = a_run;
        from_out->offset = a_off;
    }
    if (to_out != NULL) {
        to_out->section = section;
        to_out->paragraph = paragraph;
        to_out->run = b_run;
        to_out->offset = b_off;
    }
    return WP_TRUE;
}
