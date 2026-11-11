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

## Paragraph borders and shading (M16, T16.1)

`wp_para_style` carries a border: `border_sides` (`WP_BORDER_TOP`,
`_BOTTOM`, `_LEFT`, `_RIGHT`; `WP_BORDER_BOX` is all four), one
`border_kind` (`WP_BORDER_SINGLE`, `_DOUBLE`, `_THICK`), one
`border_width` (a rule, in twips; 0 is `WP_BORDER_DEFAULT_WIDTH`, 15)
and one `border_space` (twips between the text and the rules) for every
side, and `shading` (percent grey behind the paragraph, 0 for none).
Two styles that differ only in their border are different styles
(`wp_document_pstyle_find_or_add` compares every field).

**Each paragraph rules its own sides.** A box around three paragraphs is
the first ruled top, left and right, the middle left and right, the last
bottom, left and right. So layout never looks at a paragraph's
neighbours. RTF writes a boxed section the other way round, the same
`\box` on every paragraph, and Word draws a run of paragraphs with the
same borders and indents as one box; the importer turns such a run into
the model's form (`rtf_join_border_groups`), and the exporter writes each
paragraph's own sides, which Word draws as the same box.

Borders sit where Word puts them: the text keeps its indents, the rules
stand outside it by `border_space`, and space before and after lies
outside the box. Format > Borders also moves a boxed paragraph's text in
by the rule and its spacing, so the box stays inside the column.

## Tables (M16, T16.2) -- design

Written before the code (T16.2a); the code follows it, and where it
departs this section is corrected, not left behind.

### The model: RTF's flat shape

A table is not a nested object. As in RTF, it is ordinary paragraphs
that say where they belong, so the one paragraph list -- which editing,
undo, search, every importer and exporter and every paint loop walk --
stays one list.

- `wp_paragraph.trow` -- 0 for a paragraph outside any table; else a
  handle into `doc->trows`, the row definition the paragraph's row uses.
- `wp_paragraph.tflags` -- `WP_TCELL_END` on a cell's last paragraph,
  `WP_TROW_END` (with `WP_TCELL_END`) on a row's last. A row is the
  run of table paragraphs up to and including a `WP_TROW_END`; its cells
  are the runs up to each `WP_TCELL_END`. A cell's number is its count
  of `WP_TCELL_END`s from the row's start, so no paragraph stores one
  and inserting a paragraph into a cell renumbers nothing.
