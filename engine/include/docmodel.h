#ifndef WP_DOCMODEL_H
#define WP_DOCMODEL_H

#include "types.h"
#include "strpool.h"

/* The document model: tree of content (document -> sections -> paragraphs
   -> runs) plus side tables for styles, per docs/document-model.md. This
   is the single source of truth (CLAUDE.md rule 2) -- every format is an
   importer/exporter around this. */

typedef u16 wp_cstyle; /* index into wp_document.cstyles; 0 = none/invalid */
typedef u16 wp_pstyle; /* index into wp_document.pstyles; 0 = none/invalid */

typedef enum {
    WP_STYLE_BOLD      = 1 << 0,
    WP_STYLE_ITALIC    = 1 << 1,
    WP_STYLE_UNDERLINE = 1 << 2,
    WP_STYLE_STRIKE    = 1 << 3,
    WP_STYLE_SUPER     = 1 << 4,
    WP_STYLE_SUB       = 1 << 5
} wp_style_flags;

typedef struct {
    wp_str font_name;  /* pool ref; 0 (empty) = document default */
    i32    size_twips;
    u16    flags;      /* wp_style_flags bitfield */
    u8     color_r;
    u8     color_g;
    u8     color_b;
    u8     _pad;
} wp_char_style;

typedef enum {
    WP_ALIGN_LEFT = 0, WP_ALIGN_CENTER = 1, WP_ALIGN_RIGHT = 2, WP_ALIGN_JUSTIFY = 3
} wp_align;

typedef struct {
    u8  align;          /* wp_align */
    u8  _pad[3];
    i32 indent_left;
    i32 indent_right;
    i32 indent_first;   /* first-line indent; may be negative */
    i32 space_before;
    i32 space_after;
    i32 line_spacing;   /* 0 = single/auto */
    wp_str style_name;  /* named style ("Heading 1"), optional */
} wp_para_style;

typedef struct {
    wp_span   text;
    wp_cstyle style;
} wp_run;

typedef struct {
    wp_run   *runs;
    u16       run_count;
    u16       run_capacity;
    wp_pstyle style;
    void     *layout;       /* opaque layout cache, not serialized; unused before M3 */
    wp_bool   layout_dirty;
} wp_paragraph;

typedef struct {
    i32 page_width;
    i32 page_height;
    i32 margin_left;
    i32 margin_right;
    i32 margin_top;
    i32 margin_bottom;
    wp_paragraph *paras;
    u32           para_count;
    u32           para_capacity;
} wp_section;

typedef struct {
    wp_strpool strings;

    wp_char_style *cstyles;
    u16            cstyle_count;
    u16            cstyle_capacity;

    wp_para_style *pstyles;
    u16            pstyle_count;
    u16            pstyle_capacity;

    wp_section *sections;
    u32         section_count;
    u32         section_capacity;

    wp_cstyle default_cstyle;
    wp_pstyle default_pstyle;

    wp_str title;   /* pool ref; 0 = unset */
    wp_str author;  /* pool ref; 0 = unset */
} wp_document;

/* -- M2 (editing) shells --------------------------------------------- */
/* Types match document-model.md exactly so a later editing milestone
   doesn't have to change these definitions. Nothing in M1 constructs or
   consumes them yet. */

typedef struct {
    u32 section;
    u32 paragraph;
    u32 run;
    u32 offset;   /* byte offset within the run's text span; must land on
                     a codepoint boundary */
} wp_pos;

typedef struct {
    wp_pos anchor;
    wp_pos caret;
} wp_selection;   /* empty selection: anchor == caret */

typedef enum {
    WP_OP_INSERT_TEXT, WP_OP_DELETE_TEXT, WP_OP_SET_CSTYLE, WP_OP_SET_PSTYLE,
    WP_OP_SPLIT_PARA,  WP_OP_MERGE_PARA,
    /* M8.5: markers, not edits -- bracket a run of ops on the undo stack
       so wp_edit_undo/redo treats them as one user-facing step. Carry no
       payload (at/text/style_before/style_after all unused). See
       docs/editing.md's "Undo Grouping" section. */
    WP_OP_GROUP_BEGIN, WP_OP_GROUP_END
} wp_op_kind;

typedef struct wp_op {
    u8       kind;          /* wp_op_kind */
    wp_pos   at;
    wp_span  text;
    u16      style_before;
    u16      style_after;
    struct wp_op *next;     /* journal linkage */
} wp_op;

/* -- Lifecycle (T1.1) -------------------------------------------------- */

/* Initializes an empty document: an empty string pool, empty style and
   section tables, and one usable default character style and paragraph
   style (registered at table index 1 -- index 0 is always the reserved
   sentinel). default_cstyle/default_pstyle are set to those handles. */
wp_status wp_document_init(wp_document *doc);
void      wp_document_free(wp_document *doc);

/* -- Style table management (T1.1) ------------------------------------- */
/* Registers a new style definition and returns its handle. Tables grow
   via the shared 1.5x policy; a table's index 0 is always the reserved
   sentinel (never returned as a real handle). */
wp_status wp_document_cstyle_add(wp_document *doc, const wp_char_style *style,
                                  wp_cstyle *out);
wp_status wp_document_pstyle_add(wp_document *doc, const wp_para_style *style,
                                  wp_pstyle *out);

/* Returns NULL for handle 0 or any handle >= the table's count. */
const wp_char_style *wp_document_cstyle_get(const wp_document *doc, wp_cstyle handle);
const wp_para_style *wp_document_pstyle_get(const wp_document *doc, wp_pstyle handle);

/* Like *_add, but first does a linear scan (index 1..count-1, skipping
   the reserved sentinel) for a field-by-field-identical existing style
   and reuses its handle instead of appending a duplicate. For a UI that
   toggles attributes on and off repeatedly (bold on, bold off, bold on,
   ...), *_add alone would grow the table by one entry per toggle,
   eventually risking the table's own documented 65535-handle ceiling
   over a long editing session; this makes "toggle back to a style
   already in use" a no-op on the table instead. Compares fields
   explicitly rather than via memcmp so it doesn't depend on callers
   having zeroed any struct padding. */
wp_status wp_document_cstyle_find_or_add(wp_document *doc, const wp_char_style *style,
                                          wp_cstyle *out);
wp_status wp_document_pstyle_find_or_add(wp_document *doc, const wp_para_style *style,
                                          wp_pstyle *out);

/* -- Structural mutation / iteration (T1.2) ---------------------------- */
/* Each *_add function appends one element to an append-only array and
   returns its index via out_index. Because these arrays grow by
   reallocation, a pointer obtained from a *_get accessor below is
   invalidated by the NEXT *_add call on that SAME array -- always
   re-fetch via *_get immediately before use rather than holding a
   pointer across a mutation. */
wp_status wp_document_section_add(wp_document *doc, u32 *out_index);
wp_status wp_section_paragraph_add(wp_section *section, u32 *out_index);
wp_status wp_paragraph_run_add(wp_paragraph *para, wp_span text, wp_cstyle style,
                                u16 *out_index);

wp_section   *wp_document_section_get(wp_document *doc, u32 index);
wp_paragraph *wp_section_paragraph_get(wp_section *section, u32 index);
wp_run       *wp_paragraph_run_get(wp_paragraph *para, u16 index);

void wp_paragraph_pstyle_set(wp_paragraph *para, wp_pstyle style);
void wp_run_cstyle_set(wp_run *run, wp_cstyle style);

#endif /* WP_DOCMODEL_H */
