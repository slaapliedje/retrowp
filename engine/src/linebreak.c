/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stddef.h>
#include <string.h>
#include "layout.h"
#include "platform.h"
#include "utf8.h"
#include "growth_internal.h"

#define FONT_CACHE_SIZE 8

typedef struct {
    wp_cstyle  handle;
    plat_font *font;
} font_cache_entry;

typedef struct {
    font_cache_entry entries[FONT_CACHE_SIZE];
    u32 count;
} font_cache;

static void font_cache_close_all(font_cache *cache)
{
    u32 i;

    for (i = 0; i < cache->count; i++) plat_font_close(cache->entries[i].font);
    cache->count = 0;
}

/* Measures bytes[0..len) under cstyle `handle`'s font, returning both
   its width and line height (ascent+descent+leading). Opens/reuses a
   small per-relayout-call font cache; overflow past FONT_CACHE_SIZE
   distinct styles in one paragraph still works, just without caching
   (a temporary font is opened and closed for that one call). Font
   names are not yet resolvable from wp_char_style.font_name -- a bare
   wp_str offset with no length, and nothing in the codebase sets it to
   non-zero yet -- so this always requests the platform's default font;
   see docs/layout.md. */
static wp_status measure_and_height(font_cache *cache, wp_document *doc, wp_cstyle handle,
                                     const u8 *bytes, u32 len, i32 *width_out, i32 *height_out)
{
    plat_font *f = NULL;
    wp_status st;
    plat_font_metrics metrics;
    wp_bool temp_font = WP_FALSE;
    u32 i;

    for (i = 0; i < cache->count; i++) {
        if (cache->entries[i].handle == handle) {
            f = cache->entries[i].font;
            break;
        }
    }

    if (f == NULL) {
        const wp_char_style *style = wp_document_cstyle_get(doc, handle);
        if (style == NULL) return WP_RANGE;

        st = plat_font_open((const u8 *)"", 0, style->size_twips, style->flags, &f);
        if (st != WP_OK) return st;

        if (cache->count < FONT_CACHE_SIZE) {
            cache->entries[cache->count].handle = handle;
            cache->entries[cache->count].font = f;
            cache->count++;
        } else {
            temp_font = WP_TRUE;
        }
    }

    st = plat_measure_text(f, bytes, len, width_out);
    if (st != WP_OK) {
        if (temp_font) plat_font_close(f);
        return st;
    }

    st = plat_font_metrics_get(f, &metrics);
    if (st != WP_OK) {
        if (temp_font) plat_font_close(f);
        return st;
    }
    *height_out = metrics.ascent + metrics.descent + metrics.leading;

    if (temp_font) plat_font_close(f);
    return WP_OK;
}

/* Three token classes: a word (anything but blank or tab), a run of
   blanks (one gap however many spaces), and a tab -- one tab per token,
   never merged, since each advances to its own stop. */
#define TOK_WORD  0
#define TOK_SPACE 1
#define TOK_TAB   2

typedef struct {
    u8  kind;
    u32 start_run, start_offset;
    u32 end_run, end_offset;
    i32 width;
    i32 height;
} token_t;

#define MAX_TOKENS 512

static u8 byte_class(u8 c)
{
    if (c == ' ') return TOK_SPACE;
    if (c == '\t') return TOK_TAB;
    return TOK_WORD;
}

/* Tokenizes para's concatenated text into word/space/tab tokens (see
   docs/layout.md for the merge-across-run-boundaries rule). Returns the
   token count, or 0 for an empty paragraph. A fixed-size token buffer
   is used rather than a growable one -- 512 tokens (words+gaps) is far
   beyond any single paragraph this engine is realistic for at the 1MB
   tier; exceeding it truncates rather than erroring, since running out
   of buffer space to describe wrap points is not worth failing the
   whole relayout over. A tab's width here is only its height carrier:
   where it ends depends on where it starts, which only the line breaker
   knows. */
