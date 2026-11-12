/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "merge.h"
#include "strpool.h"
#include "utf8.h"
#include "platform.h"

wp_bool wp_merge_next_field(const u8 *text, u32 len, u32 from, u32 *start, u32 *end,
                            u32 *name_at, u32 *name_len)
{
    u32 i, k;

    for (i = from; i + 1 < len; i++) {
        if (text[i] != 0xC2 || text[i + 1] != 0xAB) continue;
        for (k = i + 2; k + 1 < len && k - (i + 2) <= WP_MERGE_NAME_MAX; k++) {
            if (text[k] == '\n' || text[k] == '\r') break;
            if (text[k] == 0xC2 && text[k + 1] == 0xAB) break;   /* another opens: not this one */
            if (text[k] == 0xC2 && text[k + 1] == 0xBB) {
                if (k == i + 2) break;   /* «» names nothing */
                *start = i;
                *end = k + 2;
                *name_at = i + 2;
                *name_len = k - (i + 2);
                return WP_TRUE;
            }
        }
    }
    return WP_FALSE;
}

/* ---- the data file --------------------------------------------------- */

static wp_bool valid_utf8(const u8 *b, u32 len)
{
    u32 pos = 0, cp, n;
    while (pos < len) {
        if (b[pos] < 0x80) { pos++; continue; }
        n = utf8_decode(b + pos, len - pos, &cp);
        if (n == 0) return WP_FALSE;
        pos += n;
    }
    return WP_TRUE;
}

/* The delimiter the first line uses most: tab, semicolon or comma. */
static u8 delimiter(const u8 *b, u32 len)
{
    u32 i, tabs = 0, semis = 0, commas = 0;
    wp_bool quoted = WP_FALSE;
    for (i = 0; i < len && (quoted || (b[i] != '\n' && b[i] != '\r')); i++) {
        if (b[i] == '"') quoted = (wp_bool)!quoted;
        else if (!quoted && b[i] == '\t') tabs++;
        else if (!quoted && b[i] == ';') semis++;
        else if (!quoted && b[i] == ',') commas++;
    }
    if (tabs >= semis && tabs >= commas && tabs > 0) return '\t';
    if (semis > commas) return ';';
    return ',';
}

/* One pass over the file: with `d` NULL it counts the rows and the
   widest; with it, it fills the cells (unquoted, in place in d->bytes,
   which never grows). */
static void scan(u8 *b, u32 len, u8 delim, u32 *rows, u32 *widest, wp_merge_data *d)
{
    u32 pos = 0, row = 0, col = 0, out = 0, cell_start = 0;
    wp_bool quoted = WP_FALSE, row_has = WP_FALSE;

    *widest = 0;
    while (pos <= len) {
        u8 c = pos < len ? b[pos] : (u8)'\n';
        wp_bool end_cell = WP_FALSE, end_row = WP_FALSE;

        if (quoted) {
            if (c == '"' && pos + 1 < len && b[pos + 1] == '"') { if (d) b[out] = '"'; out++; pos += 2; continue; }
            if (c == '"') { quoted = WP_FALSE; pos++; continue; }
            if (pos == len) { quoted = WP_FALSE; continue; }   /* an unclosed quote ends at the end */
            if (d) b[out] = c;
            out++; pos++;
            continue;
        }
        if (c == '"') { quoted = WP_TRUE; row_has = WP_TRUE; pos++; continue; }
        if (c == delim) end_cell = WP_TRUE;
        else if (c == '\n' || c == '\r') end_row = end_cell = WP_TRUE;
        if (!end_cell) {
            if (d) b[out] = c;
            out++; pos++; row_has = WP_TRUE;
            continue;
        }
        if (row_has || col > 0 || end_row == WP_FALSE) {
            if (d != NULL && col < d->fields && row < d->records + 1u) {
                d->cell_at[row * d->fields + col] = cell_start;
                d->cell_len[row * d->fields + col] = out - cell_start;
            }
            col++;
        }
        if (c == '\r' && pos + 1 < len && b[pos + 1] == '\n') pos++;
        pos++;
        cell_start = out;
        if (end_row) {
            if (col > 0) {
                if (col > *widest && row == 0) *widest = col;
                row++;
            }
            col = 0;
            row_has = WP_FALSE;
        }
    }
    *rows = row;
}

