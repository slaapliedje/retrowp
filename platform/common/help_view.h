/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_HELP_VIEW_H
#define WP_HELP_VIEW_H

#include "types.h"
#include "platform.h"
#include "keyevent.h"
#include "help_doc.h"

/* A simple, full-screen, scrolling help pager (M8.5 Phase 10) over a
   parsed wp_help_doc (help_doc.h). Renders through the SAME
   plat_draw_text/plat_fill_rect seam the document body and status line
   already use -- no new platform.h primitive, matching platform/common/
   statusline.c's own precedent. See docs/help.md for the full account.

   Build discipline: this file (and help_view.c) may call platform.h
   functions but must NEVER #include a native header (gem.h,
   exec/types.h, MacTypes.h, dos.h). Enforced by construction, not just
   convention -- platform/common/ is compiled by EVERY target's own
   toolchain; a stray native #include breaks the host build immediately,
   since no such header exists there.

   Deliberately NOT vi-mode: a read-only browsing widget (motion,
   "follow this link", "go back") has no Insert/Visual/Command-line
   concept, so reusing wp_vimode_state here would be a category error,
   not a reuse win -- this gets its own small, independent key-handling
   state instead, built on the same portable wp_key_event Phase 4
   already defined. */

#define WP_HELP_HISTORY_MAX 16

typedef struct {
    const wp_help_doc *doc;  /* borrowed; may be NULL (nothing to show) */
    u32 current_node;        /* WP_HELP_NODE_NONE if doc has no nodes */
    u32 scroll_row;           /* first visible WRAPPED row within the current node; reset to 0 on every node change */
    u32 selected_link;        /* index among the CURRENT node's link-kind lines, 0-based */
    u32 history[WP_HELP_HISTORY_MAX];
    u32 history_count;        /* a history stack deeper than this simply stops growing --
                                  Backspace/back still works, it just can't unwind past this
                                  many hops; a real, documented, low-stakes MVP limit */
} wp_help_view_state;

/* start_node is clamped to doc's real node range (falls back to node 0,
   or to WP_HELP_NODE_NONE if doc has zero nodes -- a doc with no @node
   lines at all is a real, tolerated possibility, see help_doc.h's own
   parser leniency note). */
void wp_help_view_init(wp_help_view_state *vs, const wp_help_doc *doc, u32 start_node);

/* The entire input surface: motion (Up/Down or k/j move the selected-
   link index; Page Up/Down scroll by a fixed approximate page --
   WP_HELP_PAGE_ROWS in help_view.c -- since this state has no idea how
   many rows the viewport actually fits until paint time, see
   wp_help_view_paint's own params), Enter follows the selected link
   (pushing the current node onto the history stack), Backspace goes
   back. Returns WP_TRUE if the caller should close the viewer (Escape
   or 'q') -- the same "caller decides what closing means" seam
   wp_vimode_action's WP_VIACTION_QUIT already established.

   Deliberately does NOT auto-scroll to keep the selected link on
   screen: doing so needs the same wrap-layout knowledge only
   wp_help_view_paint currently computes (and only at paint time, once
   viewport width is known) -- a real, documented MVP limitation, not an
   oversight. Help content authored for this project is short enough
   per node that this is a low-cost simplification; Page Up/Down remain
   available to reach a link that's scrolled out of view. */
wp_bool wp_help_view_feed_key(wp_help_view_state *vs, wp_key_event key);

/* x_px/y_px/w_px/h_px are the viewport rect in plain pixels, the same
   unit plat_draw_text/plat_fill_rect already use (matches statusline.h's
   own convention). content_width_twips is the SEPARATE width to word-
   wrap body text against, in twips -- plat_measure_text (CLAUDE.md rule
   4) only ever reports widths in twips, and platform/common/ has no
   twips<->pixel conversion of its own (that ratio is real-DPI-dependent
   and backend-specific, e.g. platform/atari/metrics_atari.c's
   atari_twips_to_px_x/y) -- so the caller supplies content_width_twips
   directly (typically atari_px_to_twips_x(w_px), the exact conversion
   the main document body already performs) rather than this function
   guessing at one. line_height_px/ascent_px are likewise supplied by
   the caller (its own plat_font_metrics_get result, already converted
   to pixels -- see statusline_height_px() in app_shell.c for the
   established pattern this mirrors) rather than recomputed here.

   ascent_px matters because plat_draw_text (like every real backend's
   Text()/v_gtext()) positions text by BASELINE, not top-left -- every
   row's y-coordinate is y_px + ascent_px + row*line_height_px, mirroring
   exactly how the main document body's own paint loop adds its font's
   ascent before calling plat_draw_text. Found and fixed the hard way
   (M9 Phase 6): every caller of this function used to pass a bare
   y_px with no ascent offset at all, which drew row 0's ascending
   strokes ABOVE the viewport's own top edge -- genuinely outside any
   rectangle the caller's own background-clear covers, so the leaked
   pixels never got cleared by a subsequent redraw either. Invisible on
   host (draw_host.c's plat_draw_text/plat_fill_rect are real no-ops, so
   host/tests/test_help_view.c can only prove this function doesn't
   crash, never that it draws in the right place) and never caught on
   Atari either, since platform/atari/app_shell.c's own pager was never
   interactively verified end-to-end (docs/help.md's own documented
   gap) -- only Amiga's Phase 6 work actually clicked through the real
   pager on real target hardware, which is what surfaced this. */
void wp_help_view_paint(plat_gc *gc, plat_font *font,
                         i32 x_px, i32 y_px, i32 w_px, i32 h_px,
                         i32 content_width_twips, i32 line_height_px,
                         i32 ascent_px, const wp_help_view_state *vs);

/* Splits [text, text+len) into greedy word-wrapped rows no wider than
   max_width_twips (via plat_measure_text -- the same measurement seam
   the main document body's own layout uses, CLAUDE.md rule 4), writing
   up to max_rows entries into out_starts/out_lens (each a byte offset/
   length into `text`) and returning the real row count, capped at
   max_rows if the line has more rows than that (a line that long is
   not realistic authored help content -- see help_view.c's own
   WP_HELP_MAX_WRAP_TOKENS comment).

   This is wp_help_view_paint's own internal wrap engine, exposed as a
   standalone function specifically so its correctness is directly
   testable (host/tests/test_help_view.c, against the host backend's
   deterministic fake-monospace plat_measure_text) rather than only
   provable by "does wp_help_view_paint crash" -- platform/host/
   draw_host.c's plat_draw_text/plat_fill_rect are no-ops (no real
   drawing surface on host), so paint() itself can only be crash-tested,
   the same boundary platform/common/statusline.c's own test already
   respects. */
u32 wp_help_wrap_line(plat_font *font, const u8 *text, u32 len, i32 max_width_twips,
                       u32 *out_starts, u32 *out_lens, u32 max_rows);

#endif /* WP_HELP_VIEW_H */
