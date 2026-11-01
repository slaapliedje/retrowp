/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "actions.h"
#include "app_ctx.h"
#include "format_txt.h"
#include "format_rtf.h"
#include "format_ws.h"
#include "format_md.h"
#include "offload.h"
#include "keymap.h"
#include "editing.h"

/* ------------------------------------------------------------------ */
/* Portable handlers                                                    */
/* ------------------------------------------------------------------ */

/* Format toggles do one of two things, decided by whether anything is
   selected right now.
 *
 * WITH A SELECTION they restyle it, through wp_edit_style_range -- one
 * undoable step however many runs it spans. Until that function existed
 * this branch could not be written at all: a character style belongs to
 * a run, so styling part of one means splitting it, which is engine
 * work and not something an action could assemble out of the public
 * primitives. Selecting a word and pressing Ctrl+B did nothing, in
 * every scheme, on every platform.
 *
 * WITHOUT ONE they toggle ctx->style_flags, the "current input style"
 * applied to newly-typed characters -- the original behaviour, and
 * still the right one when there is nothing to restyle. That is also
 * what every word processor these schemes imitate does.
 *
 * Which selection counts is wp_keymap_selection's problem: vi's Visual
 * pair, Emacs's mark and point, WordStar's ^KB/^KK markers,
 * WordPerfect's Block, Word's Shift+motion anchor. */

/* Sets `flag` across the selection if there is one and returns WP_TRUE;
   WP_FALSE means "nothing selected, toggle the input style instead".
   The direction is the usual one: a selection that is ALREADY entirely
   in this style turns it off, anything else turns it on -- so Ctrl+B on
   a half-bold selection makes all of it bold rather than unbolding the
   bold half. */
static wp_bool style_selection(wp_app_ctx *ctx, u16 flag)
{
    wp_selection sel;
    wp_style_delta delta;

    if (ctx->doc == NULL || ctx->journal == NULL) return WP_FALSE;
    if (!wp_keymap_selection(ctx, &sel)) return WP_FALSE;

    delta.size_twips = 0;
    if (wp_document_range_has_flags(ctx->doc, sel.anchor, sel.caret, flag)) {
        delta.flags_set = 0;
        delta.flags_clear = flag;
    } else {
        delta.flags_set = flag;
        delta.flags_clear = 0;
    }

    wp_edit_style_range(ctx->doc, ctx->journal, sel.anchor, sel.caret, &delta);
    return WP_TRUE;
}
/* "You have unsaved changes -- really discard them?"
 *
 * Returns WP_TRUE only when it is safe to throw the current document
 * away. Every caller that is about to replace or abandon the document
 * (New, Open, Quit) must gate on this.
 *
 * Three deliberate choices about which way it fails:
 *   - A clean document returns WP_TRUE immediately, with no dialog. A
 *     prompt on every Quit trains people to dismiss it unread, which is
 *     how a warning stops working.
 *   - "Cancel" is the DEFAULT selection, not "Discard". This dialog
 *     appears in front of someone who did not expect it; the keystroke
 *     they fire off reflexively must be the one that keeps their work.
 *   - If plat_dialog_choice itself fails, the answer is WP_FALSE --
 *     refuse to discard. A backend that cannot ask must not assume
 *     permission.
 *
 * Lives here rather than per-backend so all four ask the same question
 * with the same default; a backend only has to remember to call it. */
wp_bool wp_action_confirm_discard(wp_app_ctx *ctx)
{
    static const char *const k_opts[] = { "Discard changes", "Cancel" };
    static const char k_msg[] = "Unsaved changes will be lost.";
    const u8 *labels[2];
    u32 lens[2];
    wp_bool cancelled = WP_FALSE;
    u32 chosen = 1;
    u32 i;

    if (ctx == NULL || ctx->doc == NULL) return WP_TRUE;
    if (!ctx->doc_loaded) return WP_TRUE;
    if (!wp_document_is_modified(ctx->doc)) return WP_TRUE;

    for (i = 0; i < 2u; i++) {
        labels[i] = (const u8 *)k_opts[i];
        lens[i] = (u32)strlen(k_opts[i]);
    }

    if (plat_dialog_choice(ctx->window, (const u8 *)k_msg,
                            (u32)(sizeof(k_msg) - 1),
                            labels, lens, 2u, 1u,
                            &cancelled, &chosen) != WP_OK) {
        return WP_FALSE;
    }
    if (cancelled) return WP_FALSE;
    return (chosen == 0u) ? WP_TRUE : WP_FALSE;
}

/* File > New: replace the document with a single empty paragraph.
 *
 * Portable rather than per-backend, so all four get it from one place --
 * everything it touches (document, undo journal, keymap cursor, the
 * current-style state) lives in wp_app_ctx already. It mirrors what the
 * Atari backend's own replace_document() does on Open, minus the parts
 * that are genuinely native (the file picker, the relayout, which every
 * backend already runs before its next paint because edit.c marks
 * paragraphs dirty).
 *
 * The fresh document is built and fully checked BEFORE anything existing
 * is freed. That ordering is the whole point: if an allocation fails
 * midway, the writer still has the document they were working on. A New
 * that destroys the current text and THEN discovers it cannot allocate
 * a replacement would be the worst possible failure for a word
 * processor, and on a 1MB ST an allocation failure is a real
 * possibility, not a theoretical one. */
static wp_status act_file_new(wp_app_ctx *ctx)
{
    wp_document fresh;
    u32 sec_idx, para_idx;
    wp_section *sec;

    if (ctx == NULL || ctx->doc == NULL || ctx->journal == NULL) return WP_ERR;
    if (!wp_action_confirm_discard(ctx)) return WP_OK;   /* the writer said no */

    if (wp_document_init(&fresh) != WP_OK) return WP_NOMEM;
    if (wp_document_section_add(&fresh, &sec_idx) != WP_OK) {
        wp_document_free(&fresh);
        return WP_NOMEM;
    }
    sec = wp_document_section_get(&fresh, sec_idx);
    if (sec == NULL || wp_section_paragraph_add(sec, &para_idx) != WP_OK) {
        wp_document_free(&fresh);
        return WP_NOMEM;
    }

    /* Past this point nothing can fail, so the swap is safe to commit. */
    wp_action_replace_document(ctx, &fresh);
    return WP_OK;
}

