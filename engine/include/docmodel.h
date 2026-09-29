/* SPDX-License-Identifier: GPL-2.0-or-later */
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
typedef u16 wp_image;  /* index into wp_document.images; 0 = none/invalid */

/* The low four bits are deliberately the SAME VALUES as platform.h's
   PLAT_STYLE_* , which is what plat_font_open() takes; SUPER and SUB have
   no PLAT_ equivalent because they are a document concern that never
   reaches the font-selection seam. Restated here rather than included from
   platform.h so the document model stays free of the platform seam --
   host/tests/test_platform_style.c fails if the two ever diverge. */
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

/* M15: a paragraph-level, fixed-size placed image. Never holds pixel
   bytes directly -- only this small, always-resident descriptor lives
   in wp_document.images; the real pixel data is streamed from
   blob_path on demand at render time (services/spell/spell.c's
   "descriptor + plat_file_seek/read on demand, never load whole"
   precedent), never wp_strpool_append'd wholesale. See
   docs/document-model.md's Image section. */
typedef struct {
    i32    width_px;              /* stored bitmap's real pixel width */
    i32    height_px;             /* stored bitmap's real pixel height */
    i32    display_width_twips;   /* layout box size, computed by the
                                      INSERTING backend from width_px via
                                      its own twips<->px ratio -- the
                                      engine never computes this itself,
                                      the same division of labor
                                      plat_font_metrics_get's callers
                                      already have */
    i32    display_height_twips;
    u8     palette_count;         /* 1..16 */
    u8     _pad[3];
    u8     palette[16][3];        /* RGB; palette[N] is the color for a
                                      pixel whose packed nibble value
                                      equals N */
    wp_span blob_path;              /* pool ref (offset+length, NOT a
                                        bare wp_str -- the pool never
                                        NUL-terminates, so a length is
                                        the only way to recover this
                                        string later): native path to
                                        the pixel-cache file holding
                                        this image's packed 4-bit pixel
                                        data; offset 0/length 0 = none
                                        (a DOS-only placeholder
                                        paragraph never populates this) */
    wp_span source_name;            /* pool ref, optional; original
                                        filename/caption; offset 0/
                                        length 0 = unset */
} wp_image_desc;

typedef enum {
    WP_PARA_TEXT  = 0,  /* default -- every zero-initialized paragraph is
                            text, so every pre-M15 consumer (linebreak,
                            justify, paginate, every backend's paint
                            loop, every format importer/exporter) stays
                            unaffected without a code change */
    WP_PARA_IMAGE = 1
} wp_para_kind;

typedef struct {
    wp_run   *runs;
    u16       run_count;
    u16       run_capacity;
    wp_pstyle style;
    u8        kind;         /* wp_para_kind; runs[]/run_count stay 0 for
                                an image paragraph */
    wp_image  image;        /* meaningful only when kind == WP_PARA_IMAGE */
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

    wp_image_desc *images;   /* M15; append-only, same as cstyles/pstyles --
                                 never compacted, not even when the last
                                 paragraph referencing an entry is undone */
    u16            image_count;
    u16            image_capacity;

    wp_section *sections;
    u32         section_count;
    u32         section_capacity;

    wp_cstyle default_cstyle;
    wp_pstyle default_pstyle;

    wp_str title;   /* pool ref; 0 = unset */
    wp_str author;  /* pool ref; 0 = unset */

    /* Set by every JOURNALED edit (the wp_edit_* family in undo.c),
       cleared by wp_document_mark_saved. Deliberately NOT set by the raw
       structural functions -- those are what importers build a document
       out of, and a freshly imported document is not "modified", it is
       exactly what is on disk.

       Conservative by design: undoing back to the last saved state still
       reports modified, because the journal has no notion of which node
       was the save point and a pointer to one could dangle when redo
       history is discarded. The failure that matters is not warning when
       there ARE unsaved changes; warning once too often costs a
       keystroke, losing someone's work does not. */
    u8 modified;
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
    /* M15: inserts a paragraph-level image at op->at, built from
       wp_document_split_paragraph_raw + one extra paragraph insert (see
       wp_edit_insert_image). op->at is the pre-insert position, exactly
       like WP_OP_SPLIT_PARA's own convention; op->style_before carries
       the wp_image handle (doc->images is append-only, so undo never
       needs to remove the registered descriptor, only the paragraph
       referencing it -- the same field-reuse trick WP_OP_SPLIT_PARA/
       MERGE_PARA already use to stash a style handle). No separate
       "delete image" op kind exists -- undoing an insert is a
       self-contained inverse (remove the image paragraph, re-merge),
       the same shape WP_OP_SPLIT_PARA's own undo already has. */
    WP_OP_INSERT_IMAGE,
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

/* The unsaved-changes flag above. mark_saved is called after a
   successful save, and after a load or a New -- all three make the
   in-memory document match what a file holds (or holds nothing, for
   New). */
wp_bool   wp_document_is_modified(const wp_document *doc);
void      wp_document_mark_saved(wp_document *doc);
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

/* -- Image table management (M15) --------------------------------------- */
/* No find_or_add here -- unlike a style, two images with byte-identical
   descriptor fields are still distinct content (e.g. the same photo
   inserted twice deliberately), so deduplication isn't worth the
   complexity. Otherwise mirrors the cstyle/pstyle shape exactly: the
   table grows via the shared 1.5x policy, index 0 is always the
   reserved sentinel. */
wp_status wp_document_image_add(wp_document *doc, const wp_image_desc *desc,
                                 wp_image *out);

/* Returns NULL for handle 0 or any handle >= the table's count. */
const wp_image_desc *wp_document_image_get(const wp_document *doc, wp_image handle);

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

/* Takes the paragraph and a run INDEX rather than a wp_run* on purpose.
   A character style carries size_twips and the bold/italic flags, all of
   which change measured text width -- so changing one invalidates the
   paragraph's line breaks and its layout must be rebuilt. A wp_run has
   no way back to its owning paragraph, so the older
   wp_run_cstyle_set(run, style) form physically could not set
   layout_dirty and silently left every caller painting stale lines
   (a font-size change re-measured nothing until some unrelated edit
   happened to dirty the same paragraph). No-op if index is out of
   range. */
void wp_run_cstyle_set(wp_paragraph *para, u16 index, wp_cstyle style);

#endif /* WP_DOCMODEL_H */
