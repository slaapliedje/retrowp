#include <string.h>
#include <stddef.h>
#include "help_doc.h"
#include "platform.h"

/* Private 1.5x growth policy mirroring engine/src/growth_internal.h's
   own -- that header is explicitly private to engine/src (not part of
   the public engine surface platform/common may depend on, per
   CLAUDE.md's layer rule), so this is its own small copy, not a
   layering violation: the same "each including translation unit gets
   its own private copy" intent growth_internal.h's own comment states,
   just outside engine/src this time. */
static u32 help_grow_capacity(u32 current_cap, u32 needed_cap, u32 min_cap)
{
    u32 new_cap = current_cap + (current_cap / 2);
    if (new_cap < needed_cap) new_cap = needed_cap;
    if (new_cap < min_cap)    new_cap = min_cap;
    return new_cap;
}

static wp_status node_table_grow(wp_help_doc *doc, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= doc->node_capacity) return WP_OK;
    new_cap = help_grow_capacity(doc->node_capacity, needed_count, 4);

    new_mem = mem_realloc(doc->nodes,
                           doc->node_capacity * (u32)sizeof(wp_help_node),
                           new_cap * (u32)sizeof(wp_help_node));
    if (new_mem == NULL) return WP_NOMEM;

    doc->nodes = (wp_help_node *)new_mem;
    doc->node_capacity = new_cap;
    return WP_OK;
}

static wp_status line_table_grow(wp_help_node *node, u32 needed_count)
{
    u32 new_cap;
    void *new_mem;

    if (needed_count <= node->line_capacity) return WP_OK;
    new_cap = help_grow_capacity(node->line_capacity, needed_count, 8);

    new_mem = mem_realloc(node->lines,
                           node->line_capacity * (u32)sizeof(wp_help_line),
                           new_cap * (u32)sizeof(wp_help_line));
    if (new_mem == NULL) return WP_NOMEM;

    node->lines = (wp_help_line *)new_mem;
    node->line_capacity = new_cap;
    return WP_OK;
}

static wp_bool is_blank(u8 c)
{
    return (wp_bool)(c == ' ' || c == '\t');
}

/* Strips a trailing '\r' (CRLF tolerance) and trailing spaces/tabs --
   applied to every physical line once, up front, so no later helper
   needs to think about line endings again. */
static u32 rtrim_len(const u8 *s, u32 len)
{
    while (len > 0 && (s[len - 1] == '\r' || is_blank(s[len - 1]))) len--;
    return len;
}

static u32 ltrim_start(const u8 *s, u32 len)
{
    u32 i = 0;
    while (i < len && is_blank(s[i])) i++;
    return i;
}

static wp_bool starts_with(const u8 *s, u32 len, const char *prefix)
{
    u32 plen = (u32)strlen(prefix);
    u32 i;

    if (len < plen) return WP_FALSE;
    for (i = 0; i < plen; i++) {
        if (s[i] != (u8)prefix[i]) return WP_FALSE;
    }
    return WP_TRUE;
}

/* Parses "@node NAME "Title"" (already confirmed to start with "@node").
   The quoted title is optional -- a source line with no `"..."` after
   the name falls back to title == name, so every node always has a
   non-empty display title regardless of how tersely it was authored. */
static void parse_node_directive(const u8 *line, u32 len,
                                  u32 *name_start, u32 *name_len,
                                  u32 *title_start, u32 *title_len)
{
    u32 i = (len >= 6 && line[5] == ' ') ? 6 : 5; /* "@node " vs bare "@node" */
    u32 n_start, n_end, t_start, t_end;

    i += ltrim_start(line + i, len - i);
    n_start = i;
    while (i < len && !is_blank(line[i])) i++;
    n_end = i;

    i += ltrim_start(line + i, len - i);
    if (i < len && line[i] == '"') {
        i++;
        t_start = i;
        while (i < len && line[i] != '"') i++;
        t_end = i;
    } else {
        t_start = n_start;
        t_end = n_end;
    }

    *name_start = n_start; *name_len = n_end - n_start;
    *title_start = t_start; *title_len = t_end - t_start;
}

/* A body line is a link, not plain text, iff its trimmed content is
   exactly `{label|target}` -- the whole line, not an inline span (see
   help_doc.h's own comment on why: no per-character hit-testing needed
   in the pager this way). Returns WP_FALSE (plain text) for anything
   else, including a `{...}` with no `|` separator. */
