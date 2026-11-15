/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_SCREENLINE_H
#define WP_SCREENLINE_H

/* Where a line's words go on a screen whose faces are narrower than the
   paper's.

   With paper metrics on (papermetrics.h) a line is laid out as it will
   print, and each word has the width the printer's face gives it. A
   screen's bitmap face is often narrower -- the ST's system font against
   Times -- and a word drawn at its paper x leaves all of its shortfall
   as one gap after it: "ADVENTURER'S     RECORD". Here the words of a
   line are drawn at their own screen widths, close up, and the room left
   over goes where the paragraph's alignment puts it: after the line when
   it is ranged left, before it when ranged right, half each side when
   centred, shared between the gaps when justified. A tab starts a new
   part of the line that keeps its own start, so tab stops still line up.

   The line still breaks where the paper breaks it, and a word wider on
   screen than on paper is held to its paper width (devout.h's
   wp_dev_text_fit), so it never pushes the next one along.

   The caret, the selection and a click must agree with what is drawn,
   so every paper x a painter or hit test works with goes through
   wp_screenline_x, and every screen x back through wp_screenline_paper_x.
   With paper metrics off the screen widths are the paper widths and both
   are the identity.

   All x here are twips from the line's origin (line->x_twips, the start
   every painter draws the line from). */

#include "types.h"
#include "platform.h"
#include "docmodel.h"
#include "layout.h"

#define WP_SCREENLINE_MAX 64   /* words a line keeps; past it, drawn as laid out */

typedef struct {
    i32 paper_x;    /* the layout's start */
    i32 paper_w;    /* the layout's width */
    i32 ink_w;      /* the screen face's width, at most paper_w: what it fills */
    i32 ink_full;   /* the screen face's width as measured: wider than
                       paper_w for a word the fit must squeeze (devout.h) */
    i32 screen_x;   /* where it is drawn */
    u32 byte_start; /* the fragment it is, to find it again */
    u32 run_index;
    u16 gap;        /* spaces before it in its part of the line */
    u8  part;       /* which tab-separated part of the line */
} wp_screen_word;

typedef struct {
    u16 n;
    wp_bool identity;   /* every word where the layout put it */
    wp_screen_word w[WP_SCREENLINE_MAX];
} wp_screenline;

/* A fragment's widths: *paper_w the layout's (the face measured with
   paper metrics as they stand), *ink_w the face the screen draws it in.
   Returns WP_FALSE when the fragment has no face (the painter skips it
   too). *ink_w < 0: not known, taken as the paper width. */
typedef wp_bool (*wp_screenline_measure_fn)(void *user, wp_document *doc, u16 style,
                                            const u8 *utf8, u32 len, i32 *paper_w, i32 *ink_w);

/* Measures a line's fragments and places its words. extra_twips is the
   justify extra every space gets (wp_justify_space_extra_twips). */
void wp_screenline_build(wp_screenline *sl, wp_document *doc, wp_paragraph *para,
                         const wp_line *line, u8 align, i32 extra_twips,
                         wp_screenline_measure_fn measure, void *user);

/* The pieces of build, for a caller with its own widths (and the tests):
   reset, add the words in order, then place. */
void wp_screenline_reset(wp_screenline *sl);
void wp_screenline_add(wp_screenline *sl, i32 paper_x, i32 paper_w, i32 ink_w,
                       u16 gap, u8 part, u32 run_index, u32 byte_start);
void wp_screenline_place(wp_screenline *sl, u8 align, wp_bool justified);

/* The word a painter's fragment is, or NULL (a space, a tab, a word past
   the cap). *k is where to start looking; it moves past the match. */
const wp_screen_word *wp_screenline_word(const wp_screenline *sl, u16 *k,
                                         u32 run_index, u32 byte_start);

/* A paper x on the screen, and back. Monotonic both ways. */
i32 wp_screenline_x(const wp_screenline *sl, i32 paper_x);
i32 wp_screenline_paper_x(const wp_screenline *sl, i32 screen_x);

/* The normal views' measure: the style's face opened at its own size,
   cached across calls in *fonts. Close with wp_screen_fonts_close. */
typedef struct {
    plat_font *font;
    u16 style;
    wp_bool have;
} wp_screen_fonts;

wp_bool wp_screenline_measure_screen(void *fonts, wp_document *doc, u16 style,
                                     const u8 *utf8, u32 len, i32 *paper_w, i32 *ink_w);
void wp_screen_fonts_close(wp_screen_fonts *fonts);

#endif
