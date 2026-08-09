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

**The token buffer lives on the HEAP, deliberately.** `tokenize()`
describes a paragraph's wrap points in a fixed 512-entry `token_t`
array, and `wp_paragraph_relayout` used to declare that array as an
ordinary local — about **14KB in a single stack frame**. That is more
stack than some of this project's targets hand a process in total:
AmigaOS gives a `RUN`-launched program **4096 bytes**, of which ~1292
were already spent by the time MUI's draw callback reached layout
(measured on target, `docs/amiga-platform.md`). The result was a ~5x
overflow that corrupted whatever sat below the stack — nondeterministic
wild jumps that crashed RetroWP on ~17 of 18 Amiga boots and sometimes
took an unrelated system task down instead. It is `mem_alloc`'d now,
per CLAUDE.md's "all engine allocation goes through mem_alloc" rule,
and freed on every exit path. The allocation happens *after* the
image-paragraph branch, so an image paragraph — which never tokenizes —
pays nothing.

The general rule this establishes for `engine/`: **large buffers do not
go on the stack**, because the engine's stack frames are shared by every
backend and the tightest target sets the budget for all of them.

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

## Paragraph Kind: Image Paragraphs (M15)

`wp_paragraph_relayout` opens with an early branch on `para->kind`
(`docs/document-model.md`'s Image section): a `WP_PARA_IMAGE` paragraph
skips `tokenize()`/word-wrap entirely and calls the same `emit_line()`
every text paragraph's line-packing loop already uses, exactly once,
sized straight from the registered `wp_image_desc`'s own
`display_width_twips`/`display_height_twips` -- a fixed-size,
non-flowing block, not something that participates in word wrap.
`content_width_twips` is accepted but unused on this path (no
text-wrap-around-image in this milestone); a missing/invalid image
handle degrades to a zero-size line rather than failing relayout
outright.

This is the *only* change `linebreak.c` needed. Two more call sites
outside it have to be paragraph-kind-aware, both in `justify.c`, since
every backend's paint loop and the shared fragment-walk primitives
unconditionally call into a line's run range:

- `wp_line_next_fragment` returns `WP_FALSE` immediately for an image
  paragraph -- there are no run bytes to fragment (`runs`/`run_count`
  stay zero-init by construction), so the iterator is degenerate and
  correctly reports "nothing here" without dereferencing anything.
- `wp_justify_space_extra_twips` returns `0` immediately for the same
  reason -- there are no inter-word gaps to distribute justification
  space across.

Both guards live in the shared primitive itself rather than in each of
the four backends' own paint loops -- the same "guard it once, don't
trust every call site to remember it" reasoning as everywhere else
guard logic could otherwise be duplicated four ways. A backend's paint
loop still needs its *own* `kind` branch before walking fragments at
all (to actually paint the image's real pixels, or DOS's bracketed
text marker, instead of nothing) -- see `platform/*/app_shell.c`'s own
`para->kind == (u8)WP_PARA_IMAGE` branches -- but if one were ever
forgotten, these two guards mean the fallback behavior is "paint
nothing for this line," never a crash or garbage read.

`engine/src/paginate.c` needed **zero changes**: it only ever reads a
line's already-computed `height_twips` when deciding page breaks, and
an image paragraph's one line has a real `height_twips` like any
other line, computed by the branch above. This is a deliberate proof
point, not an oversight -- pagination's page-break arithmetic is
already fully decoupled from *what kind of content* produced a line's
height.

## Caret Positioning: `wp_line_cursor_x_twips` (T157)

`wp_line` stores a byte range plus one aggregate measured width -- enough
to *draw* a line, but nothing in it says where inside that line a given
`(run, offset)` cursor falls. Every backend that paints a caret needs
that number, and every one of them originally derived it (or failed to)
on its own:

| Backend | Before T157 |
| --- | --- |
| DOS | correct -- but only because a fixed-cell font makes the column a pure byte count (`cursor_col_in_line`), with no measuring at all |
| Mac | caret hardcoded to the left margin, and anchored an ascent too high |
| Amiga | the same two defects, verbatim |
| Atari | no caret drawn at all |

So this is the second function in `justify.c` to exist for exactly the
reason the first one does (see `wp_justify_space_extra_twips` above):
shared paint-loop support the engine owns, because four independent
re-derivations is how three of four backends got it wrong.

`wp_line_cursor_x_twips(doc, para, line, cursor_run, cursor_offset,
space_extra_twips)` walks the line's fragments in the same order and by
the same rules the paint loop does, accumulating measured width until it
reaches the cursor, then measures the partial fragment up to the cursor's
own byte offset. It returns twips relative to the **line's own drawing
origin** -- so a caller adds it to wherever it was about to start
painting that line (left margin plus the line's alignment offset), not to
the paragraph's or the window's origin. Notable behaviors, all
host-tested in `host/tests/test_justify.c`:

- **`space_extra_twips`** is the value `wp_justify_space_extra_twips`
  returned for this same line, so a JUSTIFY line's stretched gaps are
  counted exactly where the paint loop's own `extra_twips` advance
  counts them. Pass `0` when not justifying.
- **A cursor at or past the line's end** returns the full content width
  -- the ordinary case while typing, where the cursor sits one past the
  character just inserted.
- **A cursor parked in a zero-length run** (an edit can leave one behind,
  `docs/editing.md`) still resolves correctly. The fragment walk skips
  empty runs entirely, so a walk that only stopped on a *matching*
  fragment would never stop and the caret would jump to the end of the
  line; stopping as soon as a fragment's run index passes the cursor's is
  what prevents that.
- **Fonts are opened here, not borrowed** from the caller. A paint loop's
  cached font belongs to whatever run it last drew, which is not
  necessarily the cursor's run. Measuring goes through `plat_measure_text`
  like everything else (CLAUDE.md rule 4).
- **Image paragraphs** return `0`, same guard as the other two functions.

The return unit is twips, so each backend applies its own twips->px
ratio. On a backend whose ratio is not exact (Amiga's is
`(twips * dpi_x) / 1440`; Mac's is a plain `/20` and *is* exact, since
`plat_measure_text` there returns whole pixels scaled by 20) converting
the total once can differ by about a pixel from summing per-fragment
pixel widths -- below the width of the caret itself, and not worth
threading a conversion callback through the engine to avoid.
