/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "test.h"
#include "app_ctx.h"
#include "format_txt.h"
#include "layout.h"

/* The "does the user actually SEE what they typed" harness.
   ------------------------------------------------------------------
   Every backend's paint loop renders a paragraph by walking
   para->layout's wp_line list and, per line, iterating
   wp_line_next_fragment to get (run_index, byte_start, byte_end)
   triples it then hands to plat_draw_text. Nothing in the engine's edit
   path (edit.c/vimode.c) or platform/common/keymap.c rebuilds that
   layout after an edit -- edit.c only sets para->layout_dirty and
   leaves the rebuild to the app.

   That gap produced a real, shipped, four-backend bug: three of the
   four app shells relayouted only on a window resize or a whole-
   document replace, so a keystroke edit left every paint walking a
   STALE layout. Typed text landed in the document model as a genuine
   wp_run byte edit and never appeared on screen -- and on Amiga the
   stale run/offset ranges no longer described the reshaped run array
   at all, so the paint walked off the end of the real content and drew
   a row of NUL boxes. Every structural CI matrix stayed green
   throughout, because none of them ever compared painted output to the
   model.

   These tests close that hole portably. render_visible_text below is a
   faithful host-side stand-in for a backend paint loop: same layout
   walk, same fragment iterator, same run-bytes lookup -- it just
   appends to a buffer instead of calling plat_draw_text. Asserting
   that buffer against the document's own text is exactly the invariant
   the four real paint loops need and none of them could state. A
   backend that forgets to relayout after an edit now fails here, on
   the host, in a second -- rather than on real hardware, visually,
   milestones later. */

#define RENDER_MAX 4096

/* Walks doc exactly the way a backend's paint loop does and accumulates
   every byte it would have drawn. Space fragments are emitted as a
   single space, matching how a paint loop advances x by the gap width
   rather than drawing the raw bytes (tokenize() merges consecutive
   spaces into one gap, so the raw byte count is not what's on screen).
   A line break becomes '\n'. */
static void render_visible_text(wp_document *doc, char *out, u32 out_cap, u32 *out_len)
{
    u32 si, pi, li;
    u32 len = 0;

    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;

            if (layout == NULL) continue;

            for (li = 0; li < layout->line_count; li++) {
                wp_line *line = &layout->lines[li];
                wp_line_fragment_iter iter;
                wp_line_fragment frag;

                wp_line_fragment_iter_init(&iter, line);
                while (wp_line_next_fragment(doc, para, line, &iter, &frag)) {
                    wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);
                    const u8 *bytes;
                    u32 n;

                    /* A backend that dereferenced this without checking
                       would be reading through a NULL pointer; the point
                       of the harness is that it never happens, so assert
                       rather than skip. */
                    ASSERT_TRUE(run != NULL);
                    bytes = wp_strpool_get(&doc->strings, run->text);
                    ASSERT_TRUE(bytes != NULL);

                    if (frag.is_space) {
                        if (len + 1 < out_cap) out[len++] = ' ';
                        continue;
                    }
                    n = frag.byte_end - frag.byte_start;
                    if (len + n < out_cap) {
                        memcpy(out + len, bytes + frag.byte_start, n);
                        len += n;
                    }
                }
                if (len + 1 < out_cap) out[len++] = '\n';
            }
        }
    }

    out[len] = '\0';
    *out_len = len;
}

/* What every app shell must do after any edit: rebuild the layout of
   each paragraph the engine flagged dirty (plus any never built).
   Mirrors the relayout_document each backend now runs before painting. */
static void relayout_dirty(wp_document *doc, i32 width_twips)
{
    u32 si, pi;

    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            if (para->layout != NULL && !para->layout_dirty) continue;
            ASSERT_TRUE(wp_paragraph_relayout(doc, para, width_twips) == WP_OK);
        }
    }
}

static wp_key_event key_char(u32 codepoint)
{
    wp_key_event key;
    key.codepoint = codepoint;
    key.special = (u16)WP_KEY_NONE;
    key.mods = 0;
    key._pad = 0;
    return key;
}

static void feed_string(wp_app_ctx *ctx, const char *s)
{
    u32 i;
    for (i = 0; s[i] != '\0'; i++) {
        wp_keymap_feed_key(ctx, key_char((u32)(u8)s[i]), (u8)s[i]);
    }
}

/* The headline regression: type into a loaded document, relayout the
   way a backend does, and require the typed bytes to be visible in what
   that backend would have painted. Before the fix this failed on
   Atari/Amiga/Mac (they never relayouted after a keystroke) and passed
   only on DOS. */
void test_edit_render_typed_text_is_visible_after_relayout(void)
{
    static const char text[] = "Hello world.\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    char seen[RENDER_MAX];
    u32 seen_len;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    relayout_dirty(&doc, 6000);

    render_visible_text(&doc, seen, RENDER_MAX, &seen_len);
    ASSERT_TRUE(strstr(seen, "Hello world.") != NULL);
    ASSERT_TRUE(strstr(seen, "ZZTOP") == NULL);

    /* vi: 'i' enters Insert mode, then the literal characters. */
    feed_string(&ctx, "iZZTOP");

    /* The engine must have flagged the edited paragraph -- this is the
       signal every backend's relayout depends on. */
    ASSERT_TRUE(wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0)->layout_dirty);

    relayout_dirty(&doc, 6000);
    render_visible_text(&doc, seen, RENDER_MAX, &seen_len);
    ASSERT_TRUE(strstr(seen, "ZZTOP") != NULL);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Walks a paragraph's layout the way a paint loop does and asserts every
   fragment it yields is in bounds for the run it names. This is the
   memory-safety half of the contract: a backend dereferences
   wp_paragraph_run_get(...)->text and reads
   bytes[byte_start..byte_end) with no checking of its own. */
