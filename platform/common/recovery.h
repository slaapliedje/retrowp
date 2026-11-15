/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WP_RECOVERY_H
#define WP_RECOVERY_H

/* Recovery: the unsaved document, kept when the system ends RetroWP.
 *
 * A multitasking AES (XaAES, MagiC, N.AES, gem4xe) asks a program to leave
 * with AP_TERM, at shutdown or from a task manager, and waits only so
 * long. Asking the writer "save your changes?" then is the wrong answer:
 * an alert blocks the program, the shutdown's timeout runs out, and the
 * AES calls it unresponsive and kills it, work and all. So RetroWP does
 * what modern programs do. It writes the document to one recovery file
 * without asking anything and leaves. The next time it starts it says so,
 * and the writer chooses: the recovered changes, or the file as last saved.
 *
 * ONE FILE, beside the configuration (the shell's recovery_path:
 * C:\RETROWP.REC on the ST). RetroWP holds one document, so one slot is
 * enough, and a fixed place works for an untitled document and for one
 * that came from a write-protected disk, where a copy beside it could not
 * go. It is a header -- what the document was called and how it was saved
 * -- and then the document as RTF, pictures included:
 *
 *     RetroWP recovery 1
 *     path=C:\LETTERS\MUM.TXT
 *     rtf=0
 *     <empty line>
 *     {\rtf1 ...
 *
 * WHEN IT GOES. The file belongs to the document it was written from, or
 * that was recovered from it (recovery_owned), and goes when that document
 * is settled: saved, or its changes discarded on the writer's word. A
 * recovery file this run did not write and has not offered is never
 * deleted by anything but the writer's answer to the offer.
 *
 * Nothing here asks the writer anything except wp_recovery_offer, and
 * nothing on the AP_TERM path (wp_action_term_request, wp_recovery_write)
 * may: a dialog there is the bug this exists to fix.
 *
 * A shell with recovery_path NULL has none of it, and every call here is a
 * no-op. */

#include "types.h"
#include "platform.h"
#include "format_rtf.h"     /* wp_image_source, wp_image_sink */

struct wp_app_ctx;

#define WP_RECOVERY_MAGIC "RetroWP recovery 1\n"

/* Write the document to ctx->recovery_path: quietly, never a dialog, and
   without marking it saved. On success the file is the document's
   (recovery_owned). */
wp_status wp_recovery_write(struct wp_app_ctx *ctx, wp_image_source *img_src);

/* Whether a recovery file is waiting: one that exists and has the header.
   The document's name goes to path_out ("" for an untitled one), and
   whether it was RTF to *rtf_out; either may be NULL. */
wp_bool wp_recovery_pending(const struct wp_app_ctx *ctx, char *path_out, u32 cap,
                            wp_bool *rtf_out);

/* Load the recovery file as the document: under its old name and format,
   marked modified (nothing has saved it), the file now the document's. */
wp_status wp_recovery_restore(struct wp_app_ctx *ctx, wp_image_sink *sink);

/* The writer said the recovered changes are not wanted: delete the file,
   whoever it belongs to. */
void wp_recovery_drop(struct wp_app_ctx *ctx);

/* The document has been settled -- saved, or its changes discarded on the
   writer's word: its recovery file, if it has one, is not needed now. */
void wp_recovery_settled(struct wp_app_ctx *ctx);

typedef enum {
    WP_RECOVERY_NONE = 0,     /* nothing was waiting */
    WP_RECOVERY_RECOVERED,    /* the document is the recovered one */
    WP_RECOVERY_USE_SAVED     /* the copy is gone; the saved file is wanted */
} wp_recovery_answer;

/* At start-up: if a recovery file is waiting, say so and ask -- recover
   the changes, or use the file as last saved (for an untitled document,
   discard them). The saved file's name, for the shell to open, goes to
   path_out. A dialog dismissed without an answer recovers: losing the
   work is the one outcome that cannot be undone. */
wp_recovery_answer wp_recovery_offer(struct wp_app_ctx *ctx, wp_image_sink *sink,
                                     char *path_out, u32 cap);

/* The system asks RetroWP to leave (the ST's AP_TERM). Never asks the
   writer anything. A document with no unsaved changes leaves at once; one
   with changes is written to the recovery file and leaves. WP_FALSE: the
   copy could not be written, so the program must stay, refuse the request
   (AP_TFAIL), and only then tell the writer why. */
wp_bool wp_action_term_request(struct wp_app_ctx *ctx, wp_image_source *img_src);

#endif
