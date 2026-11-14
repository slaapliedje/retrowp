/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_LABELS_H
#define WP_LABELS_H

#include "types.h"
#include "docmodel.h"

/* Label sheets (M19): the common Avery sheets, Letter and A4, each as
   the sheet's size, the first label's corner, the label's size, the
   distance from one label to the next across and down (its pitch), and
   how many there are each way. Labels are made by wp_merge_labels
   (merge.h) as a table on the sheet: wp_label_sheet_apply gives a
   section the sheet's paper and margins so the table's cells fall a
   pitch apart from the first label's corner, and answers the cell gap
   that puts each label's text a twelfth of an inch in from its own
   edges -- the space between two labels is split either side of a
   cell's boundary. */

typedef struct {
    const char *name;        /* as the Labels list shows it: product and use */
    i32 sheet_w, sheet_h;    /* twips */
    i32 left, top;           /* the first label's corner from the sheet's */
    i32 label_w, label_h;
    i32 pitch_x, pitch_y;    /* one label's corner to the next's */
    u8  across, down;
} wp_label_sheet;

u32                   wp_label_sheet_count(void);
const wp_label_sheet *wp_label_sheet_get(u32 index);

/* sec's paper and margins set for the sheet; answers the table row's
   cell gap (wp_table_row.gap) for wp_merge_labels. */
i32 wp_label_sheet_apply(wp_section *sec, const wp_label_sheet *t);

/* The space a label's text keeps from its edges, twips. */
#define WP_LABEL_INSET 115

#endif /* WP_LABELS_H */