/* One swap for every path that loads a whole document -- New, Open,
   startup -- on every backend. It used to be four hand-written copies,
   and two of them (Amiga, Mac) never reset the undo journal: an Undo
   after Open replayed the previous document's edits against memory that
   had just been freed. One copy cannot drift like that. */
void wp_action_replace_document(wp_app_ctx *ctx, wp_document *fresh)
{
    const wp_char_style *def_cs;
    const wp_para_style *def_ps;

    if (ctx->doc_loaded) {
        wp_undo_journal_free(ctx->journal);
        wp_document_free(ctx->doc);
    }
    *ctx->doc = *fresh;
    wp_undo_journal_init(ctx->journal);

    /* The active keymap scheme was holding a cursor, registers and
       possibly pending operator state for a document that no longer
       exists -- reset it rather than resync it. wp_app_ctx_init already
       ran wp_keymap_init once, so there is always something valid to
       free here, even on the very first load. */
    wp_keymap_free(ctx);
    wp_keymap_init(ctx, (wp_keymap_id)ctx->keymap_id, ctx->doc, ctx->journal);
    /* A new document starts at its top, however far down the old one
       was scrolled, and has no file until a load or a save gives it
       one -- so a Save after New asks for a name rather than writing
       over the file the previous document came from. */
    wp_viewport_reset(&ctx->view);
    ctx->doc_path[0] = '\0';

    def_cs = wp_document_cstyle_get(ctx->doc, ctx->doc->default_cstyle);
    if (def_cs != NULL) {
        ctx->style_flags = def_cs->flags;
        ctx->current_size_twips = def_cs->size_twips;
    }
    def_ps = wp_document_pstyle_get(ctx->doc, ctx->doc->default_pstyle);
    if (def_ps != NULL) ctx->align = def_ps->align;

    ctx->doc_loaded = WP_TRUE;
    /* The document now matches what it was built from -- a file, or
       nothing at all for New -- so there is no unsaved work in it. */
    wp_document_mark_saved(ctx->doc);
}

static wp_status act_format_bold(wp_app_ctx *ctx)
{
    if (style_selection(ctx, WP_STYLE_BOLD)) return WP_OK;
    ctx->style_flags ^= WP_STYLE_BOLD;
    return WP_OK;
}

static wp_status act_format_italic(wp_app_ctx *ctx)
{
    if (style_selection(ctx, WP_STYLE_ITALIC)) return WP_OK;
    ctx->style_flags ^= WP_STYLE_ITALIC;
    return WP_OK;
}

static wp_status act_format_underline(wp_app_ctx *ctx)
{
    if (style_selection(ctx, WP_STYLE_UNDERLINE)) return WP_OK;
    ctx->style_flags ^= WP_STYLE_UNDERLINE;
    return WP_OK;
}

static wp_status act_format_size(wp_app_ctx *ctx)
{
    static const char *const labels[] = { "8 pt", "10 pt", "12 pt", "14 pt", "18 pt", "24 pt", "36 pt" };
    static const i32 sizes[] = { 160, 200, 240, 280, 360, 480, 720 };
    const u32 count = (u32)(sizeof(sizes) / sizeof(sizes[0]));
    const u8 *label_ptrs[7];
    u32 label_lens[7];
    u32 default_index = 2; /* 12pt, matches the document default (see docmodel.c) */
    u32 chosen;
    wp_bool cancelled;
    u32 i;
    wp_status st;

    for (i = 0; i < count; i++) {
        label_ptrs[i] = (const u8 *)labels[i];
        label_lens[i] = (u32)strlen(labels[i]);
        if (sizes[i] == ctx->current_size_twips) default_index = i;
    }

    st = plat_dialog_choice(ctx->window, (const u8 *)"Font Size", 9,
                             label_ptrs, label_lens, count, default_index,
                             &cancelled, &chosen);
    if (st != WP_OK) return st;
    if (!cancelled) ctx->current_size_twips = sizes[chosen];
    return WP_OK;
}

static wp_status act_format_align(wp_app_ctx *ctx)
{
    static const char *const labels[] = { "Left", "Center", "Right" };
    static const u8 aligns[] = { WP_ALIGN_LEFT, WP_ALIGN_CENTER, WP_ALIGN_RIGHT };
    const u32 count = (u32)(sizeof(aligns) / sizeof(aligns[0]));
    const u8 *label_ptrs[3];
    u32 label_lens[3];
    u32 default_index = 0;
    u32 chosen;
    wp_bool cancelled;
    u32 i;
    wp_status st;

    for (i = 0; i < count; i++) {
        label_ptrs[i] = (const u8 *)labels[i];
        label_lens[i] = (u32)strlen(labels[i]);
        if (aligns[i] == ctx->align) default_index = i;
    }

    st = plat_dialog_choice(ctx->window, (const u8 *)"Alignment", 9,
                             label_ptrs, label_lens, count, default_index,
                             &cancelled, &chosen);
    if (st != WP_OK) return st;
    if (!cancelled) {
        wp_section *sec;
        wp_paragraph *para;
        const wp_para_style *cur;
        wp_para_style style;
        wp_pstyle handle;

        ctx->align = aligns[chosen];

        if (!ctx->doc_loaded) return WP_OK;
        sec = wp_document_section_get(ctx->doc, wp_keymap_cursor(ctx)->section);
        if (sec == NULL) return WP_OK;
        para = wp_section_paragraph_get(sec, wp_keymap_cursor(ctx)->paragraph);
        if (para == NULL) return WP_OK;
        cur = wp_document_pstyle_get(ctx->doc, para->style);
        if (cur == NULL) return WP_OK;
        style = *cur;
        style.align = ctx->align;
        /* Not journaled, so it marks the document modified itself -- but
           only for a real change, or picking the alignment it already has
           would make Quit ask about work that does not exist. */
        if (wp_document_pstyle_find_or_add(ctx->doc, &style, &handle) == WP_OK &&
            handle != para->style) {
            wp_paragraph_pstyle_set(para, handle);
            wp_document_mark_modified(ctx->doc);
        }
    }
    return WP_OK;
}

