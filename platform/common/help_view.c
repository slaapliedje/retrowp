#include <string.h>
#include <stddef.h>
#include "help_view.h"

/* A "page" of Page Up/Down scrolling, in wrapped rows. Approximate on
   purpose -- wp_help_view_state has no idea how many rows the viewport
   actually fits (that depends on font/width, known only at paint time);
   see wp_help_view_feed_key's own comment. Sized generously enough to
   feel like a real page on any of this project's real target
   resolutions (ST Low's ~15 rows at a small font through a modern
   desktop's 40+). */
#define WP_HELP_PAGE_ROWS 12

/* Bounds a single physical source line's word-wrap tokenization --
   help content is short, authored plain text (see help_doc.h), not
   arbitrary user prose, so a fixed stack buffer is the right shape here
   (no dynamic allocation in the paint path at all), matching this
   project's general preference for bounded, non-allocating hot paths.
   A line with more distinct words than this simply wraps using only
   the first WP_HELP_MAX_WRAP_TOKENS of them -- a real, generous-enough
   cap (64 words), not silently mis-rendering realistic content. */
#define WP_HELP_MAX_WRAP_TOKENS 64

/* A single authored source line wraps into at most this many rows --
   generous for any realistic help-content line at any of this
   project's real target column widths (even ST Low's 40 columns).
   Rows beyond this cap are simply not drawn -- an authoring-time
   scope limit, not a runtime crash risk. */
#define WP_HELP_MAX_WRAP_ROWS_PER_LINE 16

/* A link line's rendered form is "> " plus its label (see paint's own
   marker handling below) -- bounded the same way link labels
   themselves should be authored short. */
#define WP_HELP_MARKER_BUF_MAX 300

typedef struct {
    u32 start;
    u32 len;
    wp_bool is_space;
    i32 width_twips;
} help_wrap_token;

static u32 node_link_count(const wp_help_node *node)
{
    u32 i, n = 0;
    for (i = 0; i < node->line_count; i++) {
        if (node->lines[i].kind == (u8)WP_HELP_LINE_LINK) n++;
    }
    return n;
}

/* Returns the line index of the nth (0-based) link-kind line, or
   node->line_count if fewer than n+1 links exist -- callers must check
   the result against line_count before using it as an index. */
static u32 nth_link_line_index(const wp_help_node *node, u32 n)
{
    u32 i, seen = 0;
    for (i = 0; i < node->line_count; i++) {
        if (node->lines[i].kind == (u8)WP_HELP_LINE_LINK) {
            if (seen == n) return i;
            seen++;
        }
    }
    return node->line_count;
}

void wp_help_view_init(wp_help_view_state *vs, const wp_help_doc *doc, u32 start_node)
{
    vs->doc = doc;
    vs->scroll_row = 0;
    vs->selected_link = 0;
    vs->history_count = 0;

    if (doc == NULL || doc->node_count == 0) {
        vs->current_node = WP_HELP_NODE_NONE;
    } else if (start_node < doc->node_count) {
        vs->current_node = start_node;
    } else {
        vs->current_node = 0;
    }
}

static void jump_to_node(wp_help_view_state *vs, u32 node_index)
{
    vs->current_node = node_index;
    vs->scroll_row = 0;
    vs->selected_link = 0;
}

