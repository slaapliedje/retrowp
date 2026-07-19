# PDF Export (M7 T7.3)

`offload/gateway/odt_gateway.py` also answers `OFFLOAD_CONVERT_EXPORT`
for `format_id = FORMAT_PDF (2)` -- PDF generation lives in the same
gateway process as ODT conversion (`docs/formats/odt.md`), not a fourth
standalone executable, since it's reached through the exact same
`CONVERT_EXPORT` op, just a different `format_id`. **Export-only**: there
is no `format_id` for "import a PDF" (`docs/formats/offload-protocol.md`
explains why -- PDF has no general, reliable text-reconstruction
guarantee).

## Why this is a genuinely different job from ODT

ODT (and RTF) only need to carry already-tagged structured text -- a
`<text:p>`/`{...}` boundary and some attributes, with word-wrap and
pagination left to whatever eventually displays or prints it. PDF has no
such deferral: a PDF *is* a fixed, already-laid-out page description.
Producing one means this gateway has to do its own real text layout --
word-wrap, pagination, and explicit per-line/per-run positioning in a
content stream -- work ODT conversion (T7.2) never needed. That's the
whole reason T7.3 was scoped and built separately from T7.2 rather than
as a quick extension of it (`docs/milestones.md`'s M7 entry says the
same thing).

## Design

- **Page**: US Letter (612&times;792 pt), 1 inch margins all around
  (468&times;648 pt column). Not configurable -- a fixed, documented
  choice, the same kind of "one sane default, not a settings dialog"
  scope this project applies elsewhere (e.g. `ATARI_ASSUMED_DPI` in
  `docs/atari-platform.md`).
- **Fonts**: the 4 standard Type1 Helvetica variants (regular/bold/
  oblique/bold-oblique), selected per run from its bold/italic flags.
  No font embedding -- every PDF viewer has these built in ("the
  standard 14 fonts"), so there's nothing to embed.
- **Glyph widths are REAL Adobe AFM data**, not approximated: extracted
  from `matplotlib`'s bundled `pdfcorefonts/Helvetica*.afm` (the actual
  Adobe Core 14 metrics) during development, for ASCII 32-126. Verified
  that Helvetica-Oblique/-BoldOblique share the exact same widths as
  their upright counterparts (italic is a shear of the same glyphs, not
  a separate width table) before relying on that to only need 2 width
  tables for 4 font variants. Characters outside ASCII 32-126 fall back
  to a single average width -- an approximation, not real AFM data for
  that range, the same "ASCII-first, extended-range best-effort" scope
  `services/spell.md` and `rtf.md` already accept elsewhere.
