/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "editing.h"
#include "edit_internal.h"
#include "layout.h"
#include "platform.h"

wp_status run_insert_at(wp_paragraph *para, u16 index, wp_span text, wp_cstyle style)
{
    wp_status st;
    u16 new_idx;
    u16 i;
    wp_run new_run;

    new_run.text = text;
    new_run.style = style;

    /* Append (growing capacity via the tested T1.2 path if needed), then
       rotate the freshly-appended element backward into position
       `index`, shifting [index, new_idx) right by one. */
    st = wp_paragraph_run_add(para, text, style, &new_idx);
    if (st != WP_OK) return st;

    for (i = new_idx; i > index; i--) {
        para->runs[i] = para->runs[i - 1];
    }
    para->runs[index] = new_run;
    return WP_OK;
}

void run_remove_at(wp_paragraph *para, u16 index)
{
    u16 i;

    for (i = index; (u16)(i + 1) < para->run_count; i++) {
        para->runs[i] = para->runs[i + 1];
    }
    para->run_count--;
    /* Removing a run renumbers every run after it, so any wp_line still
       holding start_run/end_run indices now describes different runs
       than it did -- the layout must be rebuilt before anything paints
       it. The insert side gets this for free (run_insert_at goes
       through wp_paragraph_run_add, which sets the flag); the remove
       side had no such incidental path and silently skipped it. */
    para->layout_dirty = WP_TRUE;
}

wp_status insert_span_at_pos(wp_document *doc, wp_pos pos, wp_span span, wp_pos *out_pos)
{
    wp_status st;
    wp_section *sec;
    wp_paragraph *para;
    wp_cstyle style;
    u16 insert_index;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[pos.section];
    if (pos.paragraph >= sec->para_count) return WP_RANGE;
    para = &sec->paras[pos.paragraph];

    if (para->run_count == 0) {
        /* empty paragraph: no context to inherit from */
        style = doc->default_cstyle;
        insert_index = 0;
    } else if (pos.offset == 0 && pos.run == 0) {
        /* very start of a non-empty paragraph: inherit the first run's style */
        style = para->runs[0].style;
        insert_index = 0;
    } else if (pos.offset == 0) {
        /* boundary between run (pos.run - 1) and run pos.run: inherit
           the PRECEDING run's style ("sticks to what you just typed") */
        if (pos.run >= para->run_count) return WP_RANGE;
        style = para->runs[pos.run - 1].style;
        insert_index = (u16)pos.run;
    } else {
        const wp_run *run;

        if (pos.run >= para->run_count) return WP_RANGE;
        run = &para->runs[pos.run];
        if (pos.offset > run->text.length) return WP_RANGE;
        style = run->style;

        if (pos.offset == run->text.length) {
            /* end of this run: inherit its style, insert right after it */
            insert_index = (u16)(pos.run + 1);
        } else {
            /* strictly mid-run: split. Shrink the original run in place
               to become the left fragment, insert the new text right
               after it, then insert a new run for the tail fragment
               right after that. Undo of this insert only needs to
               remove the middle (new-text) run -- see docs/editing.md
               for why the two flanking fragments never need to be
               re-merged back into one run for correctness. */
            wp_span tail_span;

            tail_span.offset = run->text.offset + pos.offset;
            tail_span.length = run->text.length - pos.offset;

            para->runs[pos.run].text.length = pos.offset;
            insert_index = (u16)(pos.run + 1);

            st = run_insert_at(para, insert_index, span, style);
            if (st != WP_OK) return st;

            st = run_insert_at(para, (u16)(insert_index + 1), tail_span, style);
            if (st != WP_OK) return st;

            if (out_pos) {
                out_pos->section = pos.section;
                out_pos->paragraph = pos.paragraph;
                out_pos->run = insert_index;
                out_pos->offset = span.length;
            }
            return WP_OK;
        }
    }

    st = run_insert_at(para, insert_index, span, style);
    if (st != WP_OK) return st;

    if (out_pos) {
        out_pos->section = pos.section;
        out_pos->paragraph = pos.paragraph;
        out_pos->run = insert_index;
        out_pos->offset = span.length;
    }
    return WP_OK;
}

wp_status wp_document_insert_text_raw(wp_document *doc, wp_pos pos,
                                       const u8 *utf8, u32 len,
                                       wp_pos *out_pos)
{
    wp_status st;
    wp_span new_span;

    st = wp_strpool_append(&doc->strings, utf8, len, &new_span);
    if (st != WP_OK) return st;

    return insert_span_at_pos(doc, pos, new_span, out_pos);
}

