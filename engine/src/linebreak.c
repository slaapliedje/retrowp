#include <stddef.h>
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

typedef struct {
    wp_bool is_space;
    u32 start_run, start_offset;
    u32 end_run, end_offset;
    i32 width;
    i32 height;
} token_t;

#define MAX_TOKENS 512

/* Tokenizes para's concatenated text into space/word tokens (see
   docs/layout.md for the merge-across-run-boundaries rule). Returns the
   token count, or 0 for an empty paragraph. A fixed-size token buffer
   is used rather than a growable one -- 512 tokens (words+gaps) is far
   beyond any single paragraph this engine is realistic for at the 1MB
   tier; exceeding it truncates rather than erroring, since running out
   of buffer space to describe wrap points is not worth failing the
   whole relayout over. */
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
            wp_bool seg_is_space = (wp_bool)(bytes[offset] == ' ');
            i32 width, height;
            wp_status st;

            offset = utf8_next(bytes, run->text.length, offset);
            while (offset < run->text.length &&
                   (wp_bool)(bytes[offset] == ' ') == seg_is_space) {
                offset = utf8_next(bytes, run->text.length, offset);
            }

            st = measure_and_height(cache, doc, run->style, bytes + seg_start,
                                     offset - seg_start, &width, &height);
            if (st != WP_OK) return st;

            /* Consecutive fragments from the SAME run always alternate
               class (by construction of the while loop above), so
               matching the previous token's class here only happens
               when this fragment is the first one of a new run
               continuing the same class the previous run ended with --
               exactly the cross-run merge case. */
            if (count > 0 && tokens[count - 1].is_space == seg_is_space) {
                tokens[count - 1].end_run = r_idx;
                tokens[count - 1].end_offset = offset;
                tokens[count - 1].width += width;
                if (height > tokens[count - 1].height) tokens[count - 1].height = height;
            } else {
                tokens[count].is_space = seg_is_space;
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
    layout->line_count++;
    return WP_OK;
}

void wp_paragraph_layout_free(wp_paragraph *para)
{
    wp_paragraph_layout *layout = (wp_paragraph_layout *)para->layout;

    if (layout == NULL) return;
    mem_free(layout->lines);
    mem_free(layout);
    para->layout = NULL;
}

wp_status wp_paragraph_relayout(wp_document *doc, wp_paragraph *para, i32 content_width_twips)
{
    wp_paragraph_layout *layout;
    font_cache cache;
    token_t tokens[MAX_TOKENS];
    u32 token_count;
    wp_status st;
    i32 indent_left = 0, indent_right = 0, indent_first = 0;
    i32 first_line_width, rest_width, available;
    wp_bool have_line_content;
    wp_bool is_first_line;
    u32 line_start_run = 0, line_start_offset = 0;
    u32 line_end_run = 0, line_end_offset = 0;
    i32 line_width = 0, line_height = 0;
    i32 pending_space_width = 0;
    u32 i;

    {
        const wp_para_style *pstyle = wp_document_pstyle_get(doc, para->style);
        if (pstyle != NULL) {
            indent_left = pstyle->indent_left;
            indent_right = pstyle->indent_right;
            indent_first = pstyle->indent_first;
        }
    }
    first_line_width = content_width_twips - indent_left - indent_right - indent_first;
    rest_width = content_width_twips - indent_left - indent_right;

    layout = (wp_paragraph_layout *)para->layout;
    if (layout == NULL) {
        layout = (wp_paragraph_layout *)mem_alloc((u32)sizeof(wp_paragraph_layout));
        if (layout == NULL) return WP_NOMEM;
        layout->lines = NULL;
        layout->line_count = 0;
        layout->line_capacity = 0;
        para->layout = layout;
    } else {
        layout->line_count = 0; /* reuse the allocated array; rebuild in place */
    }

    if (para->kind == (u8)WP_PARA_IMAGE) {
        /* M15: a fixed-size, non-flowing block -- exactly one line sized
           from the registered descriptor's own display box, no font
           measurement, tokenize() never invoked. content_width_twips is
           intentionally unused here (no text-wrap-around-image in this
           milestone, see docs/document-model.md's Image section); a
           missing/invalid handle degrades to a zero-size line rather
           than failing relayout outright. */
        const wp_image_desc *desc = wp_document_image_get(doc, para->image);
        i32 w = (desc != NULL) ? desc->display_width_twips : 0;
        i32 h = (desc != NULL) ? desc->display_height_twips : 0;

        st = emit_line(layout, 0, 0, 0, 0, w, h);
        if (st != WP_OK) return st;

        para->layout_dirty = WP_FALSE;
        return WP_OK;
    }

    cache.count = 0;
    st = tokenize(doc, para, &cache, tokens, &token_count);
    if (st != WP_OK) { font_cache_close_all(&cache); return st; }

    if (token_count == 0) {
        /* Empty paragraph: exactly one zero-width line, sized by the
           first run's style if any exists, else the document default. */
        wp_cstyle style = (para->run_count > 0) ? para->runs[0].style : doc->default_cstyle;
        i32 dummy_width, height;

        st = measure_and_height(&cache, doc, style, (const u8 *)"", 0, &dummy_width, &height);
        if (st != WP_OK) { font_cache_close_all(&cache); return st; }

        st = emit_line(layout, 0, 0, 0, 0, 0, height);
        font_cache_close_all(&cache);
        if (st != WP_OK) return st;

        para->layout_dirty = WP_FALSE;
        return WP_OK;
    }

    have_line_content = WP_FALSE;
    is_first_line = WP_TRUE;

    for (i = 0; i < token_count; i++) {
        const token_t *t = &tokens[i];

        if (t->is_space) {
            pending_space_width = t->width;
            continue;
        }

        available = is_first_line ? first_line_width : rest_width;

        if (!have_line_content) {
            line_start_run = t->start_run;
            line_start_offset = t->start_offset;
            line_width = t->width;
            line_height = t->height;
            have_line_content = WP_TRUE;
        } else {
            i32 candidate = line_width + pending_space_width + t->width;
            if (candidate <= available) {
                line_width = candidate;
                if (t->height > line_height) line_height = t->height;
            } else {
                st = emit_line(layout, line_start_run, line_start_offset,
                                line_end_run, line_end_offset, line_width, line_height);
                if (st != WP_OK) { font_cache_close_all(&cache); return st; }

                is_first_line = WP_FALSE;
                line_start_run = t->start_run;
                line_start_offset = t->start_offset;
                line_width = t->width;
                line_height = t->height;
            }
        }
        pending_space_width = 0;
        line_end_run = t->end_run;
        line_end_offset = t->end_offset;
    }

    if (have_line_content) {
        st = emit_line(layout, line_start_run, line_start_offset,
                        line_end_run, line_end_offset, line_width, line_height);
        if (st != WP_OK) { font_cache_close_all(&cache); return st; }
    }

    font_cache_close_all(&cache);
    para->layout_dirty = WP_FALSE;
    return WP_OK;
}
