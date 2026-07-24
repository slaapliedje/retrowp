#include <stddef.h>
#include "editing.h"
#include "edit_internal.h"
#include "platform.h"

void wp_undo_journal_init(wp_undo_journal *journal)
{
    journal->undo_top = NULL;
    journal->redo_top = NULL;
}

static void free_stack(wp_op *top)
{
    while (top) {
        wp_op *next = top->next;
        mem_free(top);
        top = next;
    }
}

void wp_undo_journal_free(wp_undo_journal *journal)
{
    free_stack(journal->undo_top);
    free_stack(journal->redo_top);
    journal->undo_top = NULL;
    journal->redo_top = NULL;
}

wp_bool wp_undo_can_undo(const wp_undo_journal *journal)
{
    return (wp_bool)(journal->undo_top != NULL);
}

wp_bool wp_undo_can_redo(const wp_undo_journal *journal)
{
    return (wp_bool)(journal->redo_top != NULL);
}

static void clear_redo(wp_undo_journal *journal)
{
    free_stack(journal->redo_top);
    journal->redo_top = NULL;
}

wp_status wp_edit_insert_text(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                               const u8 *utf8, u32 len, wp_pos *out_pos)
{
    wp_status st;
    wp_pos local_out;
    wp_span new_span;
    wp_op *op;

    st = wp_strpool_append(&doc->strings, utf8, len, &new_span);
    if (st != WP_OK) return st;

    st = insert_span_at_pos(doc, pos, new_span, &local_out);
    if (st != WP_OK) return st;

    op = (wp_op *)mem_alloc((u32)sizeof(wp_op));
    if (op == NULL) return WP_NOMEM;

    op->kind = (u8)WP_OP_INSERT_TEXT;
    op->at = pos;             /* the pre-insert position */
    op->text = new_span;      /* the inserted span -- also identifies the
                                  run undo must remove, via find_run_by_span */
    op->style_before = 0;     /* unused for this kind */
    op->style_after = 0;

    clear_redo(journal);
    op->next = journal->undo_top;
    journal->undo_top = op;

    if (out_pos) *out_pos = local_out;
    return WP_OK;
}

wp_status wp_edit_delete_range(wp_document *doc, wp_undo_journal *journal,
                                u32 section, u32 paragraph, u16 run_index,
                                u32 start_offset, u32 end_offset)
{
    wp_status st;
    wp_section *sec;
    wp_paragraph *para;
    wp_run *run;
    wp_span deleted_span;
    wp_cstyle style;
    u8 case_tag;
    wp_op *op;

    sec = wp_document_section_get(doc, section);
    if (sec == NULL) return WP_RANGE;
    para = wp_section_paragraph_get(sec, paragraph);
    if (para == NULL) return WP_RANGE;
    if (run_index >= para->run_count) return WP_RANGE;
    run = &para->runs[run_index];
    if (start_offset >= end_offset || end_offset > run->text.length) return WP_RANGE;

    /* Capture BEFORE mutating: the deleted sub-span (in pool
       coordinates) and the run's style. Storing the deleted sub-span
       rather than the run's original full span is what lets undo AND
       redo both work uniformly across all four structural cases without
       needing more fields than wp_op has -- see docs/editing.md. */
    deleted_span.offset = run->text.offset + start_offset;
    deleted_span.length = end_offset - start_offset;
    style = run->style;

    if (start_offset == 0 && end_offset == run->text.length) {
        case_tag = 0; /* whole run removed */
    } else if (start_offset == 0) {
        case_tag = 2; /* right fragment survives (deleted was the left part) */
    } else if (end_offset == run->text.length) {
        case_tag = 1; /* left fragment survives (deleted was the right part) */
    } else {
        case_tag = 3; /* split: both fragments survive */
    }

    st = wp_document_delete_range_raw(doc, section, paragraph, run_index,
                                       start_offset, end_offset);
    if (st != WP_OK) return st;

    op = (wp_op *)mem_alloc((u32)sizeof(wp_op));
    if (op == NULL) return WP_NOMEM;

    op->kind = (u8)WP_OP_DELETE_TEXT;
    op->at.section = section;
    op->at.paragraph = paragraph;
    op->at.run = run_index;
    op->at.offset = 0; /* unused for this kind */
    op->text = deleted_span;
    op->style_before = style;
    op->style_after = case_tag;

    clear_redo(journal);
    op->next = journal->undo_top;
    journal->undo_top = op;

    return WP_OK;
}

