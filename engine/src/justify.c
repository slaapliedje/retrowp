/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include "layout.h"
#include "platform.h"
#include "utf8.h"

void wp_line_fragment_iter_init(wp_line_fragment_iter *iter, const wp_line *line)
{
    iter->run_index = line->start_run;
    iter->byte_offset = line->start_offset;
    iter->tab_index = line->first_tab;
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
    out->is_tab = WP_FALSE;
    out->tab_twips = 0;
    if (bytes[seg_start] == '\t') {
        /* One tab, one fragment; its advance is the layout's. */
        const wp_paragraph_layout *layout = (const wp_paragraph_layout *)para->layout;
        iter->byte_offset = seg_start + 1;
        seg_is_space = WP_FALSE;
        out->is_tab = WP_TRUE;
        if (layout != NULL && iter->tab_index < layout->tab_count) {
            out->tab_twips = layout->tab_widths[iter->tab_index];
        }
        iter->tab_index++;
    } else {
        seg_is_space = (wp_bool)(bytes[seg_start] == ' ');
        iter->byte_offset = utf8_next(bytes, run_end, iter->byte_offset);
        while (iter->byte_offset < run_end && bytes[iter->byte_offset] != '\t' &&
               (wp_bool)(bytes[iter->byte_offset] == ' ') == seg_is_space) {
            iter->byte_offset = utf8_next(bytes, run_end, iter->byte_offset);
        }
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
    if (line->tab_count > 0) return 0;               /* keep tabbed columns on their stops */

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

        if (frag.is_tab) {
            /* The cursor is past this tab (the check above stopped
               before it otherwise): its whole advance counts. */
            x_twips += frag.tab_twips;
            continue;
        }

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

/* The inverse of wp_line_cursor_x_twips, for a click: the same fragment
   walk, stopping in the fragment that contains x and then stepping
   through it a codepoint at a time until the nearest boundary. Nearest,
   not "the boundary before x": a click on the right half of a glyph
   lands after it, which is where every editor puts the caret. */
wp_bool wp_line_pos_at_x_twips(wp_document *doc, wp_paragraph *para, const wp_line *line,
                               i32 x_twips, i32 space_extra_twips,
                               u32 *run_out, u32 *offset_out)
{
    wp_line_fragment_iter iter;
    wp_line_fragment frag;
    cursor_font_slot slot;
    i32 x_acc = 0;
    wp_bool found = WP_FALSE;

    slot.handle = 0;
    slot.font = NULL;
    slot.valid = WP_FALSE;

    *run_out = line->start_run;
    *offset_out = line->start_offset;
    if (para->kind == (u8)WP_PARA_IMAGE) return WP_TRUE;
    if (x_twips <= 0) return WP_TRUE;

    wp_line_fragment_iter_init(&iter, line);
    while (!found && wp_line_next_fragment(doc, para, line, &iter, &frag)) {
        const wp_run *run = wp_paragraph_run_get(para, (u16)frag.run_index);
        const u8 *bytes;
        plat_font *font;
        u32 frag_len = frag.byte_end - frag.byte_start;
        i32 width_twips = 0;
        i32 advance;

        /* The end of this fragment is a boundary the click may land on
           even when the fragment itself cannot be measured. */
        *run_out = frag.run_index;
        *offset_out = frag.byte_end;

        if (frag.is_tab) {
            if (x_twips < x_acc + frag.tab_twips) {
                /* In the tab's gap: before it or after it, whichever edge is nearer. */
                *offset_out = ((x_twips - x_acc) * 2 < frag.tab_twips) ? frag.byte_start : frag.byte_end;
                found = WP_TRUE;
                break;
            }
            x_acc += frag.tab_twips;
            continue;
        }
        if (run == NULL) continue;
        bytes = wp_strpool_get(&doc->strings, run->text);
        if (bytes == NULL || frag_len == 0) continue;

        font = cursor_slot_font(doc, run->style, &slot);
        if (font == NULL) continue;
        if (plat_measure_text(font, bytes + frag.byte_start, frag_len, &width_twips) != WP_OK) continue;
        advance = width_twips + (frag.is_space ? space_extra_twips : 0);

        if (x_twips < x_acc + advance) {
            /* Inside this fragment: find the codepoint boundary nearest
               to x by measuring growing prefixes. Stretched gaps of a
               justified line count once, at the fragment's end. */
            u32 off = frag.byte_start;
            i32 prev_x = x_acc;

            while (off < frag.byte_end) {
                u32 next = utf8_next(bytes, frag.byte_end, off);
                i32 w = 0;

                if (next <= off) next = frag.byte_end; /* malformed: take the rest */
                if (plat_measure_text(font, bytes + frag.byte_start, next - frag.byte_start, &w) != WP_OK) {
                    break;
                }
                if (x_twips < x_acc + w) {
                    /* x lies in [prev_x, x_acc + w): nearer edge wins. */
                    *offset_out = ((x_twips - prev_x) <= (x_acc + w - x_twips)) ? off : next;
                    found = WP_TRUE;
                    break;
                }
                prev_x = x_acc + w;
                off = next;
            }
            if (!found) {
                *offset_out = frag.byte_end;
                found = WP_TRUE;
            }
            *run_out = frag.run_index;
            break;
        }
        x_acc += advance;
    }

    if (!found) {
        *run_out = line->end_run;
        *offset_out = line->end_offset;
    }
    if (slot.valid) plat_font_close(slot.font);
    return WP_TRUE;
}