/* Edit > Copy / Cut / Paste.
 *
 * Copy and Cut act on the selection -- the active scheme's own notion
 * of one, through wp_keymap_selection -- and Paste inserts at the
 * cursor, replacing the selection if there is one. The clipboard carries
 * plain text; a paragraph break travels as '\n'.
 *
 * All three are portable now. Cut and Paste used to be four per-backend
 * copies that replaced the whole document, and between them they could
 * lose it: Cut emptied the document whether or not the copy had worked,
 * Paste threw the document away for whatever the clipboard held, and
 * both then marked the result saved, so quitting afterwards did not
 * warn either. Here both are ordinary journaled edits -- one undo step
 * each, and they mark the document modified like any other edit.
 * edit.c marks the touched paragraphs dirty, and every backend
 * relayouts dirty paragraphs before its next paint. */

/* Extracts the selection as plain text (one '\n' per paragraph break,
   none after the last paragraph -- wp_document_extract_text's own
   convention) and puts it on the clipboard. On success *out_text, when
   non-NULL, receives the bytes for the caller to mem_free. */
static wp_status copy_selection(wp_app_ctx *ctx, wp_selection sel, u8 **out_text, u32 *out_len)
{
    u32 needed = 0;
    u8 *buf;
    wp_status st;

    (void)wp_document_extract_text(ctx->doc, sel, NULL, 0, &needed);
    buf = (u8 *)mem_alloc(needed > 0 ? needed : 1);
    if (buf == NULL) return WP_NOMEM;

    st = wp_document_extract_text(ctx->doc, sel, buf, needed, &needed);
    if (st == WP_OK) st = plat_clipboard_set_text(buf, needed);
    if (st == WP_OK && out_text != NULL) {
        *out_text = buf;
        *out_len = needed;
    } else {
        mem_free(buf);
    }
    return st;
}

/* Cut and Copy act on the selection, whatever the active scheme calls
   one (wp_keymap_selection), and with none they say so rather than
   guess: they used to take the whole document, which is not what any
   writer pressing Ctrl+X means. */
static wp_bool need_selection(wp_app_ctx *ctx, wp_selection *sel)
{
    if (wp_keymap_selection(ctx, sel)) return WP_TRUE;
    wp_action_tell(ctx, "Nothing is selected");
    return WP_FALSE;
}

/* Does the clipboard hold exactly text[0..len)? A backend may keep less
   than it was handed and still report success: Atari, Amiga and Mac
   convert through a 1 KB buffer and keep what fits, and a character the
   native charset cannot hold comes back as '?'. Copy can live with that.
   Cut cannot, because it is about to delete the original -- so it reads
   the clipboard back rather than trusting the status. The buffer is one
   byte longer than the text so a clipboard holding MORE shows up too. */
static wp_bool clipboard_holds(const u8 *text, u32 len)
{
    u8 *back;
    u32 back_len = 0;
    wp_bool same;

    back = (u8 *)mem_alloc(len + 1);
    if (back == NULL) return WP_FALSE;
    same = (wp_bool)(plat_clipboard_get_text(back, len + 1, &back_len) == WP_OK &&
                     back_len == len && memcmp(back, text, len) == 0);
    mem_free(back);
    return same;
}

static wp_status act_edit_copy(wp_app_ctx *ctx)
{
    wp_selection sel;
    wp_status st;

    if (ctx == NULL || ctx->doc == NULL) return WP_ERR;
    if (!ctx->doc_loaded) return WP_OK;
    if (!need_selection(ctx, &sel)) return WP_OK;

    /* The selection stays: the writer may want to style it next. */
    st = copy_selection(ctx, sel, NULL, NULL);
    if (st != WP_OK) wp_action_tell(ctx, "Could not copy to the clipboard");
    return st;
}

static wp_status act_edit_cut(wp_app_ctx *ctx)
{
    u8 *text = NULL;
    u32 len = 0;
    wp_selection sel;
    wp_pos at;
    wp_status st;

    if (ctx == NULL || ctx->doc == NULL || ctx->journal == NULL) return WP_ERR;
    if (!ctx->doc_loaded) return WP_OK;
    if (!need_selection(ctx, &sel)) return WP_OK;

    st = copy_selection(ctx, sel, &text, &len);
    if (st == WP_OK && !clipboard_holds(text, len)) st = WP_RANGE;
    if (text != NULL) mem_free(text);
    if (st != WP_OK) {
        wp_action_tell(ctx, "Clipboard can't hold it all; nothing was cut");
        return st;
    }

    st = wp_edit_delete_selection(ctx->doc, ctx->journal, sel, &at);
    if (st == WP_OK) {
        *wp_keymap_cursor(ctx) = at;
        wp_keymap_clear_selection(ctx);
    }
    return st;
}

/* Static, not automatic: 4 KB is an impossible stack frame on two of
   the targets -- an Amiga program started from the Shell gets a 4 KB
   stack in total, and gem4xe gives an application 2 KB of bank $00 for
   its stack and near data together. Nothing here recurses, and the
   buffer holds nothing between calls. 4 KB is more than any backend's
   clipboard returns. */
static u8 g_paste_buf[4096];

