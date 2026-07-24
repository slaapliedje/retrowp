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
- **Fonts, two tiers (T13.3)**: 4 logical style slots (regular/bold/
  italic/bolditalic), selected per run from its bold/italic flags, each
  independently backed by whichever tier is actually available on this
  gateway host:
  - **Tier 0** (always available, zero dependencies): the 4 standard
    Type1 Helvetica variants -- no embedding, every PDF viewer has these
    built in ("the standard 14 fonts").
  - **Tier 1** (`_pdf_tier1_slot_info`/`_pdf_subset_embed`): real
    embedded TrueType via the `fonttools` library, subsetting Liberation
    Sans Regular/Bold/Italic/BoldItalic (checked at a handful of real
    Arch/Debian/Fedora install paths) down to the glyphs a given
    document actually draws. Liberation Sans is metrically compatible
    with Helvetica by design (Red Hat's own stated goal for the family)
    -- verified directly during development against this module's own
    AFM widths (`'A'` advance: 667 in both). Gated **per style slot
    independently**: `fonttools` missing, or one particular face's file
    missing on this host, or that slot simply never being used in the
    document (embedding an unused font would be pure byte waste) falls
    that ONE slot back to Tier 0 rather than refusing embedding
    altogether. A shared `/ToUnicode` CMap (an identity map over
    0x20-0xFF, correct because this module's own character codes ARE
    the Unicode codepoint in that range -- see `_pdf_escape_str`) is
    attached to every Tier-1 font, fixing a real text-extraction bug
    found during development: without it, a real viewer's
    (`poppler`'s) text-extraction fallback heuristic broke down partway
    through a subsetted font's renumbered glyph IDs even though visual
    rendering was already correct.
  - There is no document-level font-*family* selection anywhere in this
    pipeline (RTF's `\fonttbl` is a skipped destination on import, same
    as the C engine's own importer; RTF export always declares a single
    Helvetica font) -- so Tier 1 embeds one real family for the same 4
    logical slots Tier 0 already has, not a font-family feature.
- **Glyph widths are REAL data for both tiers**, not approximated:
  Tier 0 uses Adobe AFM data extracted from `matplotlib`'s bundled
  `pdfcorefonts/Helvetica*.afm` (the actual Adobe Core 14 metrics)
  during development, covering ASCII 32-126 (verified that Helvetica-
  Oblique/-BoldOblique share the exact same widths as their upright
  counterparts, so only 2 width tables are needed for 4 variants).
  Tier 1 reads real advance widths straight out of the loaded font's own
  `hmtx` table (scaled to a 1000-unit em) for the full WinAnsi range,
  32-255 -- a strictly wider real-data range than Tier 0's, since
  there's no AFM-style table to be limited by. A slot still on Tier 0
  keeps Tier 0's ASCII-only real data plus its single average-width
  fallback outside that range, same as before.
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
- **No `/Widths` array** declared on a Tier-0 font object -- deliberately:
  every real PDF viewer already knows the standard 14 fonts' built-in
  metrics, which is exactly why this module bothered to use the SAME
  real AFM widths for its own layout (the two are meant to agree without
  this file needing to assert it via an explicit array). A Tier-1
  embedded font DOES declare a real `/Widths` array (required for a
  non-standard font) plus a `/FontDescriptor` with real ascent/descent/
  cap-height/bbox/italic-angle, all read from the same loaded font.

## Verification

Automated (`host/tests/test_convert.c::test_convert_pdf_export_via_gateway`,
part of `make test`): builds a small document, exports to RTF, ships it
through the real `offload_client.c` API with `format_id=FORMAT_PDF`, and
checks the result is a genuine PDF (magic bytes, a real `/Type /Page`
object present). Deliberately small: `engine/src/offload_client.c` caps
a single response at `OFFLOAD_MAX_RESPONSE_PAYLOAD` (32768 bytes,
raised from an original 4096 once an earlier, larger fixture actually
hit it during development -- `offload_submit` returned `WP_RANGE`, not a
gateway bug) -- a real constraint of the current transport. This is a
general transport limit, not a PDF-specific one -- any op returning bulk
data over this link is bounded the same way today. T13.3's font
embedding made this cap concretely relevant again: an EMBEDDED font (a
real TrueType subset is several KB even for a handful of glyphs) can
push a small document's PDF past 32768 bytes if every style slot in it
were embedded regardless of use, which is exactly why embedding is
scoped to slots the document actually draws with (see "Fonts, two
tiers" above) -- confirmed directly during development: the fixture
below stayed under the cap once unused slots correctly fell back to
Tier 0's zero-cost standard fonts.

Manual, real-tool verification (done during development for the
properties the automated test's size constraint can't exercise):
- **Multi-page pagination**: 40 short paragraphs, generated directly
  (not through the 32768-byte-capped wire protocol) via
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
- **Font embedding (T13.3)**: verified with `pdffonts` (from
  `poppler-utils`) against a real fixture mixing a Tier-1 slot (bold, in
  this case) with unused slots -- confirmed the used slot shows up
  `emb yes uni yes` (real embedded TrueType, real ToUnicode) while the
  three unused slots correctly stay Tier 0 (`emb no`, zero embedding
  cost for a font nothing draws with). `pdftotext` against a real,
  multi-line, word-wrapped, justified paragraph confirmed full and
  correct text extraction through the embedded font and its `/ToUnicode`
  CMap -- not just visual rendering. Also verified the graceful Tier-0
  fallback path (with `fonttools` simulated missing) produces byte-for-
  byte the same small, zero-embedding PDF this module always produced
  before T13.3.

## Known gaps

- **No images, tables, or headers/footers** -- text-only layout of the
  same attribute subset `docs/formats/rtf.md` documents. Image support
  is deliberately NOT attempted as an extension of this gateway: there
  is currently no image concept anywhere in the core document model
  (`engine/include/docmodel.h`) and no real import path that could ever
  produce one (RTF's `\pict` group is discarded on import, ODT has no
  frame/image handling here) -- adding it properly means extending the
  document model first, per CLAUDE.md's "document model is the single
  source of truth" rule, a cross-cutting lift on the order of a platform
  milestone. Tracked as its own future milestone item in
  `docs/milestones.md`, not silently dropped or half-implemented here.
- **Fixed US Letter, 1 inch margins, no page-size/orientation option.**
- **Extended-charset (non-ASCII) glyph widths are approximated** for any
  style slot still on Tier 0 (no `fonttools`, or that particular
  Liberation face missing on this host) -- see the width-table note
  above. A slot actually running on Tier 1 uses real widths across the
  full WinAnsi range instead.
- **No font embedding tiering beyond Tier 0/Tier 1** -- e.g. no option to
  force Tier 0 even when Tier 1 is available, no support for embedding
  any font family beyond Liberation Sans (there is no document-level
  font-family concept anywhere in this pipeline to select from, see
  above).
- **UI wiring: done (M7 T7.4).** File Save in `platform/atari/
  app_shell.c` now recognizes `.PDF` via the same extension-sniffing
  dispatch as `.RTF`/`.ODT` -- see `docs/atari-platform.md`'s "ODT + PDF
  wired into the UI" section, including
  `ci_atari_convert_serial.sh`'s real-RS-232 verification that a
  CONVERT_EXPORT(PDF) request/response actually survives the real
  serial link this target would use, not just TCP loopback.