static wp_status tokenize(wp_document *doc, const wp_paragraph *para,
                           font_cache *cache, token_t *tokens, u32 *out_count)
{
    u32 count = 0;
    u32 r_idx;

    for (r_idx = 0; r_idx < para->run_count && count < MAX_TOKENS; r_idx++) {
        const wp_run *run = &para->runs[r_idx];
        const u8 *bytes;
        u32 offset;

        if (run->text.length == 0) continue;
        bytes = wp_strpool_get(&doc->strings, run->text);
        offset = 0;

        while (offset < run->text.length && count < MAX_TOKENS) {
            u32 seg_start = offset;
            u8 seg_kind = byte_class(bytes[offset]);
            i32 width, height;
            wp_status st;

            offset = utf8_next(bytes, run->text.length, offset);
            if (seg_kind != TOK_TAB) {
                while (offset < run->text.length && byte_class(bytes[offset]) == seg_kind) {
                    offset = utf8_next(bytes, run->text.length, offset);
                }
            }

            if (seg_kind == TOK_TAB) {
                /* Height only: measure a blank so the tab's line is as
                   tall as its font, without asking the font for a
                   control character's glyph. */
                st = measure_and_height(cache, doc, run->style, (const u8 *)" ", 1, &width, &height);
                width = 0;
            } else {
                st = measure_and_height(cache, doc, run->style, bytes + seg_start,
                                         offset - seg_start, &width, &height);
            }
            if (st != WP_OK) return st;

            /* Consecutive fragments from the SAME run always alternate
               class (by construction of the while loop above), so
               matching the previous token's class here only happens
               when this fragment is the first one of a new run
               continuing the same class the previous run ended with --
               exactly the cross-run merge case. Tabs never merge. */
            if (count > 0 && seg_kind != TOK_TAB && tokens[count - 1].kind == seg_kind) {
                tokens[count - 1].end_run = r_idx;
                tokens[count - 1].end_offset = offset;
                tokens[count - 1].width += width;
                if (height > tokens[count - 1].height) tokens[count - 1].height = height;
            } else {
                tokens[count].kind = seg_kind;
                tokens[count].start_run = r_idx;
                tokens[count].start_offset = seg_start;
                tokens[count].end_run = r_idx;
                tokens[count].end_offset = offset;
                tokens[count].width = width;
                tokens[count].height = height;
                count++;
            }
        }
    }

    *out_count = count;
    return WP_OK;
}

i32 wp_tab_next_stop(const wp_para_style *pstyle, i32 x_twips, u8 *kind_out)
{
    i32 last = 0;
    u8 i;

    if (kind_out != NULL) *kind_out = (u8)WP_TAB_LEFT;
    if (pstyle != NULL) {
        for (i = 0; i < pstyle->tab_count && i < WP_MAX_TABS; i++) {
            if (pstyle->tab_pos[i] > x_twips) {
                if (kind_out != NULL) *kind_out = pstyle->tab_kind[i];
                return pstyle->tab_pos[i];
            }
            last = pstyle->tab_pos[i];
        }
    }
    /* Default stops continue past the last explicit one. */
    if (x_twips < last) x_twips = last;
    if (x_twips < 0) return 0;
    return (x_twips / WP_DEFAULT_TAB_TWIPS + 1) * WP_DEFAULT_TAB_TWIPS;
}

static wp_status line_array_ensure(wp_paragraph_layout *layout, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= layout->line_capacity) return WP_OK;

    new_cap = wp_grow_capacity(layout->line_capacity, needed_count, 4);
    new_mem = mem_realloc(layout->lines,
                           layout->line_capacity * (u32)sizeof(wp_line),
                           new_cap * (u32)sizeof(wp_line));
    if (new_mem == NULL) return WP_NOMEM;

    layout->lines = (wp_line *)new_mem;
    layout->line_capacity = new_cap;
    return WP_OK;
}

static wp_status tab_array_ensure(wp_paragraph_layout *layout, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= layout->tab_capacity) return WP_OK;

    new_cap = wp_grow_capacity(layout->tab_capacity, needed_count, 4);
    new_mem = mem_realloc(layout->tab_widths,
                           layout->tab_capacity * (u32)sizeof(i32),
                           new_cap * (u32)sizeof(i32));
    if (new_mem == NULL) return WP_NOMEM;

    layout->tab_widths = (i32 *)new_mem;
    layout->tab_capacity = new_cap;
    return WP_OK;
}

