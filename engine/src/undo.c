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

wp_status wp_edit_undo(wp_document *doc, wp_undo_journal *journal)
{
    wp_op *op;
    wp_section *sec;
    wp_paragraph *para;

    if (journal->undo_top == NULL) return WP_ERR;
    op = journal->undo_top;
    journal->undo_top = op->next;

    sec = wp_document_section_get(doc, op->at.section);
    para = wp_section_paragraph_get(sec, op->at.paragraph);

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
    }

    op->next = journal->redo_top;
    journal->redo_top = op;
    return WP_OK;
}

wp_status wp_edit_redo(wp_document *doc, wp_undo_journal *journal)
{
    wp_op *op;
    wp_section *sec;
    wp_paragraph *para;
    wp_status st = WP_OK;

    if (journal->redo_top == NULL) return WP_ERR;
    op = journal->redo_top;
    journal->redo_top = op->next;

    sec = wp_document_section_get(doc, op->at.section);
    para = wp_section_paragraph_get(sec, op->at.paragraph);

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
    }

    op->next = journal->undo_top;
    journal->undo_top = op;
    return st;
}