static wp_status act_edit_paste(wp_app_ctx *ctx)
{
    u32 len = 0, i, n;
    wp_pos *cursor;
    wp_pos at;
    wp_selection sel;
    wp_status st;

    if (ctx == NULL || ctx->doc == NULL || ctx->journal == NULL) return WP_ERR;
    if (!ctx->doc_loaded) return WP_OK;
    if (plat_clipboard_get_text(g_paste_buf, (u32)sizeof(g_paste_buf), &len) != WP_OK) return WP_OK;

    /* Line endings to '\n', the only paragraph break wp_edit_paste_text
       knows: CRLF from DOS, a bare CR from a Mac. A final '\n' is kept:
       Copy puts one there exactly when a paragraph break was selected
       (Visual Line, a block spanning lines), and that break is part of
       what the writer is moving. */
    n = 0;
    for (i = 0; i < len; i++) {
        if (g_paste_buf[i] == '\r') {
            if (i + 1 < len && g_paste_buf[i + 1] == '\n') continue;
            g_paste_buf[n++] = '\n';
        } else {
            g_paste_buf[n++] = g_paste_buf[i];
        }
    }
    if (n == 0) return WP_OK;

    /* Over a selection, Paste replaces it: one undo step for both the
       removal and the insertion, like every editor these schemes copy. */
    cursor = wp_keymap_cursor(ctx);
    wp_undo_group_begin(ctx->journal);
    if (wp_keymap_selection(ctx, &sel)) {
        st = wp_edit_delete_selection(ctx->doc, ctx->journal, sel, &at);
        if (st != WP_OK) {
            wp_undo_group_end(ctx->journal);
            return st;
        }
        *cursor = at;
        wp_keymap_clear_selection(ctx);
    }
    st = wp_edit_paste_text(ctx->doc, ctx->journal, *cursor, g_paste_buf, n, &at);
    /* wp_edit_paste_text splits only BETWEEN lines, never on a final
       '\n' (vi's charwise put wants that). Here a final '\n' is a
       paragraph break the writer selected -- Visual Line, a block that
       ended at a line's end -- so the pasted text ends its paragraph
       and what followed the caret starts a new one, which is what a
       cut-and-paste of whole lines has to give back. */
    if (st == WP_OK && g_paste_buf[n - 1] == '\n') {
        wp_pos after;

        st = wp_edit_split_paragraph(ctx->doc, ctx->journal, at, &after);
        if (st == WP_OK) at = after;
    }
    wp_undo_group_end(ctx->journal);
    *cursor = at;   /* set even on failure: it is where the paste got to */
    return st;
}

/* Edit > Undo.
 *
 * Every scheme already has its own undo (vi's u, Word's Ctrl-Z, and so
 * on) and keeps its own cursor, so this is not the only route -- it is
 * the route for people who are not in a scheme's own idiom: a menu entry
 * on the backends that have menus, and the Atari ST's dedicated UNDO
 * key, which this project ignored completely until now even though the
 * Compendium lists it among the standard UI conventions (p.703, "UNDO --
 * Undo last operation").
 *
 * The clamp afterwards is not optional. Undo can shorten the document
 * under a cursor that was past the new end, and every scheme's own undo
 * path already clamps for exactly this reason (vimode.c's clamp_cursor).
 * wp_pos_clamp_to_document is the same primitive those use. */
static wp_status act_edit_undo(wp_app_ctx *ctx)
{
    wp_status st;

    if (ctx == NULL || ctx->doc == NULL || ctx->journal == NULL) return WP_ERR;

    st = wp_edit_undo(ctx->doc, ctx->journal);
    if (st != WP_OK) return st;   /* nothing to undo -- not an error worth reporting */

    *wp_keymap_cursor(ctx) = wp_pos_clamp_to_document(ctx->doc, *wp_keymap_cursor(ctx));
    return WP_OK;
}

static wp_status act_file_quit(wp_app_ctx *ctx)
{
    /* Quitting throws the document away just as surely as New does, so
       it asks the same question. Returning WP_OK on a refusal is
       deliberate: the writer answered, the action did what they asked
       (nothing), and reporting an error would make a backend show a
       failure message on top of a dialog they just dismissed. */
    if (!wp_action_confirm_discard(ctx)) return WP_OK;
    ctx->quit_requested = WP_TRUE;
    return WP_OK;
}

/* Every other way out -- a scheme's own quit command (vi :q, WordStar
   ^KQ, Emacs C-x C-c, ...), a window's close box -- used to leave at
   once, without the question File > Quit asks. They come here instead. */
void wp_action_request_quit(wp_app_ctx *ctx)
{
    (void)act_file_quit(ctx);
}

/* vi :wq and WordStar ^KX: the backend has just run its own Save. Leave
   only if that save really landed. A cancelled picker or a failed write
   both leave the document modified, and quitting then would throw away
   exactly the work the writer asked to keep. */
void wp_action_request_quit_after_save(wp_app_ctx *ctx)
{
    if (ctx->doc_loaded && wp_document_is_modified(ctx->doc)) return;
    ctx->quit_requested = WP_TRUE;
}

/* The tail end of every backend's Save. plat_file_open for writing
   truncates, so a write that fails halfway has already cost the old
   file; the least this can do is say so, and leave the document marked
   modified so nothing later assumes it is safe on disk. */
wp_status wp_action_write_file(wp_app_ctx *ctx, const char *native_path,
                                const u8 *buf, u32 len, wp_bool marks_saved)
{
    plat_file *f;
    wp_status st;

    st = plat_file_open(native_path, PLAT_FILE_WRITE, &f);
    if (st == WP_OK) {
        st = plat_file_write(f, buf, len);
        plat_file_close(f);
    }
    if (st != WP_OK) {
        wp_action_tell(ctx, "Couldn't save that file");
        return st;
    }
    if (marks_saved) wp_document_mark_saved(ctx->doc);
    return WP_OK;
}

/* ------------------------------------------------------------------ */
/* The registry                                                         */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Dictation (T174)                                                     */
/* ------------------------------------------------------------------ */

#define WP_DICTATE_MAX_DURATION_S     15u
#define WP_DICTATE_FLAG_TRIM_SILENCE  0x01u
/* Generous on purpose: this one call covers the writer speaking (up to
   max_duration_s) AND the gateway transcribing afterwards. Measured on
   the real Pi 5, ggml-base.en runs at ~4x realtime with roughly 1.5s of
   model load per request, so 15s of speech lands well inside this --
   but a cold, busy or slower gateway must not look like a hang. */
#define WP_DICTATE_TIMEOUT_MS         120000uL