/* The paragraph's spacing and indent, read once per relayout. */
typedef struct {
    u8  align;
    i32 indent_left, indent_right, indent_first;
    i32 space_before, space_after, line_spacing;
    const wp_para_style *pstyle;
    i32 content_width;
} para_geom;

/* The x (from the column's left edge) a line's content starts at before
   alignment, and the width it may fill. */
static i32 line_x0(const para_geom *g, wp_bool first) {
    return g->indent_left + (first ? g->indent_first : 0);
}
static i32 line_avail(const para_geom *g, wp_bool first) {
    return g->content_width - g->indent_left - g->indent_right - (first ? g->indent_first : 0);
}

/* A line box from the text's own height: line spacing first, then the
   paragraph's space above its first line and below its last. */
static void line_box(const para_geom *g, wp_line *line, i32 text_h, wp_bool first, wp_bool last)
{
    i32 box = text_h;

    if (g->line_spacing > 0) {
        box = (i32)(((long)text_h * (long)g->line_spacing) / 240L);
        if (box < text_h) box = text_h;
    } else if (g->line_spacing < 0 && -g->line_spacing > text_h) {
        box = -g->line_spacing;
    }
    line->text_height_twips = text_h;
    line->text_top_twips = first ? g->space_before : 0;
    line->height_twips = box + line->text_top_twips + (last ? g->space_after : 0);
}

static void line_place(const para_geom *g, wp_line *line, wp_bool first)
{
    i32 avail = line_avail(g, first);
    i32 off = 0;

    if (g->align == WP_ALIGN_CENTER) off = (avail - line->width_twips) / 2;
    else if (g->align == WP_ALIGN_RIGHT) off = avail - line->width_twips;
    if (off < 0) off = 0;
    line->avail_twips = avail;
    line->x_twips = line_x0(g, first) + off;
}

static wp_status emit_line(wp_paragraph_layout *layout, u32 start_run, u32 start_offset,
                            u32 end_run, u32 end_offset, i32 width, i32 height)
{
    wp_status st;
    wp_line *line;

    st = line_array_ensure(layout, layout->line_count + 1);
    if (st != WP_OK) return st;

    line = &layout->lines[layout->line_count];
    line->start_run = start_run;
    line->start_offset = start_offset;
    line->end_run = end_run;
    line->end_offset = end_offset;
    line->width_twips = width;
    line->height_twips = height;
    line->text_height_twips = height;
    line->text_top_twips = 0;
    line->x_twips = 0;
    line->avail_twips = width;
    line->first_tab = layout->tab_count;
    line->tab_count = 0;
    layout->line_count++;
    return WP_OK;
}

/* Second pass over one line's tokens [ts, te]: every tab's real advance
   now that the text after it is known (a right tab ends its segment at
   the stop, a centre tab centres it there; decimal is treated as right),
   the line's true width, and its tabs appended to the layout in order. */
static wp_status resolve_line_tabs(wp_paragraph_layout *layout, wp_line *line, const para_geom *g,
                                   token_t *tokens, u32 ts, u32 te, wp_bool first)
{
    i32 x0 = line_x0(g, first);
    i32 x = x0;
    u32 i;

    for (i = ts; i <= te; i++) {
        if (tokens[i].kind == TOK_TAB) {
            u8 kind;
            i32 stop = wp_tab_next_stop(g->pstyle, x, &kind);
            i32 seg = 0, w;
            u32 j;
            wp_status st;

            for (j = i + 1; j <= te && tokens[j].kind != TOK_TAB; j++) seg += tokens[j].width;
            if (kind == (u8)WP_TAB_RIGHT || kind == (u8)WP_TAB_DECIMAL) w = stop - x - seg;
            else if (kind == (u8)WP_TAB_CENTER) w = stop - x - seg / 2;
            else w = stop - x;
            if (w < 0) w = 0;
            tokens[i].width = w;

            st = tab_array_ensure(layout, layout->tab_count + 1);
            if (st != WP_OK) return st;
            layout->tab_widths[layout->tab_count++] = w;
            line->tab_count++;
        }
        x += tokens[i].width;
    }
    line->width_twips = x - x0;
    return WP_OK;
}

