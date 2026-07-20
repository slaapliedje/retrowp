#ifndef WP_HELP_DOC_H
#define WP_HELP_DOC_H

#include "types.h"
#include "strpool.h"

/* Portable help-document format (M8.5 Phase 10) -- a small subset of
   AmigaGuide's own real syntax (`@node NAME "Title"` / `@endnode`,
   cross-reference links), deliberately shaped close enough that a
   future AmigaGuide export is a mechanical per-line translation of the
   node/link GRAPH, not a redesign: only the inline link markup differs
   (`{label|target}` here vs AmigaGuide's `@{"label" Link target}`).
   See docs/help.md for the full grammar and worked examples.

   Build discipline: this file (and help_doc.c) may call platform.h
   functions but must NEVER #include a native header (gem.h,
   exec/types.h, MacTypes.h, dos.h). Enforced by construction, not just
   convention -- platform/common/ is compiled by EVERY target's own
   toolchain (host cc, the Atari m68k cross-compiler, and eventually
   Amiga/Mac/DOS cross-compilers); a stray native #include breaks the
   host build immediately, since no such header exists there.

   All text (node names, titles, body-line text, link labels/targets)
   lives in ONE shared wp_strpool, referenced via wp_span the same way
   the engine's own document model stores run text (engine/include/
   strpool.h) -- reused directly rather than reinvented, since a help
   doc's storage need (append-only UTF-8 bytes, referenced by offset+
   length) is identical in shape. */

typedef enum {
    WP_HELP_LINE_TEXT = 0,  /* .text is the line's plain body text */
    WP_HELP_LINE_LINK       /* .text is the link's clickable label, .target is the destination node's name */
} wp_help_line_kind;

typedef struct {
    u8      kind;    /* wp_help_line_kind */
    wp_span text;
    wp_span target;  /* WP_HELP_LINE_LINK only; zeroed (offset 0, length 0) for TEXT lines */
} wp_help_line;

typedef struct {
    wp_span name;    /* the identifier other nodes' links target */
    wp_span title;   /* display title; falls back to a copy of name's text if the source omits one */
    wp_help_line *lines;
    u32 line_count;
    u32 line_capacity;
} wp_help_node;

typedef struct {
    wp_strpool text;
    wp_help_node *nodes;
    u32 node_count;
    u32 node_capacity;
} wp_help_doc;

#define WP_HELP_NODE_NONE 0xFFFFFFFFu

/* Parses source_utf8 into a fresh wp_help_doc. Lenient by design, same
   spirit as format_txt_import/format_rtf_import: unrecognized top-level
   content (blank lines, an `@database` line, anything before the first
   `@node`) is silently skipped rather than rejected, and a node missing
   its closing `@endnode` at end-of-source is implicitly closed rather
   than treated as an error -- a help file is authored content, not a
   user-facing document format with round-trip fidelity requirements, so
   robustness against a hand-edited typo matters more than strictness.
   Returns WP_NOMEM only on real allocation failure (calls
   wp_help_doc_free itself first in that case -- out is left empty, not
   partially populated). */
wp_status wp_help_doc_parse(const u8 *source_utf8, u32 source_len, wp_help_doc *out);
void wp_help_doc_free(wp_help_doc *doc);

/* Byte-exact match against each node's name span. Returns
   WP_HELP_NODE_NONE if not found -- callers (the pager) treat a broken
   link target this way rather than crashing on a typo in hand-authored
   source. */
u32 wp_help_doc_find_node(const wp_help_doc *doc, const u8 *name_utf8, u32 name_len);

#endif /* WP_HELP_DOC_H */
