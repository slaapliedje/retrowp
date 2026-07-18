#include <string.h>
#include "format_txt.h"

static wp_status import_add_line(wp_document *doc, wp_section *sec,
                                  const u8 *line, u32 line_len)
{
    wp_status st;
    u32 para_idx;
    wp_paragraph *para;
    wp_span span;
    u16 run_idx;

    st = wp_section_paragraph_add(sec, &para_idx);
    if (st != WP_OK) return st;
    para = wp_section_paragraph_get(sec, para_idx);

    st = wp_strpool_append(&doc->strings, line, line_len, &span);
    if (st != WP_OK) return st;

    st = wp_paragraph_run_add(para, span, doc->default_cstyle, &run_idx);
    if (st != WP_OK) return st;

    wp_paragraph_pstyle_set(para, doc->default_pstyle);
    return WP_OK;
}

wp_status format_txt_import(const u8 *bytes, u32 len, wp_document *doc)
{
    wp_status st;
    u32 sec_idx;
    wp_section *sec;
    u32 i, line_start;

    st = wp_document_init(doc);
    if (st != WP_OK) return st;

    st = wp_document_section_add(doc, &sec_idx);
    if (st != WP_OK) { wp_document_free(doc); return st; }
    sec = wp_document_section_get(doc, sec_idx);

    line_start = 0;
    for (i = 0; i < len; i++) {
        if (bytes[i] == '\n') {
            u32 line_end = i;
            if (line_end > line_start && bytes[line_end - 1] == '\r') line_end--;

            st = import_add_line(doc, sec, bytes + line_start, line_end - line_start);
            if (st != WP_OK) { wp_document_free(doc); return st; }

            line_start = i + 1;
        }
    }
    if (line_start < len) {
        st = import_add_line(doc, sec, bytes + line_start, len - line_start);
        if (st != WP_OK) { wp_document_free(doc); return st; }
    }

    return WP_OK;
}

static u32 export_size(const wp_document *doc)
{
    u32 total = 0;
    u32 si, pi;

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            u16 ri;

            for (ri = 0; ri < para->run_count; ri++) {
                total += para->runs[ri].text.length;
            }
            total += 1; /* trailing '\n' */
        }
    }
    return total;
}

wp_status format_txt_export(const wp_document *doc, u8 *out, u32 out_cap,
                             u32 *out_len)
{
    u32 needed = export_size(doc);
    u32 written = 0;
    u32 si, pi;

    if (out_len) *out_len = needed;
    if (needed > out_cap) return WP_RANGE;

    for (si = 0; si < doc->section_count; si++) {
        const wp_section *sec = &doc->sections[si];
        for (pi = 0; pi < sec->para_count; pi++) {
            const wp_paragraph *para = &sec->paras[pi];
            u16 ri;

            for (ri = 0; ri < para->run_count; ri++) {
                wp_span span = para->runs[ri].text;
                if (span.length > 0) {
                    memcpy(out + written, wp_strpool_get(&doc->strings, span), span.length);
                    written += span.length;
                }
            }
            out[written] = '\n';
            written++;
        }
    }

    return WP_OK;
}
