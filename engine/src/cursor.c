#include "editing.h"
#include "utf8.h"

wp_pos wp_pos_doc_start(const wp_document *doc)
{
    wp_pos pos;

    WP_UNUSED(doc);
    pos.section = 0;
    pos.paragraph = 0;
    pos.run = 0;
    pos.offset = 0;
    return pos;
}

/* Position at the very end of a paragraph: immediately after its last
   run's last codepoint, or run=0,offset=0 if the paragraph has no runs
   (an empty paragraph has exactly one valid resting position). */
static wp_pos end_of_paragraph(u32 section, u32 paragraph, const wp_paragraph *para)
{
    wp_pos pos;

    pos.section = section;
    pos.paragraph = paragraph;
    if (para->run_count == 0) {
        pos.run = 0;
        pos.offset = 0;
    } else {
        pos.run = (u32)(para->run_count - 1);
        pos.offset = para->runs[pos.run].text.length;
    }
    return pos;
}

static wp_pos end_of_section(u32 section, const wp_section *sec)
{
    wp_pos pos;

    if (sec->para_count == 0) {
        pos.section = section;
        pos.paragraph = 0;
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }
    {
        u32 last_para = sec->para_count - 1;
        return end_of_paragraph(section, last_para, &sec->paras[last_para]);
    }
}

/* Used only by wp_pos_prev when retreating INTO a paragraph from after
   it (crossing a paragraph or section boundary backward). Lands on the
   START of the paragraph's last run's LAST codepoint -- not at that
   run's raw length -- so this is the true inverse of how wp_pos_next
   crosses forward into a paragraph's first run. See the comment in
   wp_pos_prev for why this must not reuse end_of_paragraph. */
static wp_pos retreat_into_paragraph(const wp_document *doc, u32 section,
                                      u32 paragraph, const wp_paragraph *para)
{
    wp_pos pos;

    pos.section = section;
    pos.paragraph = paragraph;

    if (para->run_count == 0) {
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }
    {
        const wp_run *last_run = &para->runs[para->run_count - 1];
        const u8 *bytes = wp_strpool_get(&doc->strings, last_run->text);

        pos.run = (u32)(para->run_count - 1);
        pos.offset = utf8_prev(bytes, last_run->text.length, last_run->text.length);
        return pos;
    }
}

wp_pos wp_pos_doc_end(const wp_document *doc)
{
    if (doc->section_count == 0) {
        wp_pos pos;
        pos.section = 0;
        pos.paragraph = 0;
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }
    {
        u32 last = doc->section_count - 1;
        return end_of_section(last, &doc->sections[last]);
    }
}

wp_pos wp_pos_next(const wp_document *doc, wp_pos pos)
{
    const wp_section *sec;
    const wp_paragraph *para;

    if (pos.section >= doc->section_count) return pos;
    sec = &doc->sections[pos.section];

    if (pos.paragraph >= sec->para_count) return pos;
    para = &sec->paras[pos.paragraph];

    if (pos.run < para->run_count) {
        const wp_run *run = &para->runs[pos.run];
        const u8 *bytes = wp_strpool_get(&doc->strings, run->text);
        u32 new_offset = utf8_next(bytes, run->text.length, pos.offset);
        wp_bool is_last_run_overall =
            (wp_bool)((pos.run + 1 >= para->run_count) &&
                      (pos.paragraph + 1 >= sec->para_count) &&
                      (pos.section + 1 >= doc->section_count));

        /* Stay at new_offset if we're still strictly within this run, OR
           this is the very last run of the very last paragraph of the
           very last section and we made progress -- that lands exactly
           at wp_pos_doc_end and must not be discarded just because
           there's no further run to cross into. */
        if (new_offset < run->text.length ||
            (is_last_run_overall && new_offset > pos.offset)) {
            pos.offset = new_offset;
            return pos;
        }
        /* reached the end of this run -- try the next run in this paragraph */
        if ((u32)(pos.run + 1) < para->run_count) {
            pos.run++;
            pos.offset = 0;
            return pos;
        }
    }

    if (pos.paragraph + 1 < sec->para_count) {
        pos.paragraph++;
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }

    if (pos.section + 1 < doc->section_count) {
        pos.section++;
        pos.paragraph = 0;
        pos.run = 0;
        pos.offset = 0;
        return pos;
    }

    return pos; /* already at the absolute end of the document */
}

wp_pos wp_pos_prev(const wp_document *doc, wp_pos pos)
{
    const wp_section *sec;
    const wp_paragraph *para;

    if (pos.section >= doc->section_count) return pos;
    sec = &doc->sections[pos.section];
    if (pos.paragraph >= sec->para_count) return pos;
    para = &sec->paras[pos.paragraph];

    if (pos.run < para->run_count && pos.offset > 0) {
        const wp_run *run = &para->runs[pos.run];
        const u8 *bytes = wp_strpool_get(&doc->strings, run->text);
        pos.offset = utf8_prev(bytes, run->text.length, pos.offset);
        return pos;
    }

    /* offset is 0 (or the paragraph is empty) -- retreat across a
       run/paragraph/section boundary. Crucially, this must land on the
       START of the previous run's LAST codepoint (one utf8_prev step
       back from its raw length), not at offset == run->text.length:
       next() only ever visits that raw-length offset at the absolute
       end of the document (see wp_pos_next) -- everywhere else it
       crosses straight to the start of the following run/paragraph/
       section instead. Landing at the raw length here would desync
       prev() from being next()'s true inverse. utf8_prev naturally
       clamps to 0 for a zero-length run, so this same formula is
       correct whether or not the previous run/paragraph is empty --
       matching how next()'s crossing lands unconditionally on the
       target's first run without checking for emptiness either. */
    if (pos.run > 0) {
        const wp_run *prev_run = &para->runs[pos.run - 1];
        const u8 *bytes = wp_strpool_get(&doc->strings, prev_run->text);
        pos.run--;
        pos.offset = utf8_prev(bytes, prev_run->text.length, prev_run->text.length);
        return pos;
    }

    if (pos.paragraph > 0) {
        u32 prev_para = pos.paragraph - 1;
        return retreat_into_paragraph(doc, pos.section, prev_para, &sec->paras[prev_para]);
    }

    if (pos.section > 0) {
        u32 prev_section = pos.section - 1;
        const wp_section *prev_sec = &doc->sections[prev_section];

        if (prev_sec->para_count == 0) {
            pos.section = prev_section;
            pos.paragraph = 0;
            pos.run = 0;
            pos.offset = 0;
            return pos;
        }
        {
            u32 last_para = prev_sec->para_count - 1;
            return retreat_into_paragraph(doc, prev_section, last_para,
                                           &prev_sec->paras[last_para]);
        }
    }

    return pos; /* already at the absolute start of the document */
}
