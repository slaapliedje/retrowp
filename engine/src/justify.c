/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "layout.h"
#include "platform.h"
#include "utf8.h"

void wp_line_fragment_iter_init(wp_line_fragment_iter *iter, const wp_line *line)
{
    iter->run_index = line->start_run;
    iter->byte_offset = line->start_offset;
    iter->finished = (wp_bool)(line->start_run == line->end_run &&
                                line->start_offset >= line->end_offset);
}

wp_bool wp_line_next_fragment(wp_document *doc, wp_paragraph *para, const wp_line *line,
                               wp_line_fragment_iter *iter, wp_line_fragment *out)
{
    const wp_run *run;
    const u8 *bytes;
    u32 run_end;
    u32 seg_start;
    wp_bool seg_is_space;

    if (iter->finished) return WP_FALSE;

    /* M15: an image paragraph's line has no run bytes to fragment at
       all (runs[]/run_count stay 0 by construction). Its degenerate
       (0,0,0,0) byte range already makes wp_line_fragment_iter_init set
       `finished` above -- this guard is explicit defense-in-depth
       rather than reliance on that coincidence, the same "guard the
       shared primitive once, don't trust every per-backend call site to
       remember it" reasoning docs/document-model.md's Image section
       gives for putting this here instead of in each backend's paint
       loop. */
    if (para->kind == (u8)WP_PARA_IMAGE) return WP_FALSE;

    /* Zero-length runs (an edit can leave one behind, docs/editing.md)
       or a run already exhausted up to the line's own end_offset are
       skipped by advancing to the next run in range -- same "empty run
       just isn't a token source" treatment tokenize() gives them. */
    for (;;) {
        run = wp_paragraph_run_get(para, (u16)iter->run_index);
        run_end = (iter->run_index == line->end_run) ? line->end_offset : run->text.length;

        if (iter->byte_offset < run_end) break;

        if (iter->run_index >= line->end_run) {
            iter->finished = WP_TRUE;
            return WP_FALSE;
        }
        iter->run_index++;
        iter->byte_offset = 0;
    }

    bytes = wp_strpool_get(&doc->strings, run->text);
    seg_start = iter->byte_offset;
    seg_is_space = (wp_bool)(bytes[seg_start] == ' ');

    iter->byte_offset = utf8_next(bytes, run_end, iter->byte_offset);
    while (iter->byte_offset < run_end &&
           (wp_bool)(bytes[iter->byte_offset] == ' ') == seg_is_space) {
        iter->byte_offset = utf8_next(bytes, run_end, iter->byte_offset);
    }

    out->run_index = iter->run_index;
    out->byte_start = seg_start;
    out->byte_end = iter->byte_offset;
    out->is_space = seg_is_space;

    if (iter->byte_offset >= run_end) {
        if (iter->run_index >= line->end_run) {
            iter->finished = WP_TRUE;
        } else {
            iter->run_index++;
            iter->byte_offset = 0;
        }
    }

    return WP_TRUE;
}

i32 wp_justify_space_extra_twips(wp_document *doc, wp_paragraph *para, const wp_line *line,
                                  u8 align, wp_bool is_last_line_of_paragraph,
                                  i32 target_width_twips)
{
    wp_line_fragment_iter iter;
    wp_line_fragment frag;
    u32 gap_count = 0;

    if (align != WP_ALIGN_JUSTIFY || is_last_line_of_paragraph) return 0;
    if (para->kind == (u8)WP_PARA_IMAGE) return 0;   /* M15: nothing to justify */

    wp_line_fragment_iter_init(&iter, line);
    while (wp_line_next_fragment(doc, para, line, &iter, &frag)) {
        if (frag.is_space) gap_count++;
    }

    if (gap_count == 0) return 0;
    return (target_width_twips - line->width_twips) / (i32)gap_count;
}


/* One open font plus the cstyle it belongs to. wp_line_cursor_x_twips
   walks fragments in paint order, and fragments are split at run
   boundaries, so consecutive fragments overwhelmingly share a style --
   the same single-slot cache shape every backend's own paint loop uses,
   rather than linebreak.c's 8-entry font_cache (that one measures a
   whole paragraph; this one stops at the cursor). */
typedef struct {
    wp_cstyle  handle;
    plat_font *font;
    wp_bool    valid;
} cursor_font_slot;

static plat_font *cursor_slot_font(wp_document *doc, wp_cstyle handle, cursor_font_slot *slot)
{
    const wp_char_style *style;
    plat_font *f = NULL;

    if (slot->valid && slot->handle == handle) return slot->font;

    style = wp_document_cstyle_get(doc, handle);
    if (style == NULL) return NULL;
    if (plat_font_open((const u8 *)"", 0, style->size_twips, style->flags, &f) != WP_OK) {
        return NULL;
    }

    if (slot->valid) plat_font_close(slot->font);
    slot->handle = handle;
    slot->font = f;
    slot->valid = WP_TRUE;
    return f;
}

i32 wp_line_cursor_x_twips(wp_document *doc, wp_paragraph *para, const wp_line *line,
                            u32 cursor_run, u32 cursor_offset, i32 space_extra_twips)
{
    wp_line_fragment_iter iter;
    wp_line_fragment frag;
    cursor_font_slot slot;
    i32 x_twips = 0;

    slot.handle = 0;
    slot.font = NULL;
    slot.valid = WP_FALSE;

    if (para->kind == (u8)WP_PARA_IMAGE) return 0;   /* M15: no run bytes to walk */

    wp_line_fragment_iter_init(&iter, line);
    while (wp_line_next_fragment(doc, para, line, &iter, &frag)) {
        const wp_run *run;
        const u8 *bytes;
        plat_font *font;
        u32 measure_len;
        i32 width_twips = 0;
        wp_bool is_last = WP_FALSE;

        /* Stop BEFORE adding this fragment if the cursor sits at or
           before its first byte. The `>` half also covers a cursor
           parked in a zero-length run (an edit can leave one behind,
           docs/editing.md) -- the fragment walk skips such a run
           entirely, so without it the caret would silently fall through
           to the end of the line. */
        if (frag.run_index > cursor_run) break;
        if (frag.run_index == cursor_run && cursor_offset <= frag.byte_start) break;

        run = wp_paragraph_run_get(para, (u16)frag.run_index);
        if (run == NULL) continue;
        bytes = wp_strpool_get(&doc->strings, run->text);
        if (bytes == NULL) continue;

        if (frag.run_index == cursor_run && cursor_offset < frag.byte_end) {
            measure_len = cursor_offset - frag.byte_start;   /* partial: cursor is inside */
            is_last = WP_TRUE;
        } else {
            measure_len = frag.byte_end - frag.byte_start;
        }

        font = cursor_slot_font(doc, run->style, &slot);
        if (measure_len == 0 || font == NULL) {
            /* Contribute nothing, exactly as every backend's paint loop
               does for the same two conditions -- so a font this
               platform can't open moves the caret and the glyphs it
               tracks by the same (zero) amount instead of desynchronizing
               them. */
            if (is_last) break;
            continue;
        }

        if (plat_measure_text(font, bytes + frag.byte_start, measure_len, &width_twips) == WP_OK) {
            x_twips += width_twips;
        }
        if (is_last) break;

        if (frag.is_space) x_twips += space_extra_twips;
    }

    if (slot.valid) plat_font_close(slot.font);
    return x_twips;
}
