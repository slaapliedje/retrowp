/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "notes.h"
#include "styles.h"
#include "strpool.h"
#include "platform.h"

#define NOTES_MAX 2048u   /* ids numbered at once; past it a note shows "*" */

static u16 run_note(const wp_document *doc, const wp_run *run)
{
    const wp_char_style *cs = wp_document_cstyle_get(doc, run->style);
    return cs != NULL ? cs->note : 0;
}

u16 wp_notes_new_id(const wp_document *doc)
{
    u32 si, pi;
    u16 ri, top = 0;
    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            if (para->note > top) top = para->note;
            for (ri = 0; ri < para->run_count; ri++) {
                u16 n = run_note(doc, &para->runs[ri]);
                if (n > top) top = n;
            }
        }
    }
    return (u16)(top < 0xFFFFu ? top + 1u : top);
}

/* A run's text made `want` where it is not already. */
static wp_status set_run_text(wp_document *doc, wp_paragraph *para, u16 ri, const char *want)
{
    u32 n = (u32)strlen(want);
    wp_run *run = wp_paragraph_run_get(para, ri);
    wp_span span;
    wp_status st;

    if (run->text.length == n && memcmp(wp_strpool_get(&doc->strings, run->text), want, n) == 0) return WP_OK;
    if (run->text.length == n && run->text.offset != 0) {
        /* The same width: the digits rewritten where they are, so the run
           keeps its span -- which is how Undo finds the run it inserted.
           A reference's text is its own (no other run shares it). */
        memcpy(doc->strings.bytes + run->text.offset, want, n);
        para->layout_dirty = WP_TRUE;
        return WP_OK;
    }
    st = wp_strpool_append(&doc->strings, (const u8 *)want, n, &span);
    if (st != WP_OK) return st;
    wp_paragraph_run_get(para, ri)->text = span;
    para->layout_dirty = WP_TRUE;
    return WP_OK;
}

static void number_text(u32 n, char *out)
{
    char d[12];
    u32 k = 0, i = 0;
    do { d[k++] = (char)('0' + n % 10u); n /= 10u; } while (n > 0 && k < 11);
    while (k > 0) out[i++] = d[--k];
    out[i] = '\0';
}

wp_status wp_notes_renumber(wp_document *doc)
{
    u16 *number;
    u32 si, pi, next = 1;
    u16 ri;
    wp_status st = WP_OK;
    wp_bool any = WP_FALSE;

    /* Nothing to do in a document without notes -- the usual case. */
    for (si = 0; si < doc->section_count && !any; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count && !any; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            if (para->note != 0) any = WP_TRUE;
            for (ri = 0; ri < para->run_count && !any; ri++) if (run_note(doc, &para->runs[ri]) != 0) any = WP_TRUE;
        }
    }
    if (!any) return WP_OK;
    number = (u16 *)mem_alloc(NOTES_MAX * (u32)sizeof(u16));
    if (number == NULL) return WP_NOMEM;
    memset(number, 0, NOTES_MAX * sizeof(u16));

    /* References, in reading order, outside the notes themselves. */
    for (si = 0; si < doc->section_count && st == WP_OK; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        for (pi = 0; pi < sec->para_count && st == WP_OK; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            if (para->note != 0) continue;
            for (ri = 0; ri < para->run_count && st == WP_OK; ri++) {
                u16 id = run_note(doc, wp_paragraph_run_get(para, ri));
                char t[12];
                if (id == 0) continue;
                if (id < NOTES_MAX && number[id] == 0) number[id] = (u16)next++;
                if (id < NOTES_MAX) number_text(number[id], t); else strcpy(t, "*");
                st = set_run_text(doc, para, ri, t);
                sec = wp_document_section_get(doc, si);
                para = wp_section_paragraph_get(sec, pi);
            }
        }
    }
    /* Each note's own number: its first run, where that names the note. */
    for (si = 0; si < doc->section_count && st == WP_OK; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        for (pi = 0; pi < sec->para_count && st == WP_OK; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            char t[12];
            if (para->note == 0 || para->run_count == 0) continue;
            if (run_note(doc, wp_paragraph_run_get(para, 0)) != para->note) continue;
            if (para->note < NOTES_MAX && number[para->note] != 0) number_text(number[para->note], t);
            else strcpy(t, "*");
            st = set_run_text(doc, para, 0, t);
        }
    }
    mem_free(number);
    return st;
}