/* A one-button plat_dialog_choice, used as a message box. There is no
   plat_dialog_message in platform.h (see its own comment: no free-text
   variant exists because nothing needed one), and adding one just for
   this would be a new required primitive on all four backends. A single
   "OK" option is the honest way to say something with what already
   exists everywhere. */
void wp_action_tell(wp_app_ctx *ctx, const char *title)
{
    const u8 *labels[1];
    u32 lens[1];
    wp_bool cancelled;
    u32 chosen;

    labels[0] = (const u8 *)"OK";
    lens[0] = 2;
    (void)plat_dialog_choice(ctx->window, (const u8 *)title, (u32)strlen(title),
                              labels, lens, 1, 0, &cancelled, &chosen);
}

wp_status wp_action_dictate(wp_app_ctx *ctx)
{
    static const char *const k_go[] = { "Speak now", "Cancel" };
    const u8 *labels[2];
    u32 lens[2];
    offload_capabilities caps;
    offload_request req;
    offload_response resp;
    u8 payload[4];
    wp_pos *cursor;
    wp_pos new_pos;
    wp_bool cancelled;
    u32 chosen;
    u32 i;
    wp_status st;

    if (ctx == NULL || !ctx->doc_loaded) return WP_ERR;

    if (!offload_available()) {
        wp_action_tell(ctx, "Dictate: no offload device connected");
        return WP_ERR;
    }

    /* CAPS pre-check BEFORE building a request, not submit-and-hope --
       T8.3 CHAT's discipline. A gateway with no microphone or no model
       clears the DICTATE bit, so this answers immediately instead of
       recording into a void for 15 seconds first. */
    if (offload_query(&caps) != WP_OK ||
        (caps.ops_supported & (1uL << (u32)OFFLOAD_DICTATE)) == 0) {
        wp_action_tell(ctx, "This gateway doesn't offer Dictate");
        return WP_UNSUPPORTED;
    }

    /* The writer needs to know WHEN to start talking, and the request
       blocks for as long as it records. Dismissing this dialog is that
       signal. Leading silence costs nothing -- TRIM_SILENCE only ends
       the recording on silence that FOLLOWS detected speech -- so the
       round-trip delay between dismissing and the microphone opening is
       harmless, which is what makes this simple approach honest rather
       than merely convenient. */
    for (i = 0; i < 2; i++) {
        labels[i] = (const u8 *)k_go[i];
        lens[i] = (u32)strlen(k_go[i]);
    }
    st = plat_dialog_choice(ctx->window,
                             (const u8 *)"Dictate: speak after OK",
                             (u32)strlen("Dictate: speak after OK"),
                             labels, lens, 2, 0, &cancelled, &chosen);
    if (st != WP_OK) return st;
    if (cancelled || chosen != 0) return WP_OK;

    payload[0] = (u8)WP_DICTATE_MAX_DURATION_S;
    payload[1] = (u8)WP_DICTATE_FLAG_TRIM_SILENCE;
    payload[2] = (u8)'e';
    payload[3] = (u8)'n';

    req.op = OFFLOAD_DICTATE;
    req.payload = payload;
    req.payload_len = 4u;
    req.timeout_ms = (u32)WP_DICTATE_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        wp_action_tell(ctx, "Dictation failed or timed out");
        return WP_ERR;
    }
    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        wp_action_tell(ctx, "The gateway couldn't transcribe that");
        return WP_ERR;
    }
    /* Empty payload with OFFLOAD_OK is "listened, heard nothing" -- a
       normal outcome, NOT an error (docs/formats/offload-protocol.md).
       Saying so plainly matters: silence is the likeliest result of a
       mistimed start, and reporting it as a failure would teach the
       writer to distrust a feature that worked correctly. */
    if (resp.result_len == 0) {
        offload_response_free(&resp);
        wp_action_tell(ctx, "Heard nothing -- nothing inserted");
        return WP_OK;
    }

    /* Dictated text is just text: no new engine concept, no new content
       type, the same call a typed character takes. edit.c marks the
       paragraph layout_dirty, and every backend relayouts dirty
       paragraphs before its next paint, so nothing here needs to reach
       for a per-backend relayout it could not call portably anyway. */
    cursor = wp_keymap_cursor(ctx);
    st = wp_edit_insert_text(ctx->doc, ctx->journal, *cursor,
                              resp.result, resp.result_len, &new_pos);
    if (st == WP_OK) *cursor = new_pos;
    offload_response_free(&resp);

    if (st != WP_OK) wp_action_tell(ctx, "Couldn't insert the dictated text");
    return st;
}