static wp_op *push_marker_op(wp_undo_journal *journal, wp_op_kind kind)
{
    wp_op *op = (wp_op *)mem_alloc((u32)sizeof(wp_op));
    if (op == NULL) return NULL;

    op->kind = (u8)kind;
    op->at.section = 0;
    op->at.paragraph = 0;
    op->at.run = 0;
    op->at.offset = 0;
    op->text.offset = 0;
    op->text.length = 0;
    op->style_before = 0;
    op->style_after = 0;

    op->next = journal->undo_top;
    journal->undo_top = op;
    return op;
}

void wp_undo_group_begin(wp_undo_journal *journal)
{
    /* A new edit invalidates redo history -- the same rule every
       journaled op already applies, done once here rather than once
       per op inside the bracketed run (each of THOSE calls would
       harmlessly re-clear an already-empty stack, but there's no
       reason to). */
    clear_redo(journal);
    push_marker_op(journal, WP_OP_GROUP_BEGIN);
}

void wp_undo_group_end(wp_undo_journal *journal)
{
    push_marker_op(journal, WP_OP_GROUP_END);
}

wp_status wp_edit_split_paragraph(wp_document *doc, wp_undo_journal *journal,
                                   wp_pos pos, wp_pos *out_pos)
{
    wp_status st;
    wp_pos local_out;
    wp_op *op;

    st = wp_document_split_paragraph_raw(doc, pos, &local_out);
    if (st != WP_OK) return st;

    op = (wp_op *)mem_alloc((u32)sizeof(wp_op));
    if (op == NULL) return WP_NOMEM;

    op->kind = (u8)WP_OP_SPLIT_PARA;
    op->at = pos;             /* the pre-split position; undo re-merges
                                  at (at.section, at.paragraph), redo
                                  re-splits at `at` again */
    op->text.offset = 0;      /* unused for this kind */
    op->text.length = 0;
    op->style_before = 0;
    op->style_after = 0;

    clear_redo(journal);
    op->next = journal->undo_top;
    journal->undo_top = op;

    if (out_pos) *out_pos = local_out;
    return WP_OK;
}

wp_status wp_edit_merge_paragraph(wp_document *doc, wp_undo_journal *journal,
                                   u32 section, u32 paragraph, wp_pos *out_pos)
{
    wp_status st;
    wp_section *sec;
    wp_paragraph *absorbed;
    wp_pos local_out;
    wp_pstyle absorbed_style;
    wp_op *op;

    /* Capture the absorbed (second) paragraph's own style BEFORE the
       merge -- the survivor keeps ITS OWN style through the merge (see
       docs/editing.md), so undo's re-split can't recover the absorbed
       paragraph's original style from the document alone; it must be
       journaled explicitly. */
    sec = wp_document_section_get(doc, section);
    if (sec == NULL) return WP_RANGE;
    absorbed = wp_section_paragraph_get(sec, paragraph + 1);
    if (absorbed == NULL) return WP_RANGE;
    absorbed_style = absorbed->style;

    st = wp_document_merge_paragraph_raw(doc, section, paragraph, &local_out);
    if (st != WP_OK) return st;

    op = (wp_op *)mem_alloc((u32)sizeof(wp_op));
    if (op == NULL) return WP_NOMEM;

    op->kind = (u8)WP_OP_MERGE_PARA;
    op->at = local_out;             /* the join point -- exactly what a
                                        re-split needs to undo this */
    op->text.offset = 0;            /* unused for this kind */
    op->text.length = 0;
    op->style_before = absorbed_style;  /* restored onto the resurrected
                                            paragraph on undo */
    op->style_after = 0;

    clear_redo(journal);
    op->next = journal->undo_top;
    journal->undo_top = op;

    if (out_pos) *out_pos = local_out;
    return WP_OK;
}

