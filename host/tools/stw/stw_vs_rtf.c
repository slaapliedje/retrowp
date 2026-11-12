/* SPDX-License-Identifier: GPL-2.0-or-later */
/* stw_vs_rtf: one Atari Works document read two ways -- its .STW through
   RetroWP's importer, the .RTF Atari Works itself exported through
   RetroWP's RTF reader -- and compared: each paragraph's alignment, and
   each visible character's bold, italic, underline and size. Prints one
   line of counts for host/tools/stw/stw_score.py --style. Text is matched
   character by character across both documents' visible characters
   (spaces and tabs skipped), so a difference in wrapping or spacing does
   not shift what is compared. */
#include <stdio.h>
#include <stdlib.h>
#include "format_stw.h"
#include "format_rtf.h"
#include "strpool.h"
#include "utf8.h"

typedef struct { u32 cp; u16 flags; i32 size; u8 align; } vis;

static u8 *slurp(const char *path, u32 *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    u8 *b;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = (u8 *)malloc((size_t)n + 1);
    if (b != NULL && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (u32)n;
    return b;
}

/* Every visible character with its style and its paragraph's alignment. */
static u32 flatten(wp_document *doc, vis *out, u32 cap)
{
    u32 n = 0, si, pi;
    u16 ri;
    for (si = 0; si < doc->section_count; si++) {
        wp_section *sec = wp_document_section_get(doc, si);
        for (pi = 0; pi < sec->para_count; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            const wp_para_style *ps = wp_document_pstyle_get(doc, para->style);
            for (ri = 0; ri < para->run_count; ri++) {
                wp_run *run = wp_paragraph_run_get(para, ri);
                const wp_char_style *cs = wp_document_cstyle_get(doc, run->style);
                const u8 *t = wp_strpool_get(&doc->strings, run->text);
                u32 i = 0, k, cp;
                while (t != NULL && i < run->text.length) {
                    k = utf8_decode(t + i, run->text.length - i, &cp);
                    if (k == 0) k = 1;
                    i += k;
                    if (cp == ' ' || cp == '\t' || cp == 0xA0) continue;
                    if (n < cap) {
                        out[n].cp = cp;
                        out[n].flags = cs != NULL ? cs->flags : 0;
                        out[n].size = cs != NULL ? cs->size_twips : 0;
                        out[n].align = ps != NULL ? ps->align : 0;
                    }
                    n++;
                }
            }
        }
    }
    return n < cap ? n : cap;
}

int main(int argc, char **argv)
{
    static vis a[200000], b[200000];
    u8 *sb, *rb;
    u32 sl, rl, na, nb, i, same_text = 0, al = 0, bo = 0, it = 0, ul = 0, sz = 0, shown = 0;
    wp_document sd, rd;

    if (argc != 3) { fprintf(stderr, "usage: stw_vs_rtf FILE.STW FILE.RTF\n"); return 2; }
    sb = slurp(argv[1], &sl);
    rb = slurp(argv[2], &rl);
    if (sb == NULL || rb == NULL) return 2;
    if (format_stw_import(sb, sl, &sd) != WP_OK || format_rtf_import(rb, rl, &rd) != WP_OK) return 1;
    na = flatten(&sd, a, 200000);
    nb = flatten(&rd, b, 200000);
    for (i = 0; i < na && i < nb; i++) {
        if (a[i].cp != b[i].cp) break;   /* compare only while the text agrees */
        same_text++;
        al += a[i].align == b[i].align;
        bo += ((a[i].flags ^ b[i].flags) & WP_STYLE_BOLD) == 0;
        if (getenv("STW_DEBUG") != NULL && ((a[i].flags ^ b[i].flags) & (getenv("STW_DEBUG")[0] == 'u' ? WP_STYLE_UNDERLINE : WP_STYLE_BOLD)) != 0 && shown++ < 12) {
            fprintf(stderr, "at %lu '%c': stw flags %x size %ld, rtf flags %x size %ld\n", (unsigned long)i,
                    a[i].cp < 128 ? (int)a[i].cp : '?', a[i].flags, (long)a[i].size, b[i].flags, (long)b[i].size);
        }
        it += ((a[i].flags ^ b[i].flags) & WP_STYLE_ITALIC) == 0;
        ul += ((a[i].flags ^ b[i].flags) & WP_STYLE_UNDERLINE) == 0;
        sz += a[i].size == b[i].size;
    }
    printf("chars %lu %lu matched %lu align %lu bold %lu italic %lu underline %lu size %lu\n",
           (unsigned long)na, (unsigned long)nb, (unsigned long)same_text, (unsigned long)al,
           (unsigned long)bo, (unsigned long)it, (unsigned long)ul, (unsigned long)sz);
    return 0;
}
