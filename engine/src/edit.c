#include "editing.h"
#include "edit_internal.h"

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
