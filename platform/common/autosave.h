/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_AUTOSAVE_H
#define WP_AUTOSAVE_H

/* Autosave (M21 T21.3): the unsaved document copied to the recovery file
 * (recovery.h) every so often, so a crash or a power cut costs minutes
 * of work, not an afternoon. The next start offers it exactly as it
 * offers the copy an AP_TERM left.
 *
 * NEVER THE DOCUMENT. Saving is the writer's decision: a copy written
 * over their file behind their back would take away the old version
 * they may have meant to go back to. Autosave writes only the recovery
 * file, and leaves the document marked unsaved.
 *
 * WHEN. There are changes the recovery file does not have; the first of
 * them was made at least `minutes` ago (the config file's
 * "autosave=N", 5 by default, "autosave=off" for never); and typing has
 * paused for WP_AUTOSAVE_PAUSE_MS, because writing a document out on a
 * floppy takes long enough to be felt, and in the middle of a sentence
 * it would be. A document with no unsaved changes is never written: the
 * file on disk already is the work.
 *
 * HOW A SHELL USES IT. It calls wp_autosave_poll after every event and
 * whenever its event wait times out, and makes that wait no longer than
 * the poll asks: 0 means nothing is due and the shell may sleep until
 * the next event. The poll itself is a few comparisons when nothing is
 * due, so calling it often costs nothing.
 *
 * A failed write keeps the last good copy (wp_recovery_write writes the
 * new one whole before it replaces the old), says so once, and is tried
 * again after another interval. A shell with no recovery_path has no
 * autosave, and the poll always answers 0. */

#include "types.h"
#include "format_rtf.h"     /* wp_image_source */

struct wp_app_ctx;

#define WP_AUTOSAVE_DEFAULT_MINUTES 5
#define WP_AUTOSAVE_MAX_MINUTES     240
#define WP_AUTOSAVE_PAUSE_MS        2000uL

/* Short enough for the ST's 40-column alert line. */
#define WP_AUTOSAVE_FAILED_MSG "Autosave failed (disk full?)"

/* Look, and write the recovery file if it is due. Answers how many
   milliseconds until the next look is worth making; 0 when nothing is
   pending. img_src: the shell's picture source, as for wp_recovery_write
   (NULL where pictures are not kept). */
u32 wp_autosave_poll(struct wp_app_ctx *ctx, wp_image_source *img_src);

/* The recovery file now holds the document as it is -- written, or the
   document was recovered from it. recovery.c calls this; nothing else
   needs to. */
void wp_autosave_caught_up(struct wp_app_ctx *ctx);

#endif