static wp_bool parse_link_line(const u8 *line, u32 len,
                                u32 *label_start, u32 *label_len,
                                u32 *target_start, u32 *target_len)
{
    u32 start = ltrim_start(line, len);
    u32 end = rtrim_len(line, len);
    u32 i, bar_pos = 0;
    wp_bool found_bar = WP_FALSE;

    if (end < start + 2) return WP_FALSE; /* need room for at least "{}" */
    if (line[start] != '{' || line[end - 1] != '}') return WP_FALSE;

    for (i = start + 1; i < end - 1; i++) {
        if (line[i] == '|') { bar_pos = i; found_bar = WP_TRUE; break; }
    }
    if (!found_bar) return WP_FALSE;

    *label_start = start + 1;
    *label_len = bar_pos - (start + 1);
    *target_start = bar_pos + 1;
    *target_len = (end - 1) - (bar_pos + 1);
    return WP_TRUE;
}

wp_status wp_help_doc_parse(const u8 *source_utf8, u32 source_len, wp_help_doc *out)
{
    u32 pos = 0;
    u32 current_node = WP_HELP_NODE_NONE;
    wp_status st;

    out->nodes = NULL;
    out->node_count = 0;
    out->node_capacity = 0;
    st = wp_strpool_init(&out->text);
    if (st != WP_OK) return st;

    while (pos < source_len) {
        u32 line_start = pos;
        u32 line_len;

        while (pos < source_len && source_utf8[pos] != '\n') pos++;
        line_len = pos - line_start;
        if (pos < source_len) pos++; /* consume the '\n' itself */
        line_len = rtrim_len(source_utf8 + line_start, line_len);

        if (current_node == WP_HELP_NODE_NONE) {
            if (starts_with(source_utf8 + line_start, line_len, "@node ") ||
                (line_len == 5 && starts_with(source_utf8 + line_start, line_len, "@node"))) {
                u32 name_start, name_len, title_start, title_len;
                wp_help_node *node;

                parse_node_directive(source_utf8 + line_start, line_len,
                                      &name_start, &name_len, &title_start, &title_len);

                st = node_table_grow(out, out->node_count + 1);
                if (st != WP_OK) goto fail;

                node = &out->nodes[out->node_count];
                node->lines = NULL;
                node->line_count = 0;
                node->line_capacity = 0;

                st = wp_strpool_append(&out->text, source_utf8 + line_start + name_start,
                                        name_len, &node->name);
                if (st != WP_OK) goto fail;
                st = wp_strpool_append(&out->text, source_utf8 + line_start + title_start,
                                        title_len, &node->title);
                if (st != WP_OK) goto fail;

                current_node = out->node_count;
                out->node_count++;
            }
            /* else: blank line, an @database header, a comment -- silently
               skipped, per this parser's documented leniency. */
        } else if (line_len == 8 && starts_with(source_utf8 + line_start, line_len, "@endnode")) {
            current_node = WP_HELP_NODE_NONE;
        } else {
            u32 label_start, label_len, target_start, target_len;
            wp_help_node *node = &out->nodes[current_node];
            wp_help_line *ln;

            st = line_table_grow(node, node->line_count + 1);
            if (st != WP_OK) goto fail;
            ln = &node->lines[node->line_count];

            if (parse_link_line(source_utf8 + line_start, line_len,
                                 &label_start, &label_len, &target_start, &target_len)) {
                ln->kind = (u8)WP_HELP_LINE_LINK;
                st = wp_strpool_append(&out->text, source_utf8 + line_start + label_start,
                                        label_len, &ln->text);
                if (st != WP_OK) goto fail;
                st = wp_strpool_append(&out->text, source_utf8 + line_start + target_start,
                                        target_len, &ln->target);
                if (st != WP_OK) goto fail;
            } else {
                ln->kind = (u8)WP_HELP_LINE_TEXT;
                st = wp_strpool_append(&out->text, source_utf8 + line_start, line_len, &ln->text);
                if (st != WP_OK) goto fail;
                ln->target.offset = 0;
                ln->target.length = 0;
            }
            node->line_count++;
        }
    }

    /* A node still open at end-of-source (missing its @endnode) is
       implicitly closed -- see help_doc.h's own leniency note. */
    return WP_OK;

fail:
    wp_help_doc_free(out);
    return st;
}

void wp_help_doc_free(wp_help_doc *doc)
{
    u32 i;

    for (i = 0; i < doc->node_count; i++) {
        mem_free(doc->nodes[i].lines);
    }
    mem_free(doc->nodes);
    doc->nodes = NULL;
    doc->node_count = 0;
    doc->node_capacity = 0;
    wp_strpool_free(&doc->text);
}

u32 wp_help_doc_find_node(const wp_help_doc *doc, const u8 *name_utf8, u32 name_len)
{
    u32 i;

    for (i = 0; i < doc->node_count; i++) {
        wp_span sp = doc->nodes[i].name;
        if (sp.length == name_len &&
            memcmp(wp_strpool_get(&doc->text, sp), name_utf8, name_len) == 0) {
            return i;
        }
    }
    return WP_HELP_NODE_NONE;
}
