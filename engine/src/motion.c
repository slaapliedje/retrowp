#include "editing.h"
#include "edit_internal.h"
#include "utf8.h"

wp_wordclass wp_wordclass_of(u32 codepoint)
{
    if (codepoint == ' ' || codepoint == '\t' || codepoint == '\r') return WP_WORDCLASS_BLANK;
    if ((codepoint >= 'a' && codepoint <= 'z') ||
        (codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= '0' && codepoint <= '9') ||
        codepoint == '_' || codepoint >= 128) {
        return WP_WORDCLASS_WORD;
    }
    return WP_WORDCLASS_PUNCT;
}

/* Returns the codepoint immediately at pos -- what wp_pos_next(pos)
   would consume -- without moving pos. A position is either strictly
   within a run's content (direct decode) or, by the canonical-position
   convention every other function in this file relies on (see
   docs/editing.md), at the very end of its paragraph -- in which case
   the next real character, if any, is at the start of a following
   paragraph/section. Returns 0 at the absolute end of the document, or
   0 on a decode failure (never guesses a replacement -- matches
   utf8_decode's own contract). */
static u32 codepoint_at(const wp_document *doc, wp_pos pos)
{
    for (;;) {
        const wp_section *sec;
        const wp_paragraph *para;

        if (pos.section >= doc->section_count) return 0;
        sec = &doc->sections[pos.section];
        if (pos.paragraph >= sec->para_count) return 0;
        para = &sec->paras[pos.paragraph];

        if (pos.run < para->run_count && pos.offset < para->runs[pos.run].text.length) {
            const wp_run *run = &para->runs[pos.run];
            const u8 *bytes = wp_strpool_get(&doc->strings, run->text);
            u32 cp;

            if (utf8_decode(bytes + pos.offset, run->text.length - pos.offset, &cp) == 0) return 0;
            return cp;
        }

        if (pos.paragraph + 1 < sec->para_count) {
            pos.paragraph++;
            pos.run = 0;
            pos.offset = 0;
        } else if (pos.section + 1 < doc->section_count) {
            pos.section++;
            pos.paragraph = 0;
            pos.run = 0;
            pos.offset = 0;
        } else {
            return 0; /* absolute end of document */
        }
    }
}

static wp_wordclass classify(u32 cp, wp_bool big)
{
    wp_wordclass c = wp_wordclass_of(cp);
    if (big && c != WP_WORDCLASS_BLANK) c = WP_WORDCLASS_WORD;
    return c;
}

/* Steps pos forward one codepoint at a time via wp_pos_next while
   codepoint_at(pos) keeps classifying as `cls`, stopping at the first
   position whose character differs (or at document end). Crosses
   paragraph boundaries freely -- correct for "skip blanks" (a paragraph
   break behaves like just another blank, matching how consecutive blank
   *lines* get skipped through in real vim), but NOT correct for "skip
   the rest of the current word" -- see skip_word_run_forward below for
   why that needs a different stopping rule. */
static wp_pos skip_while_class(const wp_document *doc, wp_pos pos, wp_wordclass cls, wp_bool big)
{
    for (;;) {
        u32 cp = codepoint_at(doc, pos);
        wp_pos next;

        if (cp == 0 || classify(cp, big) != cls) return pos;
        next = wp_pos_next(doc, pos);
        if (wp_pos_compare(next, pos) == 0) return pos; /* doc end, no progress possible */
        pos = next;
    }
}

/* Like skip_while_class, but a word/punct run never spans a paragraph
   boundary -- even if the next paragraph happens to start with a
   character of the identical class, crossing into it always ends the
   current run (matching real vim: words don't span lines). Lands
   exactly at the crossing point (the next paragraph's start), which is
   the correct outcome for wp_pos_word_forward (an implicit "blank" the
   subsequent skip_while_class(BLANK) call then evaluates on its own
   terms) but NOT for wp_pos_word_end, which must stop one step earlier
   -- see its own boundary check below instead of using this helper. */
static wp_pos skip_word_run_forward(const wp_document *doc, wp_pos pos, wp_wordclass cls, wp_bool big)
{
    for (;;) {
        u32 cp = codepoint_at(doc, pos);
        wp_pos next;

        if (cp == 0 || classify(cp, big) != cls) return pos;
        next = wp_pos_next(doc, pos);
        if (wp_pos_compare(next, pos) == 0) return pos;
        if (next.paragraph != pos.paragraph || next.section != pos.section) return next;
        pos = next;
    }
}

wp_pos wp_pos_word_forward(const wp_document *doc, wp_pos pos, wp_bool big)
{
    u32 cp = codepoint_at(doc, pos);

    if (cp == 0) return pos;
    pos = skip_word_run_forward(doc, pos, classify(cp, big), big);
    return skip_while_class(doc, pos, WP_WORDCLASS_BLANK, big);
}

wp_pos wp_pos_word_backward(const wp_document *doc, wp_pos pos, wp_bool big)
{
    wp_pos prev = wp_pos_prev(doc, pos);
    wp_wordclass cls;
    u32 cp;

    if (wp_pos_compare(prev, pos) == 0) return pos; /* already at doc start */
    pos = prev;

    /* Blanks freely cross paragraph boundaries backward too. */
    for (;;) {
        cp = codepoint_at(doc, pos);
        if (cp == 0 || classify(cp, big) != WP_WORDCLASS_BLANK) break;
        prev = wp_pos_prev(doc, pos);
        if (wp_pos_compare(prev, pos) == 0) return pos; /* doc start */
        pos = prev;
    }

    cp = codepoint_at(doc, pos);
    if (cp == 0) return pos;
    cls = classify(cp, big);

    /* But the word/punct run itself does not -- same reasoning as
       skip_word_run_forward, mirrored. */
    for (;;) {
        prev = wp_pos_prev(doc, pos);
        if (wp_pos_compare(prev, pos) == 0) return pos; /* doc start */
        if (prev.paragraph != pos.paragraph || prev.section != pos.section) return pos;
        cp = codepoint_at(doc, prev);
        if (cp == 0 || classify(cp, big) != cls) return pos; /* pos is this word's first char */
        pos = prev;
    }
}