u16 find_run_by_span(const wp_paragraph *para, wp_span span)
{
    u16 i;

    for (i = 0; i < para->run_count; i++) {
        if (para->runs[i].text.offset == span.offset &&
            para->runs[i].text.length == span.length) {
            return i;
        }
    }
    return para->run_count;
}

wp_status paragraph_insert_at(wp_section *sec, u32 index, u32 *out_index)
{
    wp_status st;
    u32 new_idx;
    u32 i;
    wp_paragraph empty_para;

    st = wp_section_paragraph_add(sec, &new_idx);
    if (st != WP_OK) return st;

    /* wp_section_paragraph_add zero-initializes the newly appended slot
       -- capture it before the rotation below shifts existing slots
       into place over it. */
    empty_para = sec->paras[new_idx];

    for (i = new_idx; i > index; i--) {
        sec->paras[i] = sec->paras[i - 1];
    }
    sec->paras[index] = empty_para;

    if (out_index) *out_index = index;
    return WP_OK;
}

void paragraph_remove_at(wp_section *sec, u32 index)
{
    u32 i;

    wp_paragraph_layout_free(&sec->paras[index]);
    mem_free(sec->paras[index].runs);

    for (i = index; (u32)(i + 1) < sec->para_count; i++) {
        sec->paras[i] = sec->paras[i + 1];
    }
    sec->para_count--;
}

/* Validates pos and reports the run index every run at/after which
   should move into the split-off paragraph, mirroring
   insert_span_at_pos's own validation branches exactly (see
   docs/editing.md) -- split needs the same "is this pos meaningful in
   this paragraph" answer insert does, just without inserting text. If
   pos lands strictly mid-run, splits that run in place (the tail
   fragment becomes a real run so the caller's move-every-run-at-or-
   after-split_run loop picks it up uniformly) and returns the tail
   run's index. */
static wp_status paragraph_split_point(wp_paragraph *para, wp_pos pos, u16 *out_split_run)
{
    if (para->run_count == 0) {
        *out_split_run = 0;
        return WP_OK;
    }
    if (pos.offset == 0 && pos.run == 0) {
        *out_split_run = 0;
        return WP_OK;
    }
    if (pos.offset == 0) {
        if (pos.run >= para->run_count) return WP_RANGE;
        *out_split_run = (u16)pos.run;
        return WP_OK;
    }
    {
        wp_status st;
        const wp_run *run;

        if (pos.run >= para->run_count) return WP_RANGE;
        run = &para->runs[pos.run];
        if (pos.offset > run->text.length) return WP_RANGE;

        if (pos.offset == run->text.length) {
            *out_split_run = (u16)(pos.run + 1);
            return WP_OK;
        }

        {
            wp_span tail_span;
            wp_cstyle style = run->style;

            tail_span.offset = run->text.offset + pos.offset;
            tail_span.length = run->text.length - pos.offset;
            para->runs[pos.run].text.length = pos.offset;

            st = run_insert_at(para, (u16)(pos.run + 1), tail_span, style);
            if (st != WP_OK) return st;

            *out_split_run = (u16)(pos.run + 1);
            return WP_OK;
        }
    }
}

wp_status wp_document_split_paragraph_raw(wp_document *doc, wp_pos pos, wp_pos *out_pos)
{
    wp_status st;
    wp_section *sec;
    wp_paragraph *para;
    wp_paragraph *new_para;
    u32 new_idx;
    u16 split_run;
    u16 i, moved_count;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[pos.section];
    if (pos.paragraph >= sec->para_count) return WP_RANGE;
    para = &sec->paras[pos.paragraph];

    st = paragraph_split_point(para, pos, &split_run);
    if (st != WP_OK) return st;

    st = paragraph_insert_at(sec, pos.paragraph + 1, &new_idx);
    if (st != WP_OK) return st;

    /* paragraph_insert_at may have realloc'd sec->paras -- re-fetch
       rather than reuse the pointer captured before the call. */
    para = &sec->paras[pos.paragraph];
    new_para = &sec->paras[new_idx];
    new_para->style = para->style;

    moved_count = (u16)(para->run_count - split_run);
    for (i = 0; i < moved_count; i++) {
        u16 dummy;
        st = wp_paragraph_run_add(new_para, para->runs[split_run + i].text,
                                   para->runs[split_run + i].style, &dummy);
        if (st != WP_OK) return st;
    }
    /* Runs never own memory (a run is just a pool span + style handle),
       so truncating the survivor's count abandons the moved slots
       safely -- nothing to free, matching how run_remove_at works. */
    para->run_count = split_run;
    para->layout_dirty = WP_TRUE;

    if (out_pos) {
        out_pos->section = pos.section;
        out_pos->paragraph = pos.paragraph + 1;
        out_pos->run = 0;
        out_pos->offset = 0;
    }
    return WP_OK;
}