/* The character format of a reference to note `id`: the text's own at
   `pos`, raised and naming the note. */
static wp_status ref_cstyle(wp_document *doc, wp_cstyle base, u16 id, wp_cstyle *out)
{
    const wp_char_style *b = wp_document_cstyle_get(doc, base);
    wp_char_style cs;
    if (b == NULL) return WP_ERR;
    cs = *b;
    cs.flags = (u16)((cs.flags | WP_STYLE_SUPER) & ~WP_STYLE_SUB);
    cs.note = id;
    return wp_document_cstyle_find_or_add(doc, &cs, out);
}

wp_status wp_notes_insert(wp_document *doc, wp_undo_journal *journal, wp_pos pos, wp_pos *note_pos)
{
    u16 id = wp_notes_new_id(doc);
    u16 style_id;
    wp_pos after, end;
    wp_section *sec;
    wp_paragraph *para;
    wp_cstyle refstyle;
    u32 last;
    wp_status st;

    if (pos.section >= doc->section_count) return WP_RANGE;
    sec = wp_document_section_get(doc, pos.section);
    if (pos.paragraph >= sec->para_count) return WP_RANGE;
    if (wp_section_paragraph_get(sec, pos.paragraph)->note != 0) return WP_UNSUPPORTED;   /* no note in a note */

    /* The reference: a number at pos, raised and naming the note. */
    st = wp_edit_insert_text(doc, journal, pos, (const u8 *)"0", 1, &after);
    if (st != WP_OK) return st;
    sec = wp_document_section_get(doc, pos.section);
    para = wp_section_paragraph_get(sec, after.paragraph);
    st = ref_cstyle(doc, wp_paragraph_run_get(para, (u16)after.run)->style, id, &refstyle);
    if (st != WP_OK) return st;
    wp_run_cstyle_set(para, (u16)after.run, refstyle);
    para->layout_dirty = WP_TRUE;

    /* The note: a paragraph after this one and the notes that follow it. */
    last = after.paragraph;
    while (last + 1 < sec->para_count && wp_section_paragraph_get(sec, last + 1)->note != 0) last++;
    para = wp_section_paragraph_get(sec, last);
    memset(&end, 0, sizeof(end));
    end.section = pos.section;
    end.paragraph = last;
    end.run = para->run_count > 0 ? (u32)para->run_count - 1u : 0;
    end.offset = para->run_count > 0 ? wp_paragraph_run_get(para, (u16)end.run)->text.length : 0;
    st = wp_edit_split_paragraph(doc, journal, end, &end);
    if (st != WP_OK) return st;
    sec = wp_document_section_get(doc, pos.section);
    para = wp_section_paragraph_get(sec, end.paragraph);
    para->note = id;
    para->tflags = 0;
    para->trow = 0;
    para->frame = 0;
    if (wp_style_ensure(doc, (const u8 *)"Footnote Text", 13, &style_id) == WP_OK) {
        sec = wp_document_section_get(doc, pos.section);
        para = wp_section_paragraph_get(sec, end.paragraph);
        wp_paragraph_pstyle_set(para, doc->named[style_id].para);
    }
    /* Its number and a space -- the space typed first, then the number
       before it as a run of its own, raised and naming the note: two
       journaled inserts, nothing split, so Undo finds each run it made. */
    st = wp_edit_insert_text(doc, journal, end, (const u8 *)" ", 1, &after);
    if (st != WP_OK) return st;
    st = wp_edit_insert_text(doc, journal, end, (const u8 *)"0", 1, &after);
    if (st != WP_OK) return st;
    para = wp_section_paragraph_get(wp_document_section_get(doc, pos.section), end.paragraph);
    if (para->run_count < 2) return WP_ERR;
    st = ref_cstyle(doc, wp_paragraph_run_get(para, 0)->style, id, &refstyle);
    if (st != WP_OK) return st;
    wp_run_cstyle_set(para, 0, refstyle);
    para->layout_dirty = WP_TRUE;
    if (note_pos != NULL) {
        memset(note_pos, 0, sizeof(*note_pos));
        note_pos->section = pos.section;
        note_pos->paragraph = end.paragraph;
        note_pos->run = 1;
        note_pos->offset = 1;
    }
    return wp_notes_renumber(doc);
}
