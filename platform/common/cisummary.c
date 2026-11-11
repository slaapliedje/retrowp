/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "cisummary.h"
#include "drawing.h"

static u32 put_str(char *out, u32 at, u32 cap, const char *s)
{
    while (*s != '\0' && at < cap) out[at++] = *s++;
    return at;
}

static u32 put_num(char *out, u32 at, u32 cap, u32 v)
{
    char rev[12];
    u32 n = 0;
    do { rev[n++] = (char)('0' + v % 10u); v /= 10u; } while (v > 0 && n < (u32)sizeof(rev));
    while (n > 0 && at < cap) out[at++] = rev[--n];
    return at;
}

u32 wp_ci_doc_summary(const wp_document *doc, char *out, u32 cap)
{
    u32 paras = 0, pictures = 0, drawings = 0, rows = 0, frames = 0, si, pi;
    u32 at = 0;
    wp_frame last_frame = 0;

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *p = &sec->paras[pi];
            paras++;
            if (p->kind == (u8)WP_PARA_IMAGE) {
                const wp_image_desc *d = wp_document_image_get(doc, p->image);
                if (d != NULL && WP_IMAGE_IS_DRAWING(d)) drawings++; else pictures++;
            }
            if (p->trow != 0 && (p->tflags & WP_TROW_END)) rows++;
            if (p->frame != 0 && p->frame != last_frame) frames++;
            last_frame = p->frame;
        }
    }
    at = put_str(out, at, cap, "doc paras=");    at = put_num(out, at, cap, paras);
    at = put_str(out, at, cap, " pictures=");    at = put_num(out, at, cap, pictures);
    at = put_str(out, at, cap, " drawings=");    at = put_num(out, at, cap, drawings);
    at = put_str(out, at, cap, " rows=");        at = put_num(out, at, cap, rows);
    at = put_str(out, at, cap, " frames=");      at = put_num(out, at, cap, frames);
    at = put_str(out, at, cap, "\n");
    return at;
}