void paragraph_end_run_offset(const wp_paragraph *para, u32 *out_run, u32 *out_offset)
{
    if (para->run_count == 0) {
        *out_run = 0;
        *out_offset = 0;
    } else {
        *out_run = (u32)(para->run_count - 1);
        *out_offset = para->runs[*out_run].text.length;
    }
}

wp_pos pos_from_byte_in_paragraph(u32 section, u32 paragraph, const wp_paragraph *para,
                                   u32 byte_offset)
{
    wp_pos pos;
    u32 consumed = 0;
    u32 r;

    pos.section = section;
    pos.paragraph = paragraph;

    if (para->run_count == 0) {
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }

    for (r = 0; r < para->run_count; r++) {
        u32 len = para->runs[r].text.length;

        if (byte_offset < consumed + len) {
            pos.run = r;
            pos.offset = byte_offset - consumed;
            return pos;
        }
        if (byte_offset == consumed + len) {
            if ((u32)(r + 1) < para->run_count) {
                pos.run = r + 1;
                pos.offset = 0;
            } else {
                pos.run = r;
                pos.offset = len;
            }
            return pos;
        }
        consumed += len;
    }

    paragraph_end_run_offset(para, &pos.run, &pos.offset);
    return pos;
}

/* Length in bytes of [run_a,off_a .. run_b,off_b) within a SINGLE
   paragraph -- callers guarantee run_a/off_a precedes-or-equals
   run_b/off_b and both are valid positions in this exact paragraph.
   Shared length/copy pair (like format_txt.c's own export_size/export
   split) so wp_document_extract_text's two-call sizing convention never
   risks the two passes disagreeing. */
static u32 paragraph_range_length(const wp_paragraph *para,
                                   u32 run_a, u32 off_a, u32 run_b, u32 off_b)
{
    u32 total = 0;
    u32 r;

    if (para->run_count == 0) return 0;

    for (r = run_a; r <= run_b && r < para->run_count; r++) {
        u32 lo = (r == run_a) ? off_a : 0;
        u32 hi = (r == run_b) ? off_b : para->runs[r].text.length;
        if (hi > lo) total += hi - lo;
    }
    return total;
}

static u32 paragraph_range_copy(const wp_document *doc, const wp_paragraph *para,
                                 u32 run_a, u32 off_a, u32 run_b, u32 off_b, u8 *out)
{
    u32 written = 0;
    u32 r;

    if (para->run_count == 0) return 0;

    for (r = run_a; r <= run_b && r < para->run_count; r++) {
        u32 lo = (r == run_a) ? off_a : 0;
        u32 hi = (r == run_b) ? off_b : para->runs[r].text.length;

        if (hi > lo) {
            const u8 *bytes = wp_strpool_get(&doc->strings, para->runs[r].text);
            memcpy(out + written, bytes + lo, hi - lo);
            written += hi - lo;
        }
    }
    return written;
}