static void assert_fragments_in_bounds(wp_document *doc, wp_paragraph *para)
{
    wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;
    u32 li;

    if (layout == NULL) return;

    for (li = 0; li < layout->line_count; li++) {
        wp_line *line = &layout->lines[li];
        wp_line_fragment_iter iter;
        wp_line_fragment frag;

        wp_line_fragment_iter_init(&iter, line);
        while (wp_line_next_fragment(doc, para, line, &iter, &frag)) {
            wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);

            ASSERT_TRUE(run != NULL);
            ASSERT_TRUE(frag.byte_start <= frag.byte_end);
            ASSERT_TRUE(frag.byte_end <= run->text.length);
        }
    }
}

/* The engine side of the same bug, and the more serious half.
   ------------------------------------------------------------------
   A wp_line stores absolute start_run/end_run/start_offset/end_offset.
   Shortening or removing a run therefore invalidates it -- the recorded
   range now covers MORE bytes than the runs still hold, so a paint loop
   reads past the end of the shortened span in the string pool. Insert
   happened to mark the paragraph dirty (run_insert_at goes through
   wp_paragraph_run_add, which sets the flag); DELETE did not, on any
   path except the mid-run split that also happens to call
   run_insert_at. So deletes were invisible to every backend's
   relayout-before-paint -- including DOS, the one backend that had the
   relayout fix all along -- and the out-of-bounds read stood.

   This asserts the contract directly: after any edit, a
   dirty-flag-driven relayout must leave every fragment in bounds. It
   fails against the pre-fix engine. */
void test_edit_render_delete_marks_layout_dirty_and_stays_in_bounds(void)
{
    static const char text[] = "Hello world.\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_paragraph *para;
    wp_key_event bs;
    u32 i;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    relayout_dirty(&doc, 6000);

    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(!para->layout_dirty);
    assert_fragments_in_bounds(&doc, para);

    /* 'A' appends at end of line and enters Insert mode; backspace then
       deletes real bytes, shortening the run in place. */
    feed_string(&ctx, "A");
    bs.codepoint = 0;
    bs.special = (u16)WP_KEY_BACKSPACE;
    bs.mods = 0;
    bs._pad = 0;
    for (i = 0; i < 6; i++) wp_keymap_feed_key(&ctx, bs, 0);

    /* The signal every backend's relayout depends on. */
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(para->layout_dirty);

    relayout_dirty(&doc, 6000);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    assert_fragments_in_bounds(&doc, para);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Same contract for insert, which only ever worked by accident. */
void test_edit_render_insert_marks_layout_dirty_and_stays_in_bounds(void)
{
    static const char text[] = "Hello world.\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_paragraph *para;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    relayout_dirty(&doc, 6000);

    feed_string(&ctx, "iZZTOP");

    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    ASSERT_TRUE(para->layout_dirty);

    relayout_dirty(&doc, 6000);
    para = wp_section_paragraph_get(wp_document_section_get(&doc, 0), 0);
    assert_fragments_in_bounds(&doc, para);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* Deleting must be as visible as inserting: the same missing-relayout
   bug hid backspaces too, leaving deleted characters on screen. */
void test_edit_render_deleted_text_disappears_after_relayout(void)
{
    static const char text[] = "Hello world.\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_key_event bs;
    char seen[RENDER_MAX];
    u32 seen_len;
    u32 i;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    relayout_dirty(&doc, 6000);

    /* Move to end of the line, enter Insert mode, backspace over "world." */
    feed_string(&ctx, "A");
    bs.codepoint = 0;
    bs.special = (u16)WP_KEY_BACKSPACE;
    bs.mods = 0;
    bs._pad = 0;
    for (i = 0; i < 6; i++) wp_keymap_feed_key(&ctx, bs, 0);

    relayout_dirty(&doc, 6000);
    render_visible_text(&doc, seen, RENDER_MAX, &seen_len);
    ASSERT_TRUE(strstr(seen, "world.") == NULL);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}

/* A clean paragraph must not be relayouted -- this is what makes
   relayouting before EVERY paint affordable on an 8MHz 68000
   (wp_paragraph_relayout has no early-out of its own: it opens fonts,
   tokenizes and measures unconditionally). If this ever regresses, the
   cost is a full re-measure of the whole document per keystroke. */
void test_edit_render_clean_paragraphs_are_not_relayouted(void)
{
    static const char text[] = "First paragraph.\nSecond paragraph.\n";
    wp_document doc;
    wp_undo_journal journal;
    wp_app_ctx ctx;
    wp_section *sec;
    void *layout_before_p0, *layout_before_p1;

    ASSERT_TRUE(format_txt_import((const u8 *)text, (u32)strlen(text), &doc) == WP_OK);
    wp_undo_journal_init(&journal);
    wp_app_ctx_init(&ctx, WP_KEYMAP_VI, &doc, &journal);
    relayout_dirty(&doc, 6000);

    sec = wp_document_section_get(&doc, 0);
    ASSERT_TRUE(sec->para_count >= 2);
    layout_before_p0 = wp_section_paragraph_get(sec, 0)->layout;
    layout_before_p1 = wp_section_paragraph_get(sec, 1)->layout;
    ASSERT_TRUE(layout_before_p0 != NULL && layout_before_p1 != NULL);

    /* Edit only the first paragraph. */
    feed_string(&ctx, "iX");

    ASSERT_TRUE(wp_section_paragraph_get(sec, 0)->layout_dirty);
    ASSERT_TRUE(!wp_section_paragraph_get(sec, 1)->layout_dirty);

    wp_keymap_free(&ctx);
    wp_undo_journal_free(&journal);
    wp_document_free(&doc);
}