wp_status wp_merge_data_read(const u8 *bytes, u32 len, wp_merge_data *out)
{
    u8 *b;
    u32 n, rows = 0, widest = 0, i;
    u8 delim;

    memset(out, 0, sizeof(*out));
    if (bytes == NULL) return WP_ERR;
    /* UTF-8 as it is; anything else is this machine's set, made UTF-8
       (at most three bytes a character). */
    if (valid_utf8(bytes, len)) {
        b = (u8 *)mem_alloc(len + 1u);
        if (b == NULL) return WP_NOMEM;
        memcpy(b, bytes, len);
        n = len;
    } else {
        b = (u8 *)mem_alloc(len * 3u + 1u);
        if (b == NULL) return WP_NOMEM;
        if (plat_native_to_utf8(bytes, len, b, len * 3u, &n) != WP_OK) { mem_free(b); return WP_BADFMT; }
    }
    if (n >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) {   /* a byte-order mark */
        memmove(b, b + 3, n - 3);
        n -= 3;
    }
    delim = delimiter(b, n);
    scan(b, n, delim, &rows, &widest, NULL);
    if (rows == 0 || widest == 0) { mem_free(b); return WP_BADFMT; }
    out->bytes = b;
    out->fields = widest;
    out->records = rows - 1u;
    out->cell_at = (u32 *)mem_alloc(rows * widest * (u32)sizeof(u32));
    out->cell_len = (u32 *)mem_alloc(rows * widest * (u32)sizeof(u32));
    if (out->cell_at == NULL || out->cell_len == NULL) { wp_merge_data_free(out); return WP_NOMEM; }
    for (i = 0; i < rows * widest; i++) { out->cell_at[i] = 0; out->cell_len[i] = 0; }
    scan(b, n, delim, &rows, &widest, out);
    out->fields = widest;
    return WP_OK;
}

void wp_merge_data_free(wp_merge_data *d)
{
    if (d->bytes != NULL) mem_free(d->bytes);
    if (d->cell_at != NULL) mem_free(d->cell_at);
    if (d->cell_len != NULL) mem_free(d->cell_len);
    memset(d, 0, sizeof(*d));
}

static u8 lower(u8 c) { return (u8)(c >= 'A' && c <= 'Z' ? c + 32 : c); }

i32 wp_merge_data_column(const wp_merge_data *d, const u8 *name, u32 len)
{
    u32 c;
    while (len > 0 && name[0] == ' ') { name++; len--; }
    while (len > 0 && name[len - 1] == ' ') len--;
    for (c = 0; c < d->fields; c++) {
        const u8 *h = d->bytes + d->cell_at[c];
        u32 hl = d->cell_len[c], i;
        while (hl > 0 && h[0] == ' ') { h++; hl--; }
        while (hl > 0 && h[hl - 1] == ' ') hl--;
        if (hl != len) continue;
        for (i = 0; i < len && lower(h[i]) == lower(name[i]); i++) {}
        if (i == len) return (i32)c;
    }
    return -1;
}

/* ---- the merge ------------------------------------------------------- */

/* text with each field the data names replaced by record `rec`'s value,
   appended to the pool; *changed whether anything was. */