wp_status wp_document_extract_text(const wp_document *doc, wp_selection sel,
                                    u8 *out, u32 out_cap, u32 *out_len)
{
    wp_pos start, end;
    const wp_section *sec;
    u32 needed;
    u32 written;
    u32 pi;

    if (wp_pos_compare(sel.anchor, sel.caret) <= 0) {
        start = sel.anchor;
        end = sel.caret;
    } else {
        start = sel.caret;
        end = sel.anchor;
    }

    if (start.section >= doc->section_count) return WP_RANGE;
    if (start.section != end.section) return WP_RANGE;
    sec = &doc->sections[start.section];
    if (start.paragraph >= sec->para_count || end.paragraph >= sec->para_count) return WP_RANGE;

    needed = 0;
    if (start.paragraph == end.paragraph) {
        needed = paragraph_range_length(&sec->paras[start.paragraph],
                                         start.run, start.offset, end.run, end.offset);
    } else {
        const wp_paragraph *first = &sec->paras[start.paragraph];
        const wp_paragraph *last = &sec->paras[end.paragraph];
        u32 first_end_run, first_end_offset;

        paragraph_end_run_offset(first, &first_end_run, &first_end_offset);
        needed += paragraph_range_length(first, start.run, start.offset,
                                          first_end_run, first_end_offset);
        needed += 1; /* paragraph-boundary '\n' */

        for (pi = start.paragraph + 1; pi < end.paragraph; pi++) {
            const wp_paragraph *mid = &sec->paras[pi];
            u32 mid_end_run, mid_end_offset;

            paragraph_end_run_offset(mid, &mid_end_run, &mid_end_offset);
            needed += paragraph_range_length(mid, 0, 0, mid_end_run, mid_end_offset);
            needed += 1;
        }

        needed += paragraph_range_length(last, 0, 0, end.run, end.offset);
    }

    if (out_len) *out_len = needed;
    if (needed > out_cap) return WP_RANGE;

    written = 0;
    if (start.paragraph == end.paragraph) {
        written += paragraph_range_copy(doc, &sec->paras[start.paragraph],
                                         start.run, start.offset, end.run, end.offset,
                                         out + written);
    } else {
        const wp_paragraph *first = &sec->paras[start.paragraph];
        const wp_paragraph *last = &sec->paras[end.paragraph];
        u32 first_end_run, first_end_offset;

        paragraph_end_run_offset(first, &first_end_run, &first_end_offset);
        written += paragraph_range_copy(doc, first, start.run, start.offset,
                                         first_end_run, first_end_offset, out + written);
        out[written] = '\n';
        written++;

        for (pi = start.paragraph + 1; pi < end.paragraph; pi++) {
            const wp_paragraph *mid = &sec->paras[pi];
            u32 mid_end_run, mid_end_offset;

            paragraph_end_run_offset(mid, &mid_end_run, &mid_end_offset);
            written += paragraph_range_copy(doc, mid, 0, 0, mid_end_run, mid_end_offset,
                                             out + written);
            out[written] = '\n';
            written++;
        }

        written += paragraph_range_copy(doc, last, 0, 0, end.run, end.offset, out + written);
    }

    return WP_OK;
}