wp_status wp_edit_insert_image(wp_document *doc, wp_undo_journal *journal,
                                wp_pos pos, const wp_image_desc *desc)
{
    wp_status st;
    wp_image handle;
    wp_pos split_out;
    wp_section *sec;
    wp_paragraph *image_para;
    u32 image_idx;
    wp_op *op;

    st = wp_document_image_add(doc, desc, &handle);
    if (st != WP_OK) return st;

    st = wp_document_split_paragraph_raw(doc, pos, &split_out);
    if (st != WP_OK) return st;

    /* split_out is (section, pos.paragraph + 1, 0, 0) -- the "after"
       paragraph split created. Insert the image paragraph immediately
       before it, at that same index (the after-paragraph shifts one
       further); split_out.paragraph is exactly the index paragraph_
       insert_at needs. */
    sec = wp_document_section_get(doc, pos.section);
    st = paragraph_insert_at(sec, split_out.paragraph, &image_idx);
    if (st != WP_OK) return st;

    image_para = wp_section_paragraph_get(sec, image_idx);
    image_para->kind = (u8)WP_PARA_IMAGE;
    image_para->image = handle;
    image_para->style = doc->default_pstyle;
    image_para->layout_dirty = WP_TRUE;

    op = (wp_op *)mem_alloc((u32)sizeof(wp_op));
    if (op == NULL) return WP_NOMEM;

    op->kind = (u8)WP_OP_INSERT_IMAGE;
    op->at = pos;              /* the pre-insert position, same convention
                                   WP_OP_SPLIT_PARA uses */
    op->text.offset = 0;       /* unused for this kind */
    op->text.length = 0;
    op->style_before = (u16)handle;  /* the registered image handle */
    op->style_after = 0;

    clear_redo(journal);
    op->next = journal->undo_top;
    journal->undo_top = op;

    return WP_OK;
}

/* Finds the (run, offset) position exactly byte_offset bytes into
   para's concatenated text, using the same canonical convention as
   everything else in this file (a boundary between two runs always
   resolves to the START of the LATER run, never the raw length of the
   earlier one, matching wp_pos_next's own crossing rule) -- except at
   the very end of the paragraph's content, which resolves to
   (run_count - 1, that run's length), matching paragraph_end_run_offset.
   Used by wp_edit_delete_selection to recover a stable position after
   a multi-step delete/merge sequence has restructured the paragraph's
   run array in ways that make the ORIGINAL (run, offset) pair no longer
   meaningful -- byte_offset (computed once, before any mutation) stays
   valid throughout, since nothing before it in the paragraph ever
   changes. */
static wp_pos pos_at_byte_offset_in_paragraph(u32 section, u32 paragraph,
                                               const wp_paragraph *para, u32 byte_offset)
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
                pos.run = r + 1; /* boundary: canonical form is the next run's start */
                pos.offset = 0;
            } else {
                pos.run = r; /* end of paragraph */
                pos.offset = len;
            }
            return pos;
        }
        consumed += len;
    }

    /* byte_offset beyond the paragraph's total length: clamp to end. */
    paragraph_end_run_offset(para, &pos.run, &pos.offset);
    return pos;
}