wp_bool wp_help_view_feed_key(wp_help_view_state *vs, wp_key_event key)
{
    const wp_help_node *node;
    u32 link_count;

    if (key.special == (u16)WP_KEY_ESCAPE) return WP_TRUE;
    if (key.special == (u16)WP_KEY_NONE && key.mods == 0 && key.codepoint == 'q') return WP_TRUE;

    if (vs->doc == NULL || vs->current_node == WP_HELP_NODE_NONE) return WP_FALSE;
    node = &vs->doc->nodes[vs->current_node];
    link_count = node_link_count(node);

    if (key.special == (u16)WP_KEY_DOWN ||
        (key.special == (u16)WP_KEY_NONE && key.mods == 0 && key.codepoint == 'j')) {
        if (link_count > 0 && vs->selected_link + 1 < link_count) vs->selected_link++;
        return WP_FALSE;
    }
    if (key.special == (u16)WP_KEY_UP ||
        (key.special == (u16)WP_KEY_NONE && key.mods == 0 && key.codepoint == 'k')) {
        if (vs->selected_link > 0) vs->selected_link--;
        return WP_FALSE;
    }
    if (key.special == (u16)WP_KEY_PAGE_DOWN) {
        vs->scroll_row += WP_HELP_PAGE_ROWS;
        return WP_FALSE;
    }
    if (key.special == (u16)WP_KEY_PAGE_UP) {
        vs->scroll_row = (vs->scroll_row > WP_HELP_PAGE_ROWS) ? vs->scroll_row - WP_HELP_PAGE_ROWS : 0;
        return WP_FALSE;
    }
    if (key.special == (u16)WP_KEY_ENTER) {
        if (link_count > 0) {
            u32 line_idx = nth_link_line_index(node, vs->selected_link);
            if (line_idx < node->line_count) {
                wp_span target_span = node->lines[line_idx].target;
                u32 target = wp_help_doc_find_node(vs->doc,
                    wp_strpool_get(&vs->doc->text, target_span), target_span.length);
                if (target != WP_HELP_NODE_NONE) {
                    if (vs->history_count < WP_HELP_HISTORY_MAX) {
                        vs->history[vs->history_count++] = vs->current_node;
                    }
                    jump_to_node(vs, target);
                }
            }
        }
        return WP_FALSE;
    }
    if (key.special == (u16)WP_KEY_BACKSPACE) {
        if (vs->history_count > 0) {
            u32 prev = vs->history[--vs->history_count];
            jump_to_node(vs, prev);
        }
        return WP_FALSE;
    }
    return WP_FALSE;
}

/* Splits [text, text+len) into alternating word/space tokens, each
   pre-measured once via plat_measure_text -- same reasoning
   engine/src/linebreak.c's own tokenize() gives (measuring growing
   candidate substrings repeatedly, rather than fixed tokens once, is
   both slower and easy to get subtly wrong at row-break boundaries).
   Simplified from linebreak.c's own version: single font (no run
   spanning), plain byte space (0x20) delimiting only -- help content is
   authored plain text, not arbitrary user prose needing Unicode word-
   break rules. */
static u32 tokenize_line(plat_font *font, const u8 *text, u32 len,
                          help_wrap_token *tokens, u32 max_tokens)
{
    u32 count = 0;
    u32 offset = 0;

    while (offset < len && count < max_tokens) {
        u32 seg_start = offset;
        wp_bool seg_is_space = (wp_bool)(text[offset] == ' ');
        i32 width = 0;

        offset++;
        while (offset < len && (wp_bool)(text[offset] == ' ') == seg_is_space) offset++;

        plat_measure_text(font, text + seg_start, offset - seg_start, &width);

        tokens[count].start = seg_start;
        tokens[count].len = offset - seg_start;
        tokens[count].is_space = seg_is_space;
        tokens[count].width_twips = width;
        count++;
    }
    return count;
}

/* Greedy-packs tokens[*tok_pos..) onto one row no wider than
   max_width_twips, advancing *tok_pos past what it consumed. A row
   never starts with a space token (leading whitespace at a wrap point
   is dropped); a space token is only ever included when the word
   following it also fits, matching engine/src/linebreak.c's own
   pending-space-width treatment. A single word wider than
   max_width_twips is still placed alone on its own row rather than
   looping forever (the same single-overlong-word allowance
   linebreak.c's own docs/layout.md documents). Returns WP_FALSE once
   there is nothing left to pack (guaranteed to make progress on every
   WP_TRUE return, so a caller looping on this always terminates). */
static wp_bool pack_next_row(const help_wrap_token *tokens, u32 token_count, u32 *tok_pos,
                              i32 max_width_twips, u32 *row_start, u32 *row_len)
{
    u32 i = *tok_pos;
    u32 start, end;
    i32 width = 0;
    wp_bool have_content = WP_FALSE;

    while (i < token_count && tokens[i].is_space) i++;
    if (i >= token_count) { *tok_pos = i; return WP_FALSE; }

    start = tokens[i].start;
    end = start;

    while (i < token_count) {
        if (!tokens[i].is_space) {
            /* First word on the row is always accepted, even if it
               alone exceeds max_width_twips. */
            width = tokens[i].width_twips;
            end = tokens[i].start + tokens[i].len;
            have_content = WP_TRUE;
            i++;
            continue;
        }
        if (have_content && i + 1 < token_count && !tokens[i + 1].is_space) {
            i32 candidate = width + tokens[i].width_twips + tokens[i + 1].width_twips;
            if (candidate > max_width_twips) break;
            width = candidate;
            end = tokens[i + 1].start + tokens[i + 1].len;
            i += 2;
            continue;
        }
        break;
    }

    *row_start = start;
    *row_len = end - start;
    *tok_pos = i;
    return WP_TRUE;
}