wp_status wp_document_merge_paragraph_raw(wp_document *doc, u32 section,
                                           u32 paragraph, wp_pos *out_pos)
{
    wp_status st;
    wp_section *sec;
    wp_paragraph *first;
    wp_paragraph *second;
    wp_pos join_pos;
    u16 i, second_count;

    if (section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[section];
    if (paragraph >= sec->para_count) return WP_RANGE;
    if (paragraph + 1 >= sec->para_count) return WP_RANGE;

    first = &sec->paras[paragraph];
    second = &sec->paras[paragraph + 1];

    /* Capture the join point BEFORE mutating first -- the position
       immediately after first's ORIGINAL last codepoint (or (0,0) if
       first was empty), the exact convention wp_pos_next/prev's own
       end_of_paragraph uses, and the only form paragraph_split_point
       above accepts as "the end of a paragraph's existing content." */
    join_pos.section = section;
    join_pos.paragraph = paragraph;
    if (first->run_count == 0) {
        join_pos.run = 0;
        join_pos.offset = 0;
    } else {
        join_pos.run = (u32)(first->run_count - 1);
        join_pos.offset = first->runs[join_pos.run].text.length;
    }

    second_count = second->run_count;
    for (i = 0; i < second_count; i++) {
        u16 dummy;
        /* second is re-fetched implicitly each iteration via the local
           pointer -- safe because wp_paragraph_run_add grows FIRST's
           array, never second's, so second's pointer stays valid. */
        st = wp_paragraph_run_add(first, second->runs[i].text,
                                   second->runs[i].style, &dummy);
        if (st != WP_OK) return st;
    }

    paragraph_remove_at(sec, paragraph + 1);
    sec->paras[paragraph].layout_dirty = WP_TRUE;

    if (out_pos) *out_pos = join_pos;
    return WP_OK;
}

wp_status wp_document_delete_range_raw(wp_document *doc, u32 section,
                                        u32 paragraph, u16 run_index,
                                        u32 start_offset, u32 end_offset)
{
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;

    if (section >= doc->section_count) return WP_RANGE;
    sec = &doc->sections[section];
    if (paragraph >= sec->para_count) return WP_RANGE;
    para = &sec->paras[paragraph];
    if (run_index >= para->run_count) return WP_RANGE;
    run = &para->runs[run_index];

    if (start_offset >= end_offset || end_offset > run->text.length) return WP_RANGE;

    /* Every path below shortens or removes run bytes, which invalidates
       this paragraph's line list: a wp_line records absolute
       start_run/end_run/start_offset/end_offset, so after a delete those
       ranges describe MORE bytes than the runs still hold. A paint loop
       walking that stale layout doesn't merely show deleted text -- it
       reads past the end of the shortened run's span in the string pool.
       Marking dirty here is what makes the four backends' own
       relayout-before-paint see the change; without it, a delete was
       invisible to them and the out-of-bounds read stood. (Only the
       mid-run split case below ever set the flag, and only by accident,
       via run_insert_at.) */
    para->layout_dirty = WP_TRUE;

    if (start_offset == 0 && end_offset == run->text.length) {
        run_remove_at(para, run_index);
        return WP_OK;
    }

    if (start_offset == 0) {
        /* right fragment survives */
        run->text.offset += end_offset;
        run->text.length -= end_offset;
        return WP_OK;
    }

    if (end_offset == run->text.length) {
        /* left fragment survives */
        run->text.length = start_offset;
        return WP_OK;
    }

    /* split: left fragment stays in place, right fragment becomes a new run */
    {
        wp_span right_span;
        wp_cstyle style = run->style;

        right_span.offset = run->text.offset + end_offset;
        right_span.length = run->text.length - end_offset;

        run->text.length = start_offset;

        return run_insert_at(para, (u16)(run_index + 1), right_span, style);
    }
}

/* -- Range helpers, shared by the styling family ----------------------- */

u32 para_total_bytes(const wp_paragraph *para)
{
    u32 total = 0;
    u16 i;

    for (i = 0; i < para->run_count; i++) total += para->runs[i].text.length;
    return total;
}

u32 para_byte_of_pos(const wp_paragraph *para, wp_pos pos)
{
    u32 total = 0;
    u16 i;

    for (i = 0; i < para->run_count && i < (u16)pos.run; i++)
        total += para->runs[i].text.length;
    return total + pos.offset;
}

void run_at_byte(const wp_paragraph *para, u32 byte, u16 *out_index, u32 *out_offset)
{
    u32 consumed = 0;
    u16 i;

    for (i = 0; i < para->run_count; i++) {
        u32 len = para->runs[i].text.length;
        if (byte < consumed + len) {
            *out_index = i;
            *out_offset = byte - consumed;
            return;
        }
        consumed += len;
    }
    *out_index = para->run_count;   /* one past the end */
    *out_offset = 0;
}

wp_status run_split_at(wp_paragraph *para, u16 index, u32 offset)
{
    wp_run *run = wp_paragraph_run_get(para, index);
    wp_span right;
    wp_cstyle style;

    if (run == NULL) return WP_OK;                 /* one past the end */
    if (offset == 0 || offset >= run->text.length) return WP_OK;

    /* Read both fields BEFORE run_insert_at: it may grow the runs array,
       which invalidates `run` itself. */
    style = run->style;
    right.offset = run->text.offset + offset;
    right.length = run->text.length - offset;

    run->text.length = offset;
    return run_insert_at(para, (u16)(index + 1), right, style);
}

wp_bool range_in_paragraph(const wp_paragraph *para, u32 section, u32 paragraph,
                            wp_pos start, wp_pos end, u32 *out_from, u32 *out_to)
{
    u32 from, to;

    if (section < start.section || section > end.section) return WP_FALSE;
    if (section == start.section && paragraph < start.paragraph) return WP_FALSE;
    if (section == end.section && paragraph > end.paragraph) return WP_FALSE;

    from = (section == start.section && paragraph == start.paragraph)
             ? para_byte_of_pos(para, start) : 0;
    to   = (section == end.section && paragraph == end.paragraph)
             ? para_byte_of_pos(para, end) : para_total_bytes(para);

    if (to > para_total_bytes(para)) to = para_total_bytes(para);
    if (to <= from) return WP_FALSE;

    *out_from = from;
    *out_to = to;
    return WP_TRUE;
}

wp_bool wp_document_range_has_flags(const wp_document *doc,
                                     wp_pos start, wp_pos end, u16 flags)
{
    u32 si, pi;
    wp_bool saw_a_run = WP_FALSE;

    if (wp_pos_compare(start, end) > 0) {
        wp_pos swap = start; start = end; end = swap;
    }

    for (si = start.section; si <= end.section && si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get((wp_document *)doc, si);

        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            u32 from, to, consumed = 0;
            u16 ri;

            if (!range_in_paragraph(para, si, pi, start, end, &from, &to)) continue;

            for (ri = 0; ri < para->run_count; ri++) {
                u32 run_start = consumed;
                u32 run_end = consumed + para->runs[ri].text.length;
                consumed = run_end;

                if (run_end <= from || run_start >= to) continue;  /* outside */
                saw_a_run = WP_TRUE;
                {
                    const wp_char_style *cs =
                        wp_document_cstyle_get(doc, para->runs[ri].style);
                    if (cs == NULL || (cs->flags & flags) != flags) return WP_FALSE;
                }
            }
        }
    }
    return saw_a_run;
}