wp_status wp_edit_delete_selection(wp_document *doc, wp_undo_journal *journal,
                                    wp_selection sel, wp_pos *out_pos)
{
    wp_pos start, end;
    wp_section *sec;
    wp_status st;
    u32 prefix_len;
    u32 r;

    if (wp_pos_compare(sel.anchor, sel.caret) <= 0) {
        start = sel.anchor;
        end = sel.caret;
    } else {
        start = sel.caret;
        end = sel.anchor;
    }

    if (wp_pos_compare(start, end) == 0) {
        if (out_pos) *out_pos = start;
        return WP_OK;
    }

    if (start.section >= doc->section_count) return WP_RANGE;
    if (start.section != end.section) return WP_RANGE;
    sec = &doc->sections[start.section];
    if (start.paragraph >= sec->para_count || end.paragraph >= sec->para_count) return WP_RANGE;

    /* Bytes preceding `start` within its own paragraph -- invariant
       across everything below, since nothing before `start` is ever
       touched; used at the end to recover a stable out_pos regardless
       of how the run array got restructured in between. */
    {
        const wp_paragraph *p = &sec->paras[start.paragraph];
        u32 rr;

        prefix_len = 0;
        for (rr = 0; rr < start.run; rr++) prefix_len += p->runs[rr].text.length;
        prefix_len += start.offset;
    }

    wp_undo_group_begin(journal);

    if (start.paragraph == end.paragraph) {
        /* Single paragraph: delete run-by-run from the LAST touched run
           back to the first, so earlier (not-yet-processed) runs' own
           indices never shift out from under a later step -- deleting/
           shrinking run R only ever affects indices >= R. */
        for (r = end.run; ; r--) {
            const wp_paragraph *p = &sec->paras[start.paragraph];
            u32 lo = (r == start.run) ? start.offset : 0;
            u32 hi = (r == end.run) ? end.offset : p->runs[r].text.length;

            if (hi > lo) {
                st = wp_edit_delete_range(doc, journal, start.section, start.paragraph,
                                           (u16)r, lo, hi);
                if (st != WP_OK) { wp_undo_group_end(journal); return st; }
            }
            if (r == start.run) break;
        }
    } else {
        u32 first_end_run, first_end_offset;
        u32 mid;

        paragraph_end_run_offset(&sec->paras[start.paragraph], &first_end_run, &first_end_offset);

        /* Tail of the first paragraph: from start to its end. */
        for (r = first_end_run; ; r--) {
            const wp_paragraph *p = &sec->paras[start.paragraph];
            u32 lo = (r == start.run) ? start.offset : 0;
            u32 hi = (r == first_end_run) ? first_end_offset : p->runs[r].text.length;

            if (hi > lo) {
                st = wp_edit_delete_range(doc, journal, start.section, start.paragraph,
                                           (u16)r, lo, hi);
                if (st != WP_OK) { wp_undo_group_end(journal); return st; }
            }
            if (r == start.run) break;
        }

        /* Head of the last paragraph: from its start through end. */
        for (r = end.run; ; r--) {
            const wp_paragraph *p = &sec->paras[end.paragraph];
            u32 hi = (r == end.run) ? end.offset : p->runs[r].text.length;

            if (hi > 0) {
                st = wp_edit_delete_range(doc, journal, end.section, end.paragraph,
                                           (u16)r, 0, hi);
                if (st != WP_OK) { wp_undo_group_end(journal); return st; }
            }
            if (r == 0) break;
        }

        /* Every whole paragraph strictly between start and end: empty
           it out first (journaled per-run delete, so the removal itself
           undoes correctly), THEN merge -- merging an already-empty
           paragraph into its neighbor appends nothing, it just removes
           the now-empty slot. Merging directly (without emptying first)
           would instead SPLICE that paragraph's full content into the
           survivor, the opposite of what a delete needs. */
        for (mid = end.paragraph - 1; mid > start.paragraph; mid--) {
            const wp_paragraph *p = &sec->paras[mid];
            while (p->run_count > 0) {
                u16 last_run = (u16)(p->run_count - 1);
                st = wp_edit_delete_range(doc, journal, start.section, mid, last_run,
                                           0, p->runs[last_run].text.length);
                if (st != WP_OK) { wp_undo_group_end(journal); return st; }
                p = &sec->paras[mid];
            }
        }

        /* Merge start.paragraph forward (end.paragraph - start.paragraph)
           times: each merge absorbs whatever paragraph now immediately
           follows it -- an emptied-out middle one, or on the final
           iteration the trimmed last paragraph. */
        {
            u32 merges = end.paragraph - start.paragraph;
            u32 k;

            for (k = 0; k < merges; k++) {
                st = wp_edit_merge_paragraph(doc, journal, start.section, start.paragraph, NULL);
                if (st != WP_OK) { wp_undo_group_end(journal); return st; }
            }
        }
    }

    wp_undo_group_end(journal);

    if (out_pos) {
        *out_pos = pos_at_byte_offset_in_paragraph(start.section, start.paragraph,
                                                     &sec->paras[start.paragraph], prefix_len);
    }
    return WP_OK;
}

