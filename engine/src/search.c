/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "editing.h"
#include "edit_internal.h"

/* M11: the shared find/replace primitive every keybinding scheme's own
   Find/Replace is built on. Promoted from vi-mode's own wp_vimode_search
   (see vimode.c's thin wrapper) -- this is the real generalization that
   function's own doc comment already predicted ("independently reusable
   by a future non-modal Find dialog"): real backward search (previously
   accepted but ignored), plus ignore_case and whole_word, both needed by
   more than one scheme (WordStar's U/W options, vim's `i` flag). */

static u8 ascii_tolower(u8 c)
{
    return (c >= 'A' && c <= 'Z') ? (u8)(c - 'A' + 'a') : c;
}

static wp_bool bytes_equal(const u8 *a, const u8 *b, u32 len, wp_bool ignore_case)
{
    u32 i;

    if (!ignore_case) return (wp_bool)(memcmp(a, b, len) == 0);
    for (i = 0; i < len; i++) {
        if (ascii_tolower(a[i]) != ascii_tolower(b[i])) return WP_FALSE;
    }
    return WP_TRUE;
}

/* whole_word check: the byte immediately before/after the match (if any)
   must not itself be a WORD-class byte -- a paragraph boundary always
   satisfies the boundary condition, matching real editors' own "start/
   end of line counts as a word boundary" behavior. ASCII-only, same
   scope as wp_wordclass_of's own documented simplification. */
static wp_bool is_word_boundary_match(const u8 *scratch, u32 len, u32 match_start, u32 needle_len)
{
    if (match_start > 0 && wp_wordclass_of((u32)scratch[match_start - 1]) == WP_WORDCLASS_WORD) {
        return WP_FALSE;
    }
    if (match_start + needle_len < len &&
        wp_wordclass_of((u32)scratch[match_start + needle_len]) == WP_WORDCLASS_WORD) {
        return WP_FALSE;
    }
    return WP_TRUE;
}

/* Extracts paragraph (si, pi)'s flattened text into scratch (capped at
   cap bytes, same documented MVP scratch-buffer limitation
   wp_vimode_search's own paragraph scan always had). Returns the actual
   (possibly truncated) length. */
static u32 extract_paragraph(const wp_document *doc, u32 si, u32 pi, u8 *scratch, u32 cap)
{
    const wp_paragraph *para = &doc->sections[si].paras[pi];
    wp_selection sel;
    u32 total_len;

    sel.anchor.section = si; sel.anchor.paragraph = pi; sel.anchor.run = 0; sel.anchor.offset = 0;
    sel.caret.section = si; sel.caret.paragraph = pi;
    paragraph_end_run_offset(para, &sel.caret.run, &sel.caret.offset);
    wp_document_extract_text(doc, sel, scratch, cap, &total_len);
    return (total_len < cap) ? total_len : cap;
}

#define WP_SEARCH_SCRATCH_CAP 512

static wp_status find_forward(const wp_document *doc, wp_pos from, const u8 *needle,
                               u32 needle_len, wp_bool ignore_case, wp_bool whole_word,
                               wp_pos *out_pos)
{
    u32 si = from.section, pi = from.paragraph;
    wp_bool first = WP_TRUE;

    for (;;) {
        const wp_section *sec = &doc->sections[si];

        if (pi < sec->para_count) {
            const wp_paragraph *para = &sec->paras[pi];
            u8 scratch[WP_SEARCH_SCRATCH_CAP];
            u32 len = extract_paragraph(doc, si, pi, scratch, (u32)sizeof(scratch));
            u32 min_start = 0;

            if (first && si == from.section && pi == from.paragraph) {
                u32 rr;
                for (rr = 0; rr < from.run; rr++) min_start += para->runs[rr].text.length;
                min_start += from.offset + 1;
            }

            if (len >= needle_len && min_start <= len - needle_len) {
                u32 s;
                for (s = min_start; s <= len - needle_len; s++) {
                    if (bytes_equal(scratch + s, needle, needle_len, ignore_case) &&
                        (!whole_word || is_word_boundary_match(scratch, len, s, needle_len))) {
                        *out_pos = pos_from_byte_in_paragraph(si, pi, para, s);
                        return WP_OK;
                    }
                }
            }
        }
        first = WP_FALSE;

        if (pi + 1 < sec->para_count) {
            pi++;
        } else if (si + 1 < doc->section_count) {
            si++;
            pi = 0;
        } else {
            si = 0;
            pi = 0;
        }

        if (si == from.section && pi == from.paragraph) break;
    }

    return WP_ERR;
}