const wp_action g_wp_actions[WP_ACT_COUNT] = {
    { WP_ACT_FILE_NEW,        WP_MENU_FILE,   "New",            "Ctrl+N", 0x0E, act_file_new, NULL },
    { WP_ACT_FILE_OPEN,         WP_MENU_FILE,   "Open",           "Ctrl+O", 0x0F, NULL, NULL },
    { WP_ACT_FILE_SAVE,         WP_MENU_FILE,   "Save",           "Ctrl+S", 0x13, NULL, NULL },
    { WP_ACT_FILE_SAVE_AS,      WP_MENU_FILE,   "Save As...",     "",       0,    NULL, NULL },
    { WP_ACT_FILE_QUIT,         WP_MENU_FILE,   "Quit",           "Ctrl+Q", 0x11, act_file_quit, NULL },
    { WP_ACT_EDIT_CUT,          WP_MENU_EDIT,   "Cut",            "Ctrl+X", 0x18, act_edit_cut, NULL },
    { WP_ACT_EDIT_COPY,         WP_MENU_EDIT,   "Copy",           "Ctrl+C", 0x03, act_edit_copy, NULL },
    { WP_ACT_EDIT_PASTE,        WP_MENU_EDIT,   "Paste",          "Ctrl+V", 0x16, act_edit_paste, NULL },
    { WP_ACT_EDIT_UNDO,       WP_MENU_EDIT,   "Undo",           0,        0,    act_edit_undo, NULL },
    { WP_ACT_FORMAT_BOLD,       WP_MENU_FORMAT, "Bold",           "Ctrl+B", 0x02, act_format_bold, NULL },
    { WP_ACT_FORMAT_ITALIC,     WP_MENU_FORMAT, "Italic",         "Ctrl+I", 0x09, act_format_italic, NULL },
    { WP_ACT_FORMAT_UNDERLINE,  WP_MENU_FORMAT, "Underline",      "Ctrl+U", 0x15, act_format_underline, NULL },
    { WP_ACT_FORMAT_SIZE,       WP_MENU_FORMAT, "Size...",        0,        0x1D, act_format_size, NULL },
    { WP_ACT_FORMAT_ALIGN,      WP_MENU_FORMAT, "Alignment...",   0,        0x0C, act_format_align, NULL },
    { WP_ACT_TOOLS_SPELLCHECK,  WP_MENU_TOOLS,  "Spell Check",    0,        0,    NULL, NULL },
    { WP_ACT_TOOLS_THESAURUS,   WP_MENU_TOOLS,  "Thesaurus",      0,        0,    NULL, NULL },
    { WP_ACT_TOOLS_GRAMMARCHECK, WP_MENU_TOOLS, "Grammar Check",  0,        0,    NULL, NULL },
    { WP_ACT_TOOLS_CHAT,        WP_MENU_TOOLS,  "Chat",           0,        0,    NULL, NULL },
    /* M15: a real Ctrl+G shortcut, not menu-only like its Tools
       siblings above -- deliberately, so this action never needs the
       Atari OBJECT tree touched at all (see docs/atari-platform.md's
       own account of two real regressions from exactly that class of
       change) and so DOS -- which has no menu system whatsoever, only
       four shortcut-bound backend actions wired today -- can reach it
       at all. The same shortcut also works on Amiga/Mac, where it's
       redundant with (but harmless alongside) their own dynamically-
       built Tools menu already showing this entry automatically. */
    { WP_ACT_TOOLS_INSERTIMAGE, WP_MENU_TOOLS,  "Insert Image",   "Ctrl+G", 0x07, NULL, NULL },
    /* T174: Ctrl+D, for the same reasons Insert Image took a real
       shortcut above -- no Atari OBJECT tree edit needed, and DOS can
       reach it at all. 0x04 was free across the whole table, and
       vi-mode claims NO control codes whatsoever (see docs/vimode.md:
       every plain-letter vi command requires an UNMODIFIED key
       precisely so the Ctrl space stays available to app actions), so
       this is reachable from the default keymap rather than menu-only.

       Dictation earning a keystroke rather than living in a menu is the
       whole point of the feature: the writer speaks a sentence without
       moving a hand to the mouse. A Tools menu entry reached by mouse
       would defeat the ergonomics it exists to serve. */
    { WP_ACT_TOOLS_DICTATE,     WP_MENU_TOOLS,  "Dictate",        "Ctrl+D", 0x04, NULL, NULL },
    { WP_ACT_HELP_CONTENTS,     WP_MENU_HELP,   "Contents",       "F1",     0,    NULL, NULL },
    /* Two more entry points into the SAME help document, not new content:
       docs/help/retrowp.help already carries a node per topic, and the
       viewer already navigates between them. Reaching "how do I move the
       cursor" took F1 then a link-follow; on a 1MB ST driving a pager
       with the keyboard, one menu pick instead of two steps is a real
       difference. No shortcut byte on either -- F1 remains the single
       keyboard door into help, and every free Ctrl-letter is worth more
       to an editing command than to a second way into a menu that is
       already one click away. See wp_action_help_node for the mapping. */
    { WP_ACT_HELP_KEYS,         WP_MENU_HELP,   "Editing Keys",   0,        0,    NULL, NULL },
    { WP_ACT_HELP_FORMAT,       WP_MENU_HELP,   "Format Keys",    0,        0,    NULL, NULL }
};

const char *wp_action_help_node(wp_action_id id, u32 *len)
{
    const char *name;

    switch (id) {
    case WP_ACT_HELP_CONTENTS: name = "Main";   break;
    case WP_ACT_HELP_KEYS:     name = "Normal"; break;
    case WP_ACT_HELP_FORMAT:   name = "Format"; break;
    default:                   return NULL;
    }

    if (len != NULL) {
        u32 n = 0;
        while (name[n] != '\0') n++;
        *len = n;
    }
    return name;
}

const wp_action *wp_action_find(wp_action_id id)
{
    if ((u32)id >= (u32)WP_ACT_COUNT) return NULL;
    return &g_wp_actions[id];
}

const wp_action *wp_action_find_by_shortcut(u8 ascii)
{
    u32 i;

    if (ascii == 0) return NULL;
    for (i = 0; i < (u32)WP_ACT_COUNT; i++) {
        if (g_wp_actions[i].shortcut_ascii == ascii) return &g_wp_actions[i];
    }
    return NULL;
}

/* ---- Files (see actions.h) ------------------------------------------ */

wp_bool wp_path_ends_with_ci(const char *path, const char *suffix)
{
    u32 plen = (u32)strlen(path);
    u32 slen = (u32)strlen(suffix);
    u32 i;

    if (plen < slen) return WP_FALSE;
    for (i = 0; i < slen; i++) {
        char a = path[plen - slen + i];
        char b = suffix[i];

        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return WP_FALSE;
    }
    return WP_TRUE;
}

void wp_action_remember_path(wp_app_ctx *ctx, const char *native_path)
{
    u32 n = (u32)strlen(native_path);

    /* A path too long to keep is not kept: the next Save asks again,
       which beats saving to a truncated name. */
    if (n >= (u32)WP_APP_PATH_MAX) {
        ctx->doc_path[0] = '\0';
        return;
    }
    memcpy(ctx->doc_path, native_path, n + 1);
}