wp_status wp_edit_paste_text(wp_document *doc, wp_undo_journal *journal, wp_pos pos,
                              const u8 *utf8, u32 len, wp_pos *out_pos)
{
    u32 i = 0;
    wp_pos cur = pos;
    wp_status st;

    while (i < len) {
        u32 line_start = i;

        while (i < len && utf8[i] != '\n') i++;
        if (i > line_start) {
            wp_pos out;
            st = wp_edit_insert_text(doc, journal, cur, utf8 + line_start, i - line_start, &out);
            if (st != WP_OK) {
                if (out_pos) *out_pos = cur;
                return st;
            }
            cur = out;
        }
        if (i < len && utf8[i] == '\n') {
            i++;
            /* wp_document_extract_text puts a '\n' AFTER every paragraph
               it joins, including the last one -- so a range ending on a
               paragraph boundary already ends "...text\n", not
               "...text". Splitting on that TRAILING newline too would
               create one extra empty paragraph beyond what was actually
               extracted; only split when there's real content after it. */
            if (i < len) {
                wp_pos out;
                st = wp_edit_split_paragraph(doc, journal, cur, &out);
                if (st != WP_OK) {
                    if (out_pos) *out_pos = cur;
                    return st;
                }
                cur = out;
            }
        }
    }

    if (out_pos) *out_pos = cur;
    return WP_OK;
}

static void apply_single_undo_op(wp_document *doc, wp_op *op)
{
    wp_section *sec = wp_document_section_get(doc, op->at.section);
    wp_paragraph *para = wp_section_paragraph_get(sec, op->at.paragraph);

    if (op->kind == (u8)WP_OP_INSERT_TEXT) {
        u16 idx = find_run_by_span(para, op->text);
        run_remove_at(para, idx);
    } else if (op->kind == (u8)WP_OP_DELETE_TEXT) {
        u16 run_index = (u16)op->at.run;

        switch (op->style_after) {
            case 0: /* whole run removed -- reinsert it */
                run_insert_at(para, run_index, op->text, op->style_before);
                break;
            case 1: /* left survived -- extend length back over the deleted part */
                para->runs[run_index].text.length += op->text.length;
                break;
            case 2: /* right survived -- extend offset/length back over the deleted part */
                para->runs[run_index].text.offset = op->text.offset;
                para->runs[run_index].text.length += op->text.length;
                break;
            default: /* 3: split -- merge the gap back in, drop the extra fragment run */
                para->runs[run_index].text.length +=
                    op->text.length + para->runs[run_index + 1].text.length;
                run_remove_at(para, (u16)(run_index + 1));
                break;
        }
    } else if (op->kind == (u8)WP_OP_SPLIT_PARA) {
        wp_document_merge_paragraph_raw(doc, op->at.section, op->at.paragraph, NULL);
    } else if (op->kind == (u8)WP_OP_MERGE_PARA) {
        wp_paragraph *resurrected;

        wp_document_split_paragraph_raw(doc, op->at, NULL);
        /* Re-split defaults the new paragraph's style to a COPY of the
           survivor's (see docs/editing.md) -- correct for a real Enter
           keypress, but wrong here: the paragraph being resurrected had
           its OWN original style before the merge absorbed it, journaled
           explicitly in style_before since the document alone can't
           recover it after the fact. */
        resurrected = wp_section_paragraph_get(sec, op->at.paragraph + 1);
        wp_paragraph_pstyle_set(resurrected, (wp_pstyle)op->style_before);
    } else if (op->kind == (u8)WP_OP_INSERT_IMAGE) {
        /* Inverse of insert: remove the image paragraph this created (at
           at.paragraph + 1) and merge the split-off tail paragraph back
           into the original -- insert-image is built from split_
           paragraph_raw + one extra paragraph insert, so its inverse is
           the same two-step shape WP_OP_SPLIT_PARA's own undo already
           uses. The registered wp_image_desc stays in doc->images --
           side tables are append-only, same as cstyles/pstyles. */
        paragraph_remove_at(sec, op->at.paragraph + 1);
        wp_document_merge_paragraph_raw(doc, op->at.section, op->at.paragraph, NULL);
    }
}

