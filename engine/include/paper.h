/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_PAPER_H
#define WP_PAPER_H

#include "types.h"
#include "docmodel.h"

/* Paper sizes (M19): the sheets, envelopes, cards and disc labels Page
   Setup offers, each in twips, portrait (width not more than height);
   landscape is the two swapped. A section's page is just its width and
   height (docmodel.h) -- this table only names the sizes and says what
   kind each is, which page view uses to draw a disc's outline and an
   envelope's default turn. */

typedef enum {
    WP_PAPER_SHEET = 0,
    WP_PAPER_ENVELOPE,     /* addressed across its length: landscape */
    WP_PAPER_CARD,
    WP_PAPER_DISC          /* a printable CD/DVD: the page is the disc's square */
} wp_paper_kind;

typedef struct {
    const char *name;      /* as Page Setup lists it */
    i32 w, h;              /* twips, portrait */
    u8  kind;              /* wp_paper_kind */
} wp_paper;

u32             wp_paper_count(void);
const wp_paper *wp_paper_get(u32 index);

/* The named size a page of w x h is, either way round (within a few
   twips), and *landscape whether it is turned; NULL for none. */
const wp_paper *wp_paper_match(i32 w, i32 h, wp_bool *landscape);

/* A disc label's geometry: the disc's diameter and its hole's, twips
   (120 mm and the 41 mm clear centre a printable disc leaves). */
#define WP_DISC_TWIPS       6803
#define WP_DISC_HOLE_TWIPS  2324

#endif /* WP_PAPER_H */