const char *wp_action_document_name(const wp_app_ctx *ctx)
{
    const char *p = ctx->doc_path;
    const char *name = p;

    /* Every target's separator: '\\' on the ST and DOS, '/' and ':' on
       AmigaDOS (SYS:Docs/a.txt), ':' on the Mac (Volume:a.txt). */
    for (; *p != '\0'; p++) {
        if (*p == '\\' || *p == '/' || *p == ':') name = p + 1;
    }
    return name;
}

/* ---- ODT and PDF, through the gateway --------------------------------
   OFFLOAD_CONVERT_IMPORT/EXPORT ship a WHOLE file as one request and one
   response (unlike Thesaurus's single words), so they get their own
   bounds. Real RS-232 at 9600 baud moves about 960 bytes a second (8N1
   is 10 bits a byte), so a few KB of document genuinely takes tens of
   seconds each way: the timeout is sized for that line, not for a fast
   local one. The response side is bounded regardless by offload_client.c's
   OFFLOAD_MAX_RESPONSE_PAYLOAD. RTF is the interchange both ways (the
   gateway speaks it; docs/formats/offload-protocol.md), without pictures:
   the gateway's converter does not carry them, and the payload cap is
   better spent on text. */
#define WP_ACTION_CONVERT_MAX_FILE   60000uL   /* under WIRE_MAX_PAYLOAD with room for the id byte and framing */
#define WP_ACTION_CONVERT_TIMEOUT_MS 120000uL  /* about two minutes; see above */

/* The link, brought up the backend's way if it gave us one, else as it
   stands. Tells the writer when there is none. */
static wp_bool link_for_convert(wp_app_ctx *ctx)
{
    wp_bool up = (ctx->ensure_offload != NULL) ? ctx->ensure_offload() : offload_available();

    if (!up) wp_action_tell(ctx, "ODT/PDF needs a connected gateway");
    return up;
}

/* payload[0] is the format id, the file follows. The RTF that comes back
   is read like a local .RTF, pictures through `sink` when there is one. */
static wp_status convert_import(wp_app_ctx *ctx, u8 *payload, u32 payload_len,
                                wp_document *fresh, wp_image_sink *sink)
{
    offload_request req;
    offload_response resp;
    wp_status st;

    req.op = OFFLOAD_CONVERT_IMPORT;
    req.payload = payload;
    req.payload_len = payload_len;
    req.timeout_ms = (u32)WP_ACTION_CONVERT_TIMEOUT_MS;

    if (offload_submit(&req, &resp) != WP_OK) {
        wp_action_tell(ctx, "Opening that file failed or timed out");
        return WP_ERR;
    }
    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        wp_action_tell(ctx, "The gateway couldn't convert that file");
        return WP_ERR;
    }
    st = (sink != NULL) ? format_rtf_import_images(resp.result, resp.result_len, fresh, sink)
                        : format_rtf_import(resp.result, resp.result_len, fresh);
    offload_response_free(&resp);
    if (st != WP_OK) wp_action_tell(ctx, "The gateway's reply couldn't be read as a document");
    return st;
}

/* The document as RTF behind a format id, to the gateway; what comes
   back is the file, written as is. ODT reads back in, so it is a save
   and the name sticks; PDF does not, so it is an export, the document
   stays modified and keeps its own name. */
static wp_status convert_export(wp_app_ctx *ctx, const char *native_path, u8 format_id)
{
    u32 rtf_len = 0;
    u8 *payload;
    offload_request req;
    offload_response resp;
    wp_status st;

    format_rtf_export(ctx->doc, NULL, 0, &rtf_len);
    if (rtf_len + 1 > WP_ACTION_CONVERT_MAX_FILE) {
        wp_action_tell(ctx, "The document is too large to send to the gateway");
        return WP_RANGE;
    }
    payload = (u8 *)mem_alloc(rtf_len + 1);
    if (payload == NULL) {
        wp_action_tell(ctx, "Not enough memory to save");
        return WP_NOMEM;
    }
    payload[0] = format_id;
    if (format_rtf_export(ctx->doc, payload + 1, rtf_len, &rtf_len) != WP_OK) {
        mem_free(payload);
        wp_action_tell(ctx, "Couldn't build the file to save");
        return WP_ERR;
    }

    req.op = OFFLOAD_CONVERT_EXPORT;
    req.payload = payload;
    req.payload_len = rtf_len + 1;
    req.timeout_ms = (u32)WP_ACTION_CONVERT_TIMEOUT_MS;

    st = offload_submit(&req, &resp);
    mem_free(payload);
    if (st != WP_OK) {
        wp_action_tell(ctx, "Saving that file failed or timed out");
        return WP_ERR;
    }
    if (resp.status != OFFLOAD_OK) {
        offload_response_free(&resp);
        wp_action_tell(ctx, "The gateway couldn't produce that file format");
        return WP_ERR;
    }
    st = wp_action_write_file(ctx, native_path, resp.result, resp.result_len,
                              (wp_bool)(format_id == (u8)OFFLOAD_FORMAT_ODT));
    offload_response_free(&resp);
    if (st == WP_OK && format_id == (u8)OFFLOAD_FORMAT_ODT) wp_action_remember_path(ctx, native_path);
    return st;
}