static wp_status apply_single_redo_op(wp_document *doc, wp_op *op)
{
    wp_section *sec = wp_document_section_get(doc, op->at.section);
    wp_paragraph *para = wp_section_paragraph_get(sec, op->at.paragraph);
    wp_status st = WP_OK;

    if (op->kind == (u8)WP_OP_INSERT_TEXT) {
        wp_pos out_pos;
        st = insert_span_at_pos(doc, op->at, op->text, &out_pos);
    } else if (op->kind == (u8)WP_OP_DELETE_TEXT) {
        /* At this point undo has restored the run to one contiguous
           span at op->at.run; op->text (still in pool coordinates)
           tells us exactly where within it to cut again, regardless of
           which of the four cases this originally was. */
        u16 run_index = (u16)op->at.run;
        u32 start_offset = op->text.offset - para->runs[run_index].text.offset;
        u32 end_offset = start_offset + op->text.length;

        st = wp_document_delete_range_raw(doc, op->at.section, op->at.paragraph,
                                           run_index, start_offset, end_offset);
    } else if (op->kind == (u8)WP_OP_SPLIT_PARA) {
        st = wp_document_split_paragraph_raw(doc, op->at, NULL);
    } else if (op->kind == (u8)WP_OP_MERGE_PARA) {
        st = wp_document_merge_paragraph_raw(doc, op->at.section, op->at.paragraph, NULL);
    } else if (op->kind == (u8)WP_OP_INSERT_IMAGE) {
        /* Redo replays the exact same two-step sequence wp_edit_insert_
           image itself performs, reusing the ALREADY-registered image
           handle from style_before rather than calling wp_document_
           image_add again (which would append a duplicate descriptor). */
        wp_pos split_out;

        st = wp_document_split_paragraph_raw(doc, op->at, &split_out);
        if (st == WP_OK) {
            u32 image_idx;

            st = paragraph_insert_at(sec, split_out.paragraph, &image_idx);
            if (st == WP_OK) {
                wp_paragraph *image_para = wp_section_paragraph_get(sec, image_idx);

                image_para->kind = (u8)WP_PARA_IMAGE;
                image_para->image = (wp_image)op->style_before;
                image_para->style = doc->default_pstyle;
                image_para->layout_dirty = WP_TRUE;
            }
        }
    }

    return st;
}

/* A plain op (not part of any group) is popped, applied, and pushed to
   redo in exactly one iteration -- depth starts and stays at 0, so the
   loop condition is false after the first pass, identical to this
   function's pre-M8.5 behavior. A WP_OP_GROUP_END instead only
   increments depth (no inverse to apply for a marker itself); the loop
   then keeps consuming ops -- applying each one's inverse, and tracking
   further nested GROUP_END/GROUP_BEGIN pairs via the same counter --
   until the GROUP_BEGIN that brings depth back to 0, which is guaranteed
   to be this group's own opening marker (nesting is always well-formed:
   every wp_undo_group_end call has a matching prior wp_undo_group_begin).
   See docs/editing.md's "Undo Grouping" section. */
wp_status wp_edit_undo(wp_document *doc, wp_undo_journal *journal)
{
    wp_op *op;
    int depth = 0;

    if (journal->undo_top == NULL) return WP_ERR;

    do {
        op = journal->undo_top;
        journal->undo_top = op->next;

        if (op->kind == (u8)WP_OP_GROUP_END) {
            depth++;
        } else if (op->kind == (u8)WP_OP_GROUP_BEGIN) {
            depth--;
        } else {
            apply_single_undo_op(doc, op);
        }

        op->next = journal->redo_top;
        journal->redo_top = op;
    } while (depth > 0);

    return WP_OK;
}

/* Mirrors wp_edit_undo: undo pushes a group's markers onto the redo
   stack in the exact order it popped them from the undo stack, which
   (LIFO twice) restores their ORIGINAL chronological push order on the
   redo stack -- so redo can walk it forward, treating GROUP_BEGIN as
   "entering" (depth++) and GROUP_END as "leaving" (depth--), the mirror
   image of undo's roles for the same two markers. */
wp_status wp_edit_redo(wp_document *doc, wp_undo_journal *journal)
{
    wp_op *op;
    int depth = 0;
    wp_status st = WP_OK;

    if (journal->redo_top == NULL) return WP_ERR;

    do {
        op = journal->redo_top;
        journal->redo_top = op->next;

        if (op->kind == (u8)WP_OP_GROUP_BEGIN) {
            depth++;
        } else if (op->kind == (u8)WP_OP_GROUP_END) {
            depth--;
        } else {
            /* If an op inside a group fails, st reflects that failed
               op's status even if later ops in the same group still
               apply -- an accepted, narrow limitation matching this
               journal's existing no-rollback-on-partial-failure
               posture elsewhere (see docs/editing.md). */
            st = apply_single_redo_op(doc, op);
        }

        op->next = journal->undo_top;
        journal->undo_top = op;
    } while (depth > 0);

    return st;
}
