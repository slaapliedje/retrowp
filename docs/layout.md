# Layout Engine (M3)

> Covers word wrap and pagination against the currently-unused
> `wp_paragraph.layout`/`layout_dirty` fields (document-model.md).
> Read this before touching `engine/src/linebreak.c` or
> `engine/src/paginate.c`.

## Prime Directive Reminder

CLAUDE.md rule 4: layout code reaches font metrics **only** through
`platform.h` (`plat_font_open`, `plat_font_metrics_get`,
`plat_measure_text`, `plat_font_close`) -- never a platform font API
directly, never hardcoded metrics. Every width/height number in this
module comes from those four calls.

## Data Structures

`wp_paragraph.layout` (currently `void*`) points to a
`wp_paragraph_layout`:

```c
typedef struct {
    u32 start_run;    /* line's content starts here (inclusive) */
    u32 start_offset;
    u32 end_run;       /* line's content ends here (exclusive) */
    u32 end_offset;
    i32 width_twips;   /* measured width of this line's content */
    i32 height_twips;  /* max (ascent+descent+leading) across the line */
} wp_line;

typedef struct {
    wp_line *lines;
    u32      line_count;
    u32      line_capacity;
} wp_paragraph_layout;
```

Pagination is a separate, caller-owned output structure -- not part of
the document model (`docs/document-model.md` has no concept of a
"page"; pages are purely a layout-time view over already-broken lines):

```c
typedef struct {
    u32 section;
    u32 paragraph;
    u32 line_index;   /* index into that paragraph's wp_paragraph_layout */
} wp_page_line_ref;

typedef struct {
    wp_page_line_ref *lines;
    u32               line_count;
    u32               line_capacity;
} wp_page;

typedef struct {
    wp_page *pages;
    u32      page_count;
    u32      page_capacity;
} wp_pagination;
```

`wp_document_free` (docmodel.c) is extended to free `para->layout` (the
`wp_paragraph_layout` and its `lines` array) if present -- this module
is what starts actually allocating it. `wp_pagination` has its own
`wp_pagination_free`, since the caller owns it (parallel to
`offload_response_free`).

## Line Breaking (T3.1): Greedy Word Wrap

**Break opportunities are ASCII spaces (`0x20`) only.** No tab/CJK/
Unicode line-breaking (UAX #14) -- out of scope for this engine's first
pass. A "word" is a maximal run of non-space codepoints; a "gap" is a
maximal run of spaces.

**Words can span run boundaries.** Runs split for reasons unrelated to
wrapping (e.g. `wp_document_insert_text_raw` always creates a new run
for inserted text, per `docs/editing.md`), so a single word's bytes can
live across two or more runs even when every character shares the same
visual style. The line breaker walks the paragraph's runs in order and
merges adjacent same-class (space/word) spans across run boundaries
into one logical token *before* making any break decision -- but width
is still measured per-run-fragment within a token and summed (each
fragment may carry a different font/size), never assuming cross-run
kerning.

**Algorithm** (per paragraph, given `content_width_twips`): tokenize
into an ordered list of `{is_space, start, end, width}` tokens (`start`/
`end` as `(run, offset)` pairs), then greedily pack tokens onto lines:

- A word token is added to the current line if `current_width +
  pending_space_width + token.width <= content_width_twips`, OR the
  current line is still empty (a single word wider than the whole
  column is placed anyway and overflows -- not split character-by-
  character; no fallback break for pathological unbreakable tokens).
- A space token is held as `pending_space_width` and only actually
  added to the line's width if a following word is committed to the
  *same* line. If the next word instead starts a new line, the pending
  space is dropped entirely -- trailing/leading whitespace at a wrap
  point never affects width, matching standard word-wrap behavior.
- An empty paragraph (zero runs, or a single zero-length run) produces
  exactly one zero-width line, using the first run's style if any
  exists, else `doc->default_cstyle`, purely for height purposes (an
  empty paragraph still occupies vertical space).

**First-line vs. rest-of-paragraph width**: `wp_paragraph_relayout`
takes one `content_width_twips` parameter (the section's usable column
width) and internally derives the true available width per line from
the paragraph's `wp_para_style` (`indent_left`, `indent_right`,
`indent_first`) -- line 0 gets `content_width_twips - indent_left -
indent_right - indent_first`; every other line gets
`content_width_twips - indent_left - indent_right`. This lets a hanging
or first-line indent work without the caller needing to know about it.

**Font handling**: a small fixed-size (8-entry) linear-scan cache of
`(wp_cstyle, plat_font*)` keyed by character style, opened lazily and
all closed at the end of one `wp_paragraph_relayout` call, avoids
redundant `plat_font_open`/`plat_font_close` pairs for repeated styles
within a paragraph. A 9th+ distinct style in one paragraph just opens
and closes without caching (graceful degradation, not a hard limit --
realistic documents don't have that many distinct styles in one
paragraph).

## Pagination (T3.2)

`wp_document_paginate(doc, out)`: for each section, compute
`content_width_twips = page_width - margin_left - margin_right`; for
each paragraph in it, relayout if dirty (see T3.3), then walk lines in
document order accumulating height. A line starts a new page when
adding its `height_twips` would exceed the section's `content_height =
page_height - margin_top - margin_bottom`. **A new section always
starts a new page**, even if the previous page has room left -- the
common word-processor convention for section breaks, and simple to
reason about/test.

No handling for a single line taller than a page's entire content
height -- it just gets placed on its own (overflowing) page. Not worth
solving in this milestone.

## Cache Invalidation & "Minimal Recompute" (T3.3)

`layout_dirty` is already set to `WP_TRUE` by
`wp_paragraph_pstyle_set`/`wp_paragraph_run_add` (docmodel_build.c,
M1/M2) whenever a paragraph's content or style changes. `wp_document_paginate`
only calls `wp_paragraph_relayout` (line breaking -- the expensive part,
driven by `plat_measure_text` calls proportional to the paragraph's own
token count) for paragraphs where `layout_dirty` is true or
`layout == NULL`; every other paragraph's existing `wp_line` array is
reused untouched.

**The page-filling pass always runs in full** over every paragraph's
(possibly cached, possibly freshly recomputed) line list. This is
deliberate, not a shortcut: page assignment is pure arithmetic over
already-known line heights -- O(line count), no platform calls, no
font measurement -- fundamentally cheap compared to line breaking, which
is O(paragraph text length) *and* calls into the platform metrics seam
per token. "Minimal recompute" for this milestone means minimal
*expensive* recompute; redoing cheap bookkeeping for the whole document
on every pagination call is not a performance concern worth the
complexity of incremental page-filling.

**Test strategy**: `platform/host/metrics_host.c` gets a
`metrics_host_measure_call_count()`/`metrics_host_reset_measure_count()`
pair (mirroring `mem_host`'s leak counters). The T3.3 test paginates a
multi-paragraph document once, resets the counter, edits one paragraph,
paginates again, and asserts the measure-call count is far smaller than
a full from-scratch pagination of the same document -- a direct,
quantitative proof that editing paragraph K does not re-measure every
other paragraph's text.