void wp_paragraph_layout_free(wp_paragraph *para)
{
    wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;

    if (layout == NULL) return;
    mem_free(layout->lines);
    if (layout->tab_widths != NULL) mem_free(layout->tab_widths);
    mem_free(layout);
    para->layout = NULL;
}

wp_status wp_paragraph_relayout(wp_document *doc, wp_paragraph *para, i32 content_width_twips)
{
    wp_paragraph_layout *layout;
    font_cache cache;
    token_t *tokens;
    u32 token_count;
    wp_status st;
    para_geom g;
    wp_bool have_line_content;
    wp_bool is_first_line;
    u32 line_first_tok = 0, line_last_tok = 0;
    i32 line_width = 0, line_height = 0;
    i32 pending_space_width = 0;
    u32 i;

    memset(&g, 0, sizeof(g));
    g.content_width = content_width_twips;
    g.pstyle = wp_document_pstyle_get(doc, para->style);
    if (g.pstyle != NULL) {
        g.align = g.pstyle->align;
        g.indent_left = g.pstyle->indent_left;
        g.indent_right = g.pstyle->indent_right;
        g.indent_first = g.pstyle->indent_first;
        g.space_before = g.pstyle->space_before;
        g.space_after = g.pstyle->space_after;
        g.line_spacing = g.pstyle->line_spacing;
    }

    layout = (wp_paragraph_layout *)para->layout;
    if (layout == NULL) {
        layout = (wp_paragraph_layout *)mem_alloc((u32)sizeof(wp_paragraph_layout));
        if (layout == NULL) return WP_NOMEM;
        memset(layout, 0, sizeof(*layout));
        para->layout = layout;
    } else {
        layout->line_count = 0; /* reuse the allocated arrays; rebuild in place */
        layout->tab_count = 0;
    }

    if (para->kind == (u8)WP_PARA_IMAGE) {
        /* M15: a fixed-size, non-flowing block -- exactly one line sized
           from the registered descriptor's own display box, no font
           measurement, tokenize() never invoked. No text wraps around an
           image (docs/document-model.md's Image section); it is placed
           by the paragraph's indent and alignment and spaced like any
           other paragraph. A missing/invalid handle degrades to a
           zero-size line rather than failing relayout outright. */
        const wp_image_desc *desc = wp_document_image_get(doc, para->image);
        i32 w = (desc != NULL) ? desc->display_width_twips : 0;
        i32 h = (desc != NULL) ? desc->display_height_twips : 0;
        para_geom ig = g;

        ig.line_spacing = 0;   /* a picture is as tall as it is */
        st = emit_line(layout, 0, 0, 0, 0, w, h);
        if (st != WP_OK) return st;
        line_box(&ig, &layout->lines[0], h, WP_TRUE, WP_TRUE);
        line_place(&ig, &layout->lines[0], WP_TRUE);

        para->layout_dirty = WP_FALSE;
        return WP_OK;
    }

    /* HEAP, not stack. This was `token_t tokens[MAX_TOKENS]` -- a single
       ~14KB automatic array (512 entries x 28 bytes), which is more
       stack than some of this project's own targets give a process in
       total. AmigaOS hands a RUN-launched program 4096 bytes; measured
       on target, 1292 of those were already in use by the time MUI's
       draw callback reached this function, leaving ~2.8KB for a 14KB
       frame. The result was a ~5x overflow that scribbled over whatever
       lived below the stack -- surviving harmlessly when that memory
       happened to be dead, and producing wild jumps into garbage when it
       was not, which is why RetroWP crashed on roughly 17 of 18 Amiga
       boots with an illegal instruction at a different address each time
       and sometimes took an unrelated task (IPrefs) down with it.
       Allocated after the image-paragraph branch above, so an image
       paragraph -- which never tokenizes -- pays nothing. */
    tokens = (token_t *)mem_alloc((u32)(sizeof(token_t) * MAX_TOKENS));
    if (tokens == NULL) return WP_NOMEM;

    cache.count = 0;
    st = tokenize(doc, para, &cache, tokens, &token_count);
    if (st != WP_OK) { font_cache_close_all(&cache); mem_free(tokens); return st; }

    if (token_count == 0) {
        /* Empty paragraph: exactly one zero-width line, sized by the
           first run's style if any exists, else the document default. */
        wp_cstyle style = (para->run_count > 0) ? para->runs[0].style : doc->default_cstyle;
        i32 dummy_width, height;

        st = measure_and_height(&cache, doc, style, (const u8 *)"", 0, &dummy_width, &height);
        if (st != WP_OK) { font_cache_close_all(&cache); mem_free(tokens); return st; }

        st = emit_line(layout, 0, 0, 0, 0, 0, height);
        font_cache_close_all(&cache);
        mem_free(tokens);
        if (st != WP_OK) return st;
        line_box(&g, &layout->lines[0], height, WP_TRUE, WP_TRUE);
        line_place(&g, &layout->lines[0], WP_TRUE);

        para->layout_dirty = WP_FALSE;
        return WP_OK;
    }

    have_line_content = WP_FALSE;
    is_first_line = WP_TRUE;

    /* Pass one: greedy wrap. A tab is content (it can begin a line, which
       is how a tabbed first line gets its indent) and is sized as a left
       tab from wherever it starts; pass two corrects right and centre
       tabs once the text after them is known. Blanks at a line's start
       and end belong to no line, as before. */
    for (i = 0; i < token_count; i++) {
        token_t *t = &tokens[i];
        i32 available;

        if (t->kind == TOK_SPACE) {
            pending_space_width = t->width;
            continue;
        }

        available = line_avail(&g, is_first_line);

        if (t->kind == TOK_TAB) {
            i32 at = line_x0(&g, is_first_line) +
                     (have_line_content ? line_width + pending_space_width : 0);
            t->width = wp_tab_next_stop(g.pstyle, at, NULL) - at;
        }

        if (!have_line_content) {
            line_first_tok = i;
            line_width = t->width;
            line_height = t->height;
            have_line_content = WP_TRUE;
        } else {
            i32 candidate = line_width + pending_space_width + t->width;
            if (candidate <= available) {
                line_width = candidate;
                if (t->height > line_height) line_height = t->height;
            } else {
                st = emit_line(layout, tokens[line_first_tok].start_run, tokens[line_first_tok].start_offset,
                                tokens[line_last_tok].end_run, tokens[line_last_tok].end_offset,
                                line_width, line_height);
                if (st == WP_OK) {
                    st = resolve_line_tabs(layout, &layout->lines[layout->line_count - 1], &g,
                                           tokens, line_first_tok, line_last_tok, is_first_line);
                }
                if (st != WP_OK) { font_cache_close_all(&cache); mem_free(tokens); return st; }

                is_first_line = WP_FALSE;
                if (t->kind == TOK_TAB) {
                    i32 at = line_x0(&g, WP_FALSE);
                    t->width = wp_tab_next_stop(g.pstyle, at, NULL) - at;
                }
                line_first_tok = i;
                line_width = t->width;
                line_height = t->height;
            }
        }
        pending_space_width = 0;
        line_last_tok = i;
    }

    if (have_line_content) {
        st = emit_line(layout, tokens[line_first_tok].start_run, tokens[line_first_tok].start_offset,
                        tokens[line_last_tok].end_run, tokens[line_last_tok].end_offset,
                        line_width, line_height);
        if (st == WP_OK) {
            st = resolve_line_tabs(layout, &layout->lines[layout->line_count - 1], &g,
                                   tokens, line_first_tok, line_last_tok, is_first_line);
        }
        if (st != WP_OK) { font_cache_close_all(&cache); mem_free(tokens); return st; }
    }

    /* Boxes and placement, now that first and last are known. */
    for (i = 0; i < layout->line_count; i++) {
        wp_line *line = &layout->lines[i];
        line_box(&g, line, line->text_height_twips, (wp_bool)(i == 0),
                 (wp_bool)(i + 1 == layout->line_count));
        line_place(&g, line, (wp_bool)(i == 0));
    }

    font_cache_close_all(&cache);
    mem_free(tokens);
    para->layout_dirty = WP_FALSE;
    return WP_OK;
}