wp_pos wp_pos_word_end(const wp_document *doc, wp_pos pos, wp_bool big)
{
    wp_pos next = wp_pos_next(doc, pos);
    wp_wordclass cls;
    u32 cp;

    if (wp_pos_compare(next, pos) == 0) return pos; /* already at doc end */
    pos = next;

    /* Blanks freely cross paragraph boundaries. */
    for (;;) {
        cp = codepoint_at(doc, pos);
        if (cp == 0) return pos;
        if (classify(cp, big) != WP_WORDCLASS_BLANK) break;
        next = wp_pos_next(doc, pos);
        if (wp_pos_compare(next, pos) == 0) return pos;
        pos = next;
    }

    cls = classify(codepoint_at(doc, pos), big);

    /* The word/punct run does not -- stop one step short of crossing
       (pos is already "immediately after this word's last character,"
       this engine's insertion-point caret convention, not vim's
       on-character one -- see docs/vimode.md), rather than crossing
       into the next paragraph the way skip_word_run_forward would. */
    for (;;) {
        next = wp_pos_next(doc, pos);
        if (wp_pos_compare(next, pos) == 0) return next; /* doc end mid-word */
        if (next.paragraph != pos.paragraph || next.section != pos.section) return pos;
        cp = codepoint_at(doc, next);
        if (cp == 0 || classify(cp, big) != cls) return next;
        pos = next;
    }
}

wp_pos wp_pos_line_start(const wp_document *doc, wp_pos pos)
{
    wp_pos out;

    WP_UNUSED(doc);
    out.section = pos.section;
    out.paragraph = pos.paragraph;
    out.run = 0;
    out.offset = 0;
    return out;
}

wp_pos wp_pos_line_end(const wp_document *doc, wp_pos pos)
{
    const wp_section *sec;
    const wp_paragraph *para;
    wp_pos out;

    out.section = pos.section;
    out.paragraph = pos.paragraph;
    out.run = 0;
    out.offset = 0;

    if (pos.section >= doc->section_count) return out;
    sec = &doc->sections[pos.section];
    if (pos.paragraph >= sec->para_count) return out;
    para = &sec->paras[pos.paragraph];
    paragraph_end_run_offset(para, &out.run, &out.offset);
    return out;
}

wp_pos wp_pos_line_first_nonblank(const wp_document *doc, wp_pos pos)
{
    wp_pos out = wp_pos_line_start(doc, pos);
    wp_pos line_end = wp_pos_line_end(doc, pos);

    while (wp_pos_compare(out, line_end) < 0) {
        u32 cp = codepoint_at(doc, out);
        if (cp == 0 || wp_wordclass_of(cp) != WP_WORDCLASS_BLANK) return out;
        out = wp_pos_next(doc, out);
    }
    return line_end; /* all-blank (or empty) line: land at its end */
}

wp_pos wp_pos_paragraph_down(const wp_document *doc, wp_pos pos)
{
    const wp_section *sec;
    wp_pos out;

    if (pos.section >= doc->section_count) return pos;
    sec = &doc->sections[pos.section];
    if (pos.paragraph + 1 >= sec->para_count) return pos;

    out.section = pos.section;
    out.paragraph = pos.paragraph + 1;
    out.run = 0;
    out.offset = 0;
    return out;
}

wp_pos wp_pos_paragraph_up(const wp_document *doc, wp_pos pos)
{
    wp_pos out;

    WP_UNUSED(doc);
    if (pos.paragraph == 0) return pos;

    out.section = pos.section;
    out.paragraph = pos.paragraph - 1;
    out.run = 0;
    out.offset = 0;
    return out;
}

wp_pos wp_pos_para_block_forward(const wp_document *doc, wp_pos pos)
{
    const wp_section *sec;
    u32 p;

    if (pos.section >= doc->section_count) return pos;
    sec = &doc->sections[pos.section];

    for (p = pos.paragraph + 1; p < sec->para_count; p++) {
        if (sec->paras[p].run_count == 0) {
            wp_pos out;
            out.section = pos.section;
            out.paragraph = p;
            out.run = 0;
            out.offset = 0;
            return out;
        }
    }
    /* No empty paragraph ahead in this section -- land at the document's
       end. Exactly correct for the (only real-world) single-section
       case; a later section's content is not scanned, matching this
       whole pass's single-section scope elsewhere. */
    return wp_pos_doc_end(doc);
}

wp_pos wp_pos_para_block_backward(const wp_document *doc, wp_pos pos)
{
    const wp_section *sec;
    u32 p;

    if (pos.section >= doc->section_count) return pos;
    sec = &doc->sections[pos.section];
    if (pos.paragraph == 0) return wp_pos_doc_start(doc);

    p = pos.paragraph - 1;
    for (;;) {
        if (sec->paras[p].run_count == 0) {
            wp_pos out;
            out.section = pos.section;
            out.paragraph = p;
            out.run = 0;
            out.offset = 0;
            return out;
        }
        if (p == 0) break;
        p--;
    }
    return wp_pos_doc_start(doc);
}