static wp_status find_backward(const wp_document *doc, wp_pos from, const u8 *needle,
                                u32 needle_len, wp_bool ignore_case, wp_bool whole_word,
                                wp_pos *out_pos)
{
    u32 si = from.section, pi = from.paragraph;
    wp_bool first = WP_TRUE;

    for (;;) {
        const wp_section *sec = &doc->sections[si];

        if (pi < sec->para_count) {
            const wp_paragraph *para = &sec->paras[pi];
            u8 scratch[WP_SEARCH_SCRATCH_CAP];
            u32 len = extract_paragraph(doc, si, pi, scratch, (u32)sizeof(scratch));
            wp_bool bounded = (wp_bool)(first && si == from.section && pi == from.paragraph);
            u32 max_start = 0; /* exclusive upper bound on match start, only when bounded */

            if (bounded) {
                u32 rr;
                for (rr = 0; rr < from.run; rr++) max_start += para->runs[rr].text.length;
                max_start += from.offset;
            }

            if (len >= needle_len) {
                u32 limit = bounded ? max_start : (len - needle_len + 1);
                u32 s;
                wp_bool found = WP_FALSE;
                u32 found_at = 0;

                for (s = 0; s < limit && s <= len - needle_len; s++) {
                    if (bytes_equal(scratch + s, needle, needle_len, ignore_case) &&
                        (!whole_word || is_word_boundary_match(scratch, len, s, needle_len))) {
                        found = WP_TRUE;
                        found_at = s; /* keep scanning -- want the LAST (rightmost) match */
                    }
                }
                if (found) {
                    *out_pos = pos_from_byte_in_paragraph(si, pi, para, found_at);
                    return WP_OK;
                }
            }
        }
        first = WP_FALSE;

        if (pi > 0) {
            pi--;
        } else if (si > 0) {
            si--;
            pi = doc->sections[si].para_count > 0 ? doc->sections[si].para_count - 1 : 0;
        } else {
            si = doc->section_count > 0 ? doc->section_count - 1 : 0;
            pi = doc->sections[si].para_count > 0 ? doc->sections[si].para_count - 1 : 0;
        }

        if (si == from.section && pi == from.paragraph) break;
    }

    return WP_ERR;
}

wp_status wp_document_find_text(const wp_document *doc, wp_pos from,
                                 const u8 *needle, u32 needle_len,
                                 wp_bool forward, wp_bool ignore_case,
                                 wp_bool whole_word, wp_pos *out_pos)
{
    if (needle_len == 0 || from.section >= doc->section_count) return WP_ERR;
    if (doc->sections[from.section].para_count == 0) return WP_ERR;

    return forward
        ? find_forward(doc, from, needle, needle_len, ignore_case, whole_word, out_pos)
        : find_backward(doc, from, needle, needle_len, ignore_case, whole_word, out_pos);
}

wp_selection wp_document_match_selection(const wp_document *doc, wp_pos start, u32 needle_len)
{
    const wp_paragraph *para = &doc->sections[start.section].paras[start.paragraph];
    wp_selection sel;
    u32 start_byte = 0;
    u32 r;

    for (r = 0; r < start.run; r++) start_byte += para->runs[r].text.length;
    start_byte += start.offset;

    sel.anchor = start;
    sel.caret = pos_from_byte_in_paragraph(start.section, start.paragraph, para,
                                            start_byte + needle_len);
    return sel;
}