u32 wp_help_wrap_line(plat_font *font, const u8 *text, u32 len, i32 max_width_twips,
                       u32 *out_starts, u32 *out_lens, u32 max_rows)
{
    help_wrap_token tokens[WP_HELP_MAX_WRAP_TOKENS];
    u32 token_count = tokenize_line(font, text, len, tokens, WP_HELP_MAX_WRAP_TOKENS);
    u32 tok_pos = 0;
    u32 row_count = 0;
    u32 row_start, row_len;

    while (row_count < max_rows &&
           pack_next_row(tokens, token_count, &tok_pos, max_width_twips, &row_start, &row_len)) {
        out_starts[row_count] = row_start;
        out_lens[row_count] = row_len;
        row_count++;
    }
    return row_count;
}

void wp_help_view_paint(plat_gc *gc, plat_font *font,
                         i32 x_px, i32 y_px, i32 w_px, i32 h_px,
                         i32 content_width_twips, i32 line_height_px,
                         const wp_help_view_state *vs)
{
    const wp_help_node *node;
    u32 li, row_index, link_seen, max_rows;

    plat_fill_rect(gc, x_px, y_px, w_px, h_px, 0xFF, 0xFF, 0xFF);

    if (vs == NULL || vs->doc == NULL || vs->current_node == WP_HELP_NODE_NONE ||
        vs->current_node >= vs->doc->node_count || line_height_px <= 0) {
        return;
    }
    node = &vs->doc->nodes[vs->current_node];
    max_rows = (u32)(h_px / line_height_px);
    row_index = 0;
    link_seen = 0;

    for (li = 0; li < node->line_count && row_index < vs->scroll_row + max_rows; li++) {
        const wp_help_line *ln = &node->lines[li];
        wp_bool is_link = (wp_bool)(ln->kind == (u8)WP_HELP_LINE_LINK);
        wp_bool is_selected = (wp_bool)(is_link && link_seen == vs->selected_link);
        const u8 *text = wp_strpool_get(&vs->doc->text, ln->text);
        u32 text_len = ln->text.length;
        u8 marker_buf[WP_HELP_MARKER_BUF_MAX];

        if (is_link) {
            u32 n = text_len;
            if (n > (u32)sizeof(marker_buf) - 2) n = (u32)sizeof(marker_buf) - 2;
            marker_buf[0] = '>';
            marker_buf[1] = ' ';
            memcpy(marker_buf + 2, text, n);
            text = marker_buf;
            text_len = n + 2;
            link_seen++;
        }

        if (!is_link && text_len == 0) {
            /* A blank authored line -- one empty row, preserving the
               author's intended paragraph spacing rather than
               collapsing it away. */
            row_index++;
        } else {
            u32 row_starts[WP_HELP_MAX_WRAP_ROWS_PER_LINE];
            u32 row_lens[WP_HELP_MAX_WRAP_ROWS_PER_LINE];
            u32 row_count = wp_help_wrap_line(font, text, text_len, content_width_twips,
                                               row_starts, row_lens, WP_HELP_MAX_WRAP_ROWS_PER_LINE);
            u32 ri;

            for (ri = 0; ri < row_count; ri++) {
                if (row_index >= vs->scroll_row && row_index - vs->scroll_row < max_rows) {
                    i32 py = y_px + (i32)(row_index - vs->scroll_row) * line_height_px;
                    if (is_selected) {
                        plat_fill_rect(gc, x_px, py, w_px, line_height_px, 0xC0, 0xC0, 0xC0);
                    }
                    plat_draw_text(gc, font, x_px, py, text + row_starts[ri], row_lens[ri], 0, 0, 0);
                }
                row_index++;
                if (row_index >= vs->scroll_row + max_rows) break;
            }
        }
    }
}