static wp_status fill(wp_document *doc, const wp_merge_data *d, u32 rec, wp_span text,
                      wp_span *out, wp_bool *changed)
{
    const u8 *t = wp_strpool_get(&doc->strings, text);
    u32 pos = 0, start, end, na, nl, need = 0, w = 0;
    u8 *buf;
    wp_status st;

    *changed = WP_FALSE;
    *out = text;
    if (t == NULL || !wp_merge_next_field(t, text.length, 0, &start, &end, &na, &nl)) return WP_OK;
    /* How long, then the bytes. */
    while (wp_merge_next_field(t, text.length, pos, &start, &end, &na, &nl)) {
        i32 col = wp_merge_data_column(d, t + na, nl);
        need += (start - pos) + (col >= 0 ? d->cell_len[(rec + 1u) * d->fields + (u32)col] : end - start);
        pos = end;
    }
    need += text.length - pos;
    buf = (u8 *)mem_alloc(need > 0 ? need : 1u);
    if (buf == NULL) return WP_NOMEM;
    pos = 0;
    while (wp_merge_next_field(t, text.length, pos, &start, &end, &na, &nl)) {
        i32 col = wp_merge_data_column(d, t + na, nl);
        memcpy(buf + w, t + pos, start - pos);
        w += start - pos;
        if (col >= 0) {
            u32 cell = (rec + 1u) * d->fields + (u32)col;
            memcpy(buf + w, d->bytes + d->cell_at[cell], d->cell_len[cell]);
            w += d->cell_len[cell];
        } else {
            memcpy(buf + w, t + start, end - start);
            w += end - start;
        }
        pos = end;
    }
    memcpy(buf + w, t + pos, text.length - pos);
    w += text.length - pos;
    /* The pool may move as it grows: t is not used after this. */
    st = wp_strpool_append(&doc->strings, buf, w, out);
    mem_free(buf);
    if (st == WP_OK) *changed = WP_TRUE;
    return st;
}

/* Paragraph `src` of section `from` filled from record `rec` into a new
   paragraph at the end of section `to`. */
static wp_status copy_para(wp_document *doc, u32 from, u32 pi, u32 to, const wp_merge_data *d, u32 rec)
{
    wp_section *ss = wp_document_section_get(doc, from);
    wp_section *ds;
    wp_paragraph *sp, *dp;
    u32 idx;
    u16 ri;
    wp_status st;

    st = wp_section_paragraph_add(wp_document_section_get(doc, to), &idx);
    if (st != WP_OK) return st;
    /* Adding may move both sections' paragraph arrays: fetch afresh. */
    ss = wp_document_section_get(doc, from);
    ds = wp_document_section_get(doc, to);
    sp = wp_section_paragraph_get(ss, pi);
    dp = wp_section_paragraph_get(ds, idx);
    wp_paragraph_pstyle_set(dp, sp->style);
    dp->kind = sp->kind;
    dp->image = sp->image;
    dp->trow = sp->trow;
    dp->tflags = sp->tflags;
    dp->frame = sp->frame;
    for (ri = 0; ri < sp->run_count; ri++) {
        wp_run run = *wp_paragraph_run_get(sp, ri);
        wp_span text;
        wp_bool changed;
        u16 out;
        st = fill(doc, d, rec, run.text, &text, &changed);
        if (st != WP_OK) return st;
        ss = wp_document_section_get(doc, from);
        ds = wp_document_section_get(doc, to);
        sp = wp_section_paragraph_get(ss, pi);
        dp = wp_section_paragraph_get(ds, idx);
        st = wp_paragraph_run_add(dp, text, run.style, &out);
        if (st != WP_OK) return st;
    }
    return WP_OK;
}

