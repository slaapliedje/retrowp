# Document Model — Single Source of Truth

> This is the canonical in-memory representation. Every format (plaintext, RTF,
> ODT, PDF) imports INTO this and exports FROM this. Layout reads it; editing
> mutates it; the offload client serializes parts of it. No format's quirks leak
> into these structures — formats adapt to the model, never the reverse.

## Design Principles
1. Tree of content + side tables for styles (avoid duplicating style data per run).
2. Text stored as UTF-8 in a per-document string pool; runs reference spans.
3. Indices/handles, not raw pointers, for anything that must be serialized.
4. Fixed-width types everywhere (see types.h). Big-endian on disk/wire.
5. Everything allocatable via platform mem_* ; every struct has init/free.

## Type Foundations (from types.h)
```c
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;
typedef signed short   i16;
typedef signed long    i32;
typedef u8             wp\_bool;   /* 0 / 1 */
#define WP\_FALSE 0
#define WP\_TRUE  1

typedef enum {
    WP\_OK = 0, WP\_ERR = 1, WP\_NOMEM = 2, WP\_RANGE = 3,
    WP\_UNSUPPORTED = 4, WP\_TIMEOUT = 5, WP\_BADFMT = 6
} wp\_status;

/* Handle types: 0 is always the "none/invalid" sentinel. */
typedef u32 wp\_str;    /* index into string pool               */
typedef u16 wp\_cstyle; /* index into character-style table      */
typedef u16 wp\_pstyle; /* index into paragraph-style table      */

typedef struct {
    u8  *bytes;     /* UTF-8, not NUL-terminated internally     */
    u32  length;    /* bytes used                               */
    u32  capacity;  /* bytes allocated                          */
} wp\_strpool;

/* A referenced span within the pool. */
typedef struct {
    wp\_str offset;  /* byte offset into pool                    */
    u32    length;  /* byte length of the span                  */
} wp\_span;

typedef enum {
    WP\_STYLE\_BOLD      = 1 << 0,
    WP\_STYLE\_ITALIC    = 1 << 1,
    WP\_STYLE\_UNDERLINE = 1 << 2,
    WP\_STYLE\_STRIKE    = 1 << 3,
    WP\_STYLE\_SUPER     = 1 << 4,
    WP\_STYLE\_SUB       = 1 << 5
} wp\_style\_flags;

typedef struct {
    wp\_str  font\_name;   /* pool ref; empty = document default   */
    i32     size\_twips;  /* font size in twips                    */
    u16     flags;       /* wp\_style\_flags bitfield               */
    u8      color\_r;     /* text color; default 0,0,0             */
    u8      color\_g;
    u8      color\_b;
    u8      _pad;
} wp\_char\_style;

typedef enum {
    WP\_ALIGN\_LEFT = 0, WP\_ALIGN\_CENTER = 1,
    WP\_ALIGN\_RIGHT = 2, WP\_ALIGN\_JUSTIFY = 3
} wp\_align;

typedef struct {
    u8   align;            /* wp\_align                            */
    u8   _pad[3];
    i32  indent\_left;      /* twips                               */
    i32  indent\_right;     /* twips                               */
    i32  indent\_first;     /* first-line indent, twips (may be <0)*/
    i32  space\_before;     /* twips                               */
    i32  space\_after;      /* twips                               */
    i32  line\_spacing;     /* twips; 0 = single/auto              */
    wp\_str style\_name;     /* named style ("Heading 1"), optional */
} wp\_para\_style;

typedef struct {
    wp\_span   text;    /* span into string pool                  */
    wp\_cstyle style;   /* character style index                  */
} wp\_run;

typedef u16 wp\_image;  /* index into wp\_document.images; 0 = none/invalid */

typedef enum {
    WP\_PARA\_TEXT  = 0,  /* default -- every zero-initialized paragraph is
                            text, so every pre-M15 consumer stays
                            unaffected without a code change */
    WP\_PARA\_IMAGE = 1
} wp\_para\_kind;

typedef struct {
    wp\_run   *runs;
    u16       run\_count;
    u16       run\_capacity;
    wp\_pstyle style;      /* paragraph style index               */
    u8        kind;       /* wp\_para\_kind; runs[]/run\_count stay 0
                              for an image paragraph              */
    wp\_image  image;      /* meaningful only when kind == WP\_PARA\_IMAGE */
    /* Layout cache (not serialized): */
    void     *layout;     /* opaque, owned by layout engine      */
    wp\_bool   layout\_dirty;
} wp\_paragraph;

typedef struct {
    i32  page\_width;      /* twips                                */
    i32  page\_height;     /* twips                                */
    i32  margin\_left;
    i32  margin\_right;
    i32  margin\_top;
    i32  margin\_bottom;
    wp\_paragraph *paras;
    u32           para\_count;
    u32           para\_capacity;
} wp\_section;

typedef struct {
    wp\_strpool     strings;

    wp\_char\_style *cstyles;      /* character style table         */
    u16            cstyle\_count;
    u16            cstyle\_capacity;

    wp\_para\_style *pstyles;      /* paragraph style table         */
    u16            pstyle\_count;
    u16            pstyle\_capacity;

    wp\_image\_desc *images;       /* placed-image descriptor table  */
    u16            image\_count;
    u16            image\_capacity;

    wp\_section    *sections;
    u32            section\_count;
    u32            section\_capacity;

    /* Document defaults */
    wp\_cstyle      default\_cstyle;
    wp\_pstyle      default\_pstyle;

    /* Metadata (pool refs; 0 = unset) */
    wp\_str         title;
    wp\_str         author;
} wp\_document;

/* A fully-qualified location in the document tree. */
typedef struct {
    u32 section;    /* index into document.sections            */
    u32 paragraph;  /* index into section.paras                */
    u32 run;        /* index into paragraph.runs               */
    u32 offset;     /* BYTE offset within the run's text span  */
                    /* (UTF-8; must land on a codepoint boundary)*/
} wp\_pos;

typedef struct {
    wp\_pos anchor;  /* where selection started                 */
    wp\_pos caret;   /* where cursor is now                     */
} wp\_selection;     /* empty selection: anchor == caret        */

typedef enum {
    WP\_OP\_INSERT\_TEXT, WP\_OP\_DELETE\_TEXT,
    WP\_OP\_SET\_CSTYLE,  WP\_OP\_SET\_PSTYLE,
    WP\_OP\_SPLIT\_PARA,  WP\_OP\_MERGE\_PARA,
    WP\_OP\_INSERT\_IMAGE, /* see Images section below */
    WP\_OP\_GROUP\_BEGIN, WP\_OP\_GROUP\_END
} wp\_op\_kind;

typedef struct wp\_op {
    u8       kind;         /* wp\_op\_kind                         */
    wp\_pos   at;           /* where it applied                   */
    wp\_span  text;         /* affected text (for insert/delete)  */
    u16      style\_before; /* for style ops                      */
    u16      style\_after;
    struct wp\_op *next;    /* journal linkage                    */
} wp\_op;

## Images (M15)

An image is a **paragraph-level, fixed-size, non-flowing block** -- not
a run-level inline object. `wp_paragraph.kind` (`wp_para_kind`) is `0`
(`WP_PARA_TEXT`) for every paragraph that existed before this
milestone and every paragraph any pre-M15 code path still creates --
that zero-init default is what keeps every unmodified format
importer/exporter, and every backend that hasn't been taught about
`WP_PARA_IMAGE`, correctly inert. When `kind == WP_PARA_IMAGE`, the
paragraph's `runs`/`run_count` stay at their zero-init values and
`image` names an entry in `wp_document.images` instead.

`wp_document.images` is a fourth append-only side table, mirroring
`cstyles`/`pstyles` exactly: 1.5x growth, index 0 reserved as the
sentinel, never compacted (an entry stays registered even after the
last paragraph referencing it is undone -- undo only ever needs to
remove/re-add the *paragraph*, never the descriptor). Unlike style
tables there is no `_find_or_add` -- two images with byte-identical
descriptor fields are still distinct content (the same photo inserted
twice deliberately), so de-duplication isn't worth the complexity.

`wp_image_desc` is deliberately small and always resident (unlike a
`wp_run`'s text, which lives in the string pool): `width_px`/
`height_px` (the real decoded bitmap size), `display_width_twips`/
`display_height_twips` (the layout box size -- computed by the
*inserting backend* from `width_px` via its own twips<->px ratio, the
same division of labor `plat_font_metrics_get`'s callers already have;
the engine itself never computes this), a `palette[16][3]` of up to 16
RGB entries plus `palette_count`, and two `wp_span` pool refs:
`blob_path` (the native path to a disk-cached file holding the actual
packed 4-bit pixel bytes) and `source_name` (the original filename/
caption, optional). **Pixel bytes themselves are never held in memory
by the document model** -- only `blob_path` names where to stream them
from at render time, the same "descriptor + read-on-demand, never load
the whole thing" precedent `services/spell/spell.c` already set for a
large dictionary. `blob_path` offset 0/length 0 means no pixel cache
exists at all -- DOS is the one backend that legitimately leaves this
unset (T15.6: it has no addressable graphics to paint pixels into at
all, so it never reads a cache file back, only the two integer
dimensions and `source_name` for its bracketed text marker).

Undo reuses `WP_OP_SPLIT_PARA`/`WP_OP_MERGE_PARA`'s own trick of
stashing a handle in the 16-bit `style_before` field rather than
growing `wp_op`: `WP_OP_INSERT_IMAGE` carries the just-registered
`wp_image` handle there. There is no separate "delete image" op kind --
undoing an insert is a self-contained inverse (remove the image
paragraph, re-merge the split), the same shape `WP_OP_SPLIT_PARA`'s own
undo already has. Because the images table is never compacted, redoing
the insert afterward can safely reuse the original handle rather than
re-registering a new descriptor.

**Consumers that must be paragraph-kind-aware**: `wp_paragraph_relayout`
(`engine/src/linebreak.c`) branches on `kind` before doing anything
else -- an image paragraph gets exactly one line, sized straight from
the descriptor's own display box, with `tokenize()` never invoked and
`content_width_twips` unused (no text-wrap-around-image in this
milestone). `wp_line_next_fragment`/`wp_justify_space_extra_twips`
(`engine/src/justify.c`) both guard on `kind == WP_PARA_IMAGE` and
return immediately (nothing to fragment, nothing to justify) --
guarded once in the shared primitive rather than trusting every
per-backend paint loop to remember it independently. See
`docs/layout.md`'s own paragraph-kind section for the full layout-side
picture. `engine/src/paginate.c` needed **zero changes** -- it only
ever reads a line's already-computed `height_twips` when deciding page
breaks, and an image paragraph's single line has one like any other.