- **Word wrap** (`_pdf_build_items`/`_pdf_wrap_paragraph`): tokenizes each
  paragraph into word/space items merged across run boundaries (so a
  word split by a style change never breaks mid-word -- the same
  property `engine/src/linebreak.c` guarantees on the engine side,
  reimplemented here since this gateway does its own independent layout
  pass, not a reuse of the engine's), then greedily packs items into
  lines. An overlong single word wider than the column overflows its
  line rather than breaking mid-word, matching `docs/layout.md`'s
  documented allowance for the engine's own line breaker.
- **Alignment**: left/right/center are direct positioning. **Justify**
  redistributes slack via the PDF `Tw` (word-spacing) operator, computed
  as `(column_width - natural_line_width) / space_count` and applied
  uniformly to every space character shown on that line via `Tj`,
  regardless of which run it came from -- simpler and more robust than
  computing per-word positions by hand, and correct precisely because
  the SAME real AFM widths were used to decide `natural_line_width` in
  the first place. The last line of a justified paragraph is left-
  aligned, not stretched -- standard typographic convention (a single
  short last line stretched to full width reads as a mistake, not a
  feature).
- **Underline/strikethrough**: PDF has no native text-decoration
  attribute for either, so both are drawn manually as a thin stroked
  line under (or through) the run's text, positioned using the SAME
  per-run x-position bookkeeping the layout pass already computes from
  the real AFM widths -- not a separate approximation that could drift
  from where the text itself actually renders.
- **Superscript/subscript**: uses PDF's `Ts` (text rise) operator
  directly -- exactly what it's for. The run's font size is also scaled
  down (0.65&times;) for BOTH width measurement and the `Tf` size, so
  wrapping decisions and rendered appearance never disagree about how
  wide superscript/subscript text is. The rise/scale values are
  reasonable typographic defaults, not a claim of matching any specific
  font's real, published super/subscript metrics (Helvetica has none).
- **Pagination**: tracks vertical position; when the next line wouldn't
  fit above the bottom margin, starts a new `/Page` object with its own
  content stream.
- **No `/Widths` array** declared on the font objects -- deliberately:
  every real PDF viewer already knows the standard 14 fonts' built-in
  metrics, which is exactly why this module bothered to use the SAME
  real AFM widths for its own layout (the two are meant to agree without
  this file needing to assert it via an explicit array).

## Verification

Automated (`host/tests/test_convert.c::test_convert_pdf_export_via_gateway`,
part of `make test`): builds a small document, exports to RTF, ships it
through the real `offload_client.c` API with `format_id=FORMAT_PDF`, and
checks the result is a genuine PDF (magic bytes, a real `/Type /Page`
object present). Deliberately small: `engine/src/offload_client.c` caps
a single response at `OFFLOAD_MAX_RESPONSE_PAYLOAD` (4096 bytes) -- a
real constraint of the current transport that an earlier, larger (30-
paragraph) version of this test actually hit during development
(`offload_submit` returned `WP_RANGE`, not a gateway bug) before being
resized down. This is a general transport limit, not a PDF-specific one
-- any op returning bulk data over this link is bounded the same way
today.

Manual, real-tool verification (done during development for the
properties the automated test's size constraint can't exercise):
- **Multi-page pagination**: 40 short paragraphs, generated directly
  (not through the 4096-byte-capped wire protocol) via
  `paragraphs_to_pdf_bytes()`, produced a real 3-page PDF; `pdftotext`
  confirmed all 40 paragraphs present and in order across the page
  breaks.
- **Visual correctness**: a fixture mixing centered bold title text,
  justified body text with an inline bold run and italic colored run
  (word-wrapped across multiple lines, verified by eye that inter-word
  spacing visibly stretches on justified lines but NOT on a paragraph's
  last line), and a right-aligned closing line -- rendered with
  `pdftoppm` and inspected as an image. Underline, strikethrough, and
  superscript/subscript (`E=mc²`, `H₂O`) were verified the same way
  against a fixture built through the real C engine's
  `format_rtf_export()` (via a throwaway C tool linking the already-
  built engine objects, not a hand-written RTF string) -- confirming the
  positioning bookkeeping agrees with real rendering, not just with
  itself.
- **Real-world tool compatibility**: every test PDF was opened with
  `pdfinfo` (structure/page count) and `pdftotext -layout` (text
  extraction, line order) from `poppler-utils` -- a real, independent
  PDF implementation, not this project's own code checking its own
  output.

## Known gaps

- **No images, tables, headers/footers, or font embedding** -- text-only
  layout of the same attribute subset `docs/formats/rtf.md` documents.
  Font embedding/image support are explicitly named as later scope in
  `docs/milestones.md`'s M10 (Typography Polish), not silently dropped
  here.
- **Fixed US Letter, 1 inch margins, no page-size/orientation option.**
- **Extended-charset (non-ASCII) glyph widths are approximated**, not
  real AFM data -- see the width-table note above.
- **Not wired into `platform/atari`'s UI** -- same class of gap
  `docs/formats/odt.md` and `docs/atari-platform.md`'s Thesaurus section
  already note for their own features; a "Save as PDF" menu action is
  natural follow-up work, not attempted in this pass.