wp_status wp_merge_labels(wp_document *doc, const wp_merge_data *d, u32 across, u32 down,
                          u32 *copies_out)
{
    wp_section *sec;
    wp_table_row row;
    wp_trow handle;
    u32 tmpl, pi, rec, col = 0, k;
    i32 column, text_h;
    wp_status st;

    if (copies_out != NULL) *copies_out = 0;
    if (doc == NULL || d == NULL || d->records == 0 || doc->section_count == 0) return WP_ERR;
    if (across < 1 || across > WP_MAX_CELLS || down < 1 || down > 60) return WP_RANGE;
    sec = wp_document_section_get(doc, 0);
    tmpl = sec->para_count;
    if (tmpl == 0) return WP_ERR;
    for (pi = 0; pi < tmpl; pi++) {
        const wp_paragraph *p = wp_section_paragraph_get(sec, pi);
        if (p->trow != 0 || p->frame != 0) return WP_UNSUPPORTED;
    }
    column = sec->page_width - sec->margin_left - sec->margin_right;
    text_h = sec->page_height - sec->margin_top - sec->margin_bottom;
    memset(&row, 0, sizeof(row));
    row.gap = 115;   /* a twelfth of an inch in from each label's edge */
    row.cell_count = (u8)across;
    for (k = 0; k < across; k++) row.cell_right[k] = (i32)(((u32)column * (k + 1u)) / across);
    row.height = -(text_h / (i32)down);
    st = wp_document_trow_find_or_add(doc, &row, &handle);
    if (st != WP_OK) return st;

    /* Every label after the template, then the template let go. */
    for (rec = 0; rec < d->records && st == WP_OK; rec++) {
        for (pi = 0; pi < tmpl && st == WP_OK; pi++) {
            st = copy_para(doc, 0, pi, 0, d, rec);
            if (st == WP_OK) {
                wp_section *s0 = wp_document_section_get(doc, 0);
                wp_paragraph *np = wp_section_paragraph_get(s0, s0->para_count - 1);
                np->trow = handle;
                np->tflags = 0;
                if (pi == tmpl - 1) {
                    np->tflags = WP_TCELL_END;
                    if (col == across - 1) np->tflags |= WP_TROW_END;
                }
            }
        }
        if (st == WP_OK) {
            col = (col + 1u) % across;
            if (copies_out != NULL) *copies_out = rec + 1u;
        }
    }
    while (st == WP_OK && col != 0) {   /* the last row filled out */
        u32 idx;
        wp_section *s0 = wp_document_section_get(doc, 0);
        st = wp_section_paragraph_add(s0, &idx);
        if (st == WP_OK) {
            wp_paragraph *np = wp_section_paragraph_get(s0, idx);
            wp_paragraph_pstyle_set(np, doc->default_pstyle);
            np->trow = handle;
            np->tflags = (u8)(WP_TCELL_END | (col == across - 1 ? WP_TROW_END : 0));
            col = (col + 1u) % across;
        }
    }
    if (st != WP_OK) return st;
    wp_section_paragraphs_drop(wp_document_section_get(doc, 0), 0, tmpl);
    return WP_OK;
}

wp_status wp_merge_document(wp_document *doc, const wp_merge_data *d, u32 *copies_out)
{
    u32 sections, rec, s, pi;
    wp_status st = WP_OK;

    if (copies_out != NULL) *copies_out = 0;
    if (doc == NULL || d == NULL || d->records == 0) return WP_ERR;
    sections = doc->section_count;

    /* Records 2.. first, each a copy of the template's sections (a new
       section starts a new page) -- made while the template is still
       untouched; then record 1 fills the template itself. */
    for (rec = 1; rec < d->records && st == WP_OK; rec++) {
        for (s = 0; s < sections && st == WP_OK; s++) {
            u32 ns;
            wp_section *src;
            st = wp_document_section_add(doc, &ns);
            if (st != WP_OK) break;
            src = wp_document_section_get(doc, s);
            {
                wp_section *dst = wp_document_section_get(doc, ns);
                dst->page_width = src->page_width;
                dst->page_height = src->page_height;
                dst->margin_left = src->margin_left;
                dst->margin_right = src->margin_right;
                dst->margin_top = src->margin_top;
                dst->margin_bottom = src->margin_bottom;
                dst->header = src->header;
                dst->footer = src->footer;
            }
            for (pi = 0; pi < wp_document_section_get(doc, s)->para_count && st == WP_OK; pi++) {
                st = copy_para(doc, s, pi, ns, d, rec);
            }
        }
        if (st == WP_OK && copies_out != NULL) *copies_out = rec;
    }
    if (st != WP_OK) return st;
    for (s = 0; s < sections && st == WP_OK; s++) {
        wp_section *sec = wp_document_section_get(doc, s);
        for (pi = 0; pi < sec->para_count && st == WP_OK; pi++) {
            wp_paragraph *para = wp_section_paragraph_get(sec, pi);
            u16 ri;
            for (ri = 0; ri < para->run_count && st == WP_OK; ri++) {
                wp_span text;
                wp_bool changed;
                st = fill(doc, d, 0, wp_paragraph_run_get(para, ri)->text, &text, &changed);
                sec = wp_document_section_get(doc, s);
                para = wp_section_paragraph_get(sec, pi);
                if (st == WP_OK && changed) wp_paragraph_run_get(para, ri)->text = text;
            }
            para->layout_dirty = WP_TRUE;
        }
    }
    if (st == WP_OK && copies_out != NULL) *copies_out = d->records;
    return st;
}