wp_status wp_action_load_file(wp_app_ctx *ctx, const char *native_path, wp_image_sink *sink)
{
    plat_file *f;
    u32 size = 0, read_len = 0;
    u8 *buf, *data;
    wp_bool via_gateway = wp_path_ends_with_ci(native_path, ".ODT");
    wp_document fresh;
    wp_status st;

    /* The link first, so a writer without one is told before any
       waiting; the file is read straight into the request, behind its
       format id, rather than copied there. */
    if (via_gateway && !link_for_convert(ctx)) return WP_ERR;

    if (plat_file_open(native_path, PLAT_FILE_READ, &f) != WP_OK) {
        wp_action_tell(ctx, "Couldn't open that file");
        return WP_ERR;
    }
    if (plat_file_size(f, &size) != WP_OK) {
        plat_file_close(f);
        wp_action_tell(ctx, "Couldn't read that file");
        return WP_ERR;
    }
    if (via_gateway && size + 1 > WP_ACTION_CONVERT_MAX_FILE) {
        plat_file_close(f);
        wp_action_tell(ctx, "That file is too large to send to the gateway");
        return WP_RANGE;
    }
    buf = (u8 *)mem_alloc(size + 2);
    if (buf == NULL) {
        plat_file_close(f);
        wp_action_tell(ctx, "Not enough memory to open that file");
        return WP_NOMEM;
    }
    data = via_gateway ? buf + 1 : buf;
    st = plat_file_read(f, data, size, &read_len);
    plat_file_close(f);

    if (st == WP_OK) {
        /* WordStar is detected by CONTENT, not by name: those files
           carry no magic number and no reserved extension (.WS, .DOC,
           none at all), and the sniff never claims plain ASCII. The
           rest is by name: RTF's "{\rtf" opening is as good as a magic
           number, but a writer who names a plain-text file .rtf wants
           it read as RTF and told what is wrong, not silently shown as
           text; Markdown and ODT have no opening at all. See
           docs/formats/. */
        if (via_gateway) {
            buf[0] = (u8)OFFLOAD_FORMAT_ODT;
            st = convert_import(ctx, buf, read_len + 1, &fresh, sink);
        } else if (format_ws_looks_like(data, read_len)) {
            st = format_ws_import(data, read_len, &fresh);
        } else if (wp_path_ends_with_ci(native_path, ".RTF")) {
            st = (sink != NULL) ? format_rtf_import_images(data, read_len, &fresh, sink)
                                : format_rtf_import(data, read_len, &fresh);
        } else if (wp_path_ends_with_ci(native_path, ".MD")) {
            st = format_md_import(data, read_len, &fresh);
        } else {
            st = format_txt_import(data, read_len, &fresh);
        }
    }
    mem_free(buf);

    if (st != WP_OK) {
        if (!via_gateway) wp_action_tell(ctx, "Couldn't load that file; the document is unchanged");
        return st;
    }
    wp_action_replace_document(ctx, &fresh);
    wp_action_remember_path(ctx, native_path);
    return WP_OK;
}

wp_status wp_action_save_file(wp_app_ctx *ctx, const char *native_path, wp_image_source *img_src)
{
    wp_bool as_rtf = wp_path_ends_with_ci(native_path, ".RTF");
    wp_bool as_md = wp_path_ends_with_ci(native_path, ".MD");
    u32 needed = 0;
    u8 *buf;
    wp_status st;

    if (!ctx->doc_loaded) return WP_OK;

    if (wp_path_ends_with_ci(native_path, ".ODT") || wp_path_ends_with_ci(native_path, ".PDF")) {
        if (!link_for_convert(ctx)) return WP_ERR;
        return convert_export(ctx, native_path,
                              wp_path_ends_with_ci(native_path, ".ODT") ? (u8)OFFLOAD_FORMAT_ODT
                                                                        : (u8)OFFLOAD_FORMAT_PDF);
    }

    /* The two-call sizing convention every exporter shares. */
    if (as_rtf) {
        if (img_src != NULL) format_rtf_export_images(ctx->doc, NULL, 0, &needed, img_src);
        else                 format_rtf_export(ctx->doc, NULL, 0, &needed);
    } else if (as_md) {
        format_md_export(ctx->doc, NULL, 0, &needed);
    } else {
        format_txt_export(ctx->doc, NULL, 0, &needed);
    }
    buf = (u8 *)mem_alloc(needed > 0 ? needed : 1);
    if (buf == NULL) {
        wp_action_tell(ctx, "Not enough memory to save");
        return WP_NOMEM;
    }

    if (as_rtf) {
        st = (img_src != NULL) ? format_rtf_export_images(ctx->doc, buf, needed, &needed, img_src)
                               : format_rtf_export(ctx->doc, buf, needed, &needed);
    } else if (as_md) {
        st = format_md_export(ctx->doc, buf, needed, &needed);
    } else {
        st = format_txt_export(ctx->doc, buf, needed, &needed);
    }
    if (st == WP_OK) {
        st = wp_action_write_file(ctx, native_path, buf, needed, WP_TRUE);
    } else {
        wp_action_tell(ctx, "Couldn't build the file to save");
    }
    mem_free(buf);

    if (st == WP_OK) wp_action_remember_path(ctx, native_path);
    return st;
}

wp_status wp_action_file_open(wp_app_ctx *ctx, wp_pick_path_fn pick, wp_image_sink *sink)
{
    char path[WP_APP_PATH_MAX];

    /* Opening replaces the whole document, so it asks before discarding
       unsaved work -- BEFORE the picker, so the writer is not made to
       choose a file and only then told they cannot have it. */
    if (!wp_action_confirm_discard(ctx)) return WP_OK;

    /* An ex command may already have named the file (":e foo"); then
       the picker is skipped, which is the whole point: never leave the
       keyboard to name a file. */
    if (!wp_keymap_take_pending_path(path, (u32)sizeof(path))) {
        if (pick == NULL || !pick(ctx, WP_FALSE, path, (u32)sizeof(path))) return WP_OK;
    }
    return wp_action_load_file(ctx, path, sink);
}

wp_bool wp_action_save_path(wp_app_ctx *ctx, wp_bool save_as, wp_pick_path_fn pick,
                            char *path_out, u32 cap)
{
    if (!ctx->doc_loaded) return WP_FALSE;
    if (wp_keymap_take_pending_path(path_out, cap)) return WP_TRUE;
    if (!save_as && ctx->doc_path[0] != '\0' && strlen(ctx->doc_path) < cap) {
        strcpy(path_out, ctx->doc_path);
        return WP_TRUE;
    }
    return (wp_bool)(pick != NULL && pick(ctx, WP_TRUE, path_out, cap));
}

wp_status wp_action_file_save(wp_app_ctx *ctx, wp_bool save_as, wp_pick_path_fn pick,
                              wp_image_source *img_src)
{
    char path[WP_APP_PATH_MAX];

    if (!wp_action_save_path(ctx, save_as, pick, path, (u32)sizeof(path))) return WP_OK;
    return wp_action_save_file(ctx, path, img_src);
}