- `wp_table_row` (in `doc->trows`, append-only like the other side
  tables, found-or-added so a thousand alike rows share one): `left`
  (`\trleftN`, the row's left edge from the column's), `gap`
  (`\trgaphN`, half the space between cells: text sits `gap` in from
  each cell edge), `cell_count` and `cell_right[]` (`\cellxN`, each
  cell's right edge from the column's left edge, up to
  `WP_MAX_CELLS` = 16), and per cell its borders (`WP_BORDER_*` sides,
  one kind and width as a paragraph's) and shading.

A row definition with more cells than a row has paragraphs-cells is
legal (the rest are empty); a row with more cells than its definition
puts the extra cells after the last edge, one default width each.

### Layout

- **Width.** A table paragraph wraps at its cell's text width:
  `cell_right[k] - cell_left[k] - 2 * gap`, where `cell_left[0]` is
  `left` and `cell_left[k]` is `cell_right[k-1]`. The two relayout
  walks (`wp_pageview_relayout`, `wp_document_paginate`) already go
  through paragraphs in order, so they know the row and cell as they go
  (`wp_paragraph_wrap_twips` answers it for anyone else, by walking
  back to the row's start -- a row is a few paragraphs).
- **Placement.** Every `wp_line` gains three fields, set by a table pass
  that `wp_document_mark_pages` runs first (it walks every line anyway):
  - `cell_x0`, `cell_x1` -- the cell's edges, from the column's left
    edge; 0 and 0 outside a table. A painter draws a line at
    `column + cell_x0 + x_twips` and clears only `[cell_x0, cell_x1)`
    behind it, not the whole width, or it would wipe the cell beside.
  - `advance_twips` -- how far down the next line starts, which is what
    every walker (painters, viewport, hit test, page breaks) now adds,
    instead of `height_twips`. Outside a table it equals the height. In
    a row, each cell's lines advance normally except the cell's last,
    which returns to the row's top -- or, for the row's last cell, goes
    to the row's bottom, the tallest cell's height below its top. So a
    row's advances sum to its height, and a walker that knows nothing
    of tables still lands on the right line after it.
  - `WP_LINE_ROW_FIRST` in a new `flags` byte on a row's first line, so
    a painter draws the row's cell borders and shading there (the row's
    cells and height from `wp_table_row_geometry`), and the page breaker
    keeps a row on one page: a row that does not fit starts a new page
    (a row taller than a page is cut, a first cut).
- `height_twips` keeps meaning the line's own box -- what the caret,
  selection and paragraph borders use.

### Hit testing and the caret

A click's y can fall on lines of several cells; the line chosen is the
one whose cell's `[cell_x0, cell_x1)` holds the click's x. The caret
and selection are per line, as now, at `cell_x0 + x`.

### Editing

Tab moves to the next cell's start (Shift+Tab the previous); at the
last cell, Tab adds a row. Enter splits a paragraph inside its cell: the
new paragraph takes the row, and the cell and row ends move to whichever
of the two is last. Backspace at a cell's start does not join it to the
cell before. Insert Table makes rows x columns of empty cells, evenly
across the column; insert and delete row and column edit the run and the
row definitions. Undo journals the paragraph fields with the text, as
it does the style.

### RTF

`\trowd \trleftN \trgaphN \cellxN \clbrdrt \clbrdrb \clbrdrl \clbrdrr`
(with the border words as for paragraphs) `\clshdngN \intbl \cell \row`
in, and the same out. Nested tables (`\itap`) come in flattened.

### Stages

1. The model, RTF in and out, layout and drawing on every screen and
   the page printer: a Word table opens, shows and prints through GDOS.
2. Editing: Tab, Enter, Backspace, Insert Table, rows and columns.
3. Character printers (`format_prn`): rows laid out side by side in the
   grid, cell rules in box-drawing characters. Until then a table prints
   there as its cells one after another.

## Frames (M16, T16.3)

A frame is a text box at a fixed place on the page -- a form's labelled
field, a sidebar. As with tables, it is ordinary paragraphs that say
where they belong, keeping the one paragraph list:

- `wp_paragraph.frame` -- 0 outside a frame, else a handle into
  `doc->frames`; consecutive paragraphs with the same handle are one
  frame's text, stacked top to bottom.
- `wp_frame` (found-or-added, append-only): `x`, `y`, `w`, `h` in twips
  (`\posxN \posyN \abswN \abshN`; h 0 is as tall as the text, a
  positive h at least that, negative exactly -h -- kept and saved, but
  layout does not use h yet: a frame is as tall as its text), what x is measured
  from (`\phpg` the paper's edge, `\phmrg` the margin, the default)
  and what y is measured from (`\pvpg` the paper's top, `\pvmrg` the
  top margin, `\pvpara` the paragraph the frame follows, the default).

Layout: a frame's paragraphs wrap at its width. The table pass
(`wp_document_mark_pages`) also places frame lines: each line gains
`page_y_twips` (its top below the page's top margin -- for every line,
not only frames), and a frame line `WP_LINE_FRAME`, its frame's text
edges in `cell_x0`/`cell_x1`, its offset in the frame in `row_dy_twips`,
the frame's top below the page's top margin in `frame_top_twips`, and an
advance of 0 -- a frame takes no room in the text, which does not flow
round it (a first cut). A frame belongs to the page of the paragraph it
follows. The line walker finds each page's top from the last ordinary
line (its top less `page_y_twips`) and puts a frame line at that top
plus `frame_top_twips` and its offset. A frame's border is its
paragraphs' own (T16.1); RTF import joins boxed paragraphs into one box
only within one frame, so two frames side by side are two boxes. The
viewport gives frame lines no height. Painters draw each frame line like
any other; where a document has frames, the ST clears the view once
rather than a strip behind each line, which would wipe a frame drawn
earlier (the Amiga and the Mac clear once anyway).

Scrolling: a painter skips the lines above the view, and a frame's lines
follow their paragraph, so a frame whose paragraph has scrolled off
would vanish while it still sits lower on the page. The walk is started
with `wp_line_walk_start_view`, which takes the page's top from the view's
first ordinary line (its own `page_y_twips`), and a painter still draws
a skipped frame line on that page. A frame line that would start above
the view or below it is not drawn at all -- it would land on the ruler
bar or the status line.

First cut, still to come: the text does not flow round a frame; a click
lands on the main text, not in a frame (the caret reaches frame text
from the keyboard); there is no command to make a frame -- frames come
from RTF; character printers print a frame's paragraphs where they fall
in the text, at the full width.

## Bitmaps read on the machine (M16, T16.5)

Insert Image reads the common bitmap formats itself, with no gateway:
engine/src/bitmapfile.c takes GEM .IMG (mono, and XIMG colour), IFF
ILBM (ByteRun1, up to 8 planes, Extra-Half-Brite) and Deluxe Paint's PBM,
MacPaint (raw or in MacBinary; known by its name or a PNTG MacBinary
type, having no signature), PCX (1/2/4/8 bits, EGA planes, 24-bit) and
BMP (1/4/8/24/32 bits, uncompressed). PICT, HAM, RLE BMP: not yet.

No image is held whole -- the 1 MB ST rules that out. Each format reads
a row at a time, twice: the first pass counts its colours (a direct-
colour file's are put on a 6x6x6 cube), the second maps them to the most
used, as many as the machine shows (the darkest and lightest kept
whatever their counts -- line art's ink and paper), and shrinks the rows
to the machine's largest picture, each output pixel the darkest of the
block it stands for so that thin lines survive. The packed rows go out
through an image sink into the picture cache, as RTF import's do.

The cache is shared now: platform/common/imgcache.c gives every backend
a sink (and, for saving, a source) over files it names itself, from the
counter its Insert Image already used. The Amiga and the Mac open RTF
with it and save through it, so their pictures survive a round trip,
which they did not before; DOS saves through it but opens without one
(docs/dos-platform.md). The shared hook, `wp_action_insert_picture_file`,
takes a `wp_picture_target` -- the sink, the colours, the largest size,
the backend's pixels-to-twips pair -- the same limits each backend's
Insert Image asks the gateway for (the Mac asks for 16 colours here,
since its picture painter dithers them).

## Drawings (M16, T16.4)

A drawing is scalable art -- a heading's flourish, a form's rules, a
logo -- kept as a list of primitives rather than pixels, so it is drawn
as sharp as the device it lands on: a screen, a 300 dpi laser.

It is a picture paragraph (`WP_PARA_IMAGE`) like any other -- a fixed
box, its own undo, one layout line, a placeholder where a machine can't
draw it -- whose descriptor carries a `drawing` span instead of a pixel
cache. For a drawing, `width_px`/`height_px` are its own coordinate
extent; `display_width_twips`/`display_height_twips` are the box as
always; `blob_path` and the palette are unused. `WP_IMAGE_IS_DRAWING`
tells the two apart. The primitive list is small and stays resident in
the string pool, as a run's text does; drawing.h gives its encoding --
pen, brush, polyline, polygon, rectangle, rounded rectangle, ellipse,
arc, big-endian, in the drawing's own coordinates -- and a Windows
metafile's defaults (a one-pixel black pen, a white brush), so one
round-trips unchanged.

`wp_drawing_render` flattens the list for a device: scaled to the box,
curves into enough straight segments to look round at that size (from 8
to 64 to an ellipse, by a sine table in 256ths of a turn -- no floating
point on the targets), handed to a sink as one-pixel polylines and
filled polygons. A wider pen is stroked into polygons here, in the
engine, where both of the box's scales are known: the pen is an ellipse
as many pixels across and down as they make it, so a stroke is as thick
one way as the other on a screen whose pixels are not square (Amiga
hires, ST Medium) -- each segment a quadrilateral, an octagon at every
joint and end.

Drawing it (stage 2): retroplat's `plat_fill_polygon` and
`plat_draw_polyline` (platform.h, "Vector drawing"), and
platform/common/drawpaint.c's `wp_paint_drawing`, which every screen
painter and the page printer call for a drawing's picture paragraph --
the page printer at the printer's own resolution. A mid grey fills in
the device's pattern, as shading does, so a mono screen and a printer
keep it. Backends: the ST fills with `v_fillarea` (by spans under
SpeedoGDOS, whose FX80 driver crashes on it -- docs/atari-platform.md),
the Mac with QuickDraw polygons, the Amiga by spans through `RectFill`
(an area fill wants a screen-sized TmpRas in chip memory), Qt and win32
natively; DOS shows "[drawing]", as it shows a picture's marker.

Getting drawings in (stage 3): Insert Image looks at the file first
(`wp_action_insert_picture_file`, platform/common/actions.c). A GEM
metafile or a Windows metafile is read on the machine by
engine/src/drawfile.c and inserted as a drawing at its own size (the
file's page size, or a placeable header's units per inch; three inches
across where it says none), made to fit the column -- one undo step. An
SVG goes to the gateway (OFFLOAD_DRAWING), which answers with a Windows
metafile read the same way. Anything else is a picture, as before.

The GEM reader (the Atari Compendium, Appendix C) plays back polylines,
filled areas, the GDPs (bar, arc, pie, circle, ellipse, elliptical arc
and pie, rounded boxes) and Beziers (v_bez, v_bez_fill: flattened, eight
segments to a cubic, each subpath on its own), with line and fill
colours from the VDI palette, vs_color (256 entries) and NVDI's RGB call
(opcode 138: sub-ID 1 the fill, 2 the line); a fill pattern is a lighter
shade of its colour, a perimeter a hairline. The coordinate system comes
from the header (y measured down from its upper-right corner's y, either
way round), the extents give the drawing's box. Files are little-endian
(even an ST's); the header-length word says which. Text, markers and
line styles are skipped. Tested against Kandinsky's samples (the tiger,
a 747, Columbia), Papyrus's and BLOWUP's.

PostScript printing draws a drawing as paths (docs/formats/prn.md).
On a device of fewer than 16 colours -- mono, ST Medium, a printer,
Workbench's four pens, the one-bit Mac -- every mid colour fills as a
pattern by its brightness (retroplat's `plat_luma_percent`), else orange
would print black; richer screens pattern only greys.

RTF carries a drawing as a Windows metafile (docs/formats/rtf.md,
"Drawings"). engine/src/format_wmf.c reads SetWindowOrg/Ext (the origin
moved to 0, a negative extent flipping the axis), CreatePenIndirect,
CreateBrushIndirect (hatched brushes as a half-tone of their colour),
SelectObject, DeleteObject (and the other object-making records, so the
numbering stays right), MoveTo/LineTo (a run of LineTos as one line),
Polyline, Polygon, PolyPolygon (each polygon filled on its own -- holes
are filled over), Rectangle, RoundRect, Ellipse and Arc; Pie and Chord
are drawn as their arc. Text, bitmaps, clipping and the rest are
skipped. It writes the same records back.

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

