# RTF Import/Export (M5)

`format_rtf` targets classic RTF 1.x (the dialect Word 5.x/6.x, WordPad,
and most retro-era word processors read and write), not modern RTF/OOXML
extensions. Scope is deliberately bounded to what maps cleanly onto the
locked document model (`wp_char_style`/`wp_para_style`) and what a 1MB
target can parse with a fixed-depth state stack and no unbounded
allocation growth per token.

## Supported control words

**Structure**
- `{` / `}` push/pop a formatting-state scope. Bounded stack depth
  (`RTF_MAX_DEPTH` = 64) -- deeper nesting is `WP_RANGE`, not a crash.
- `\*` marks an ignorable destination (e.g. `{\*\generator ...}`): the
  whole group is skipped, its text never reaches the document.
- Any OTHER unrecognized control word is ignored (not an error) per the
  RTF spec's own forward-compatibility rule -- real-world RTF always
  carries control words from newer versions or vendor extensions that
  older readers must tolerate. Only unrecognized *destinations* (always
  `\*`-prefixed in files this importer is expected to handle) are
  skipped wholesale; an unrecognized plain control word's surrounding
  text is still imported.

**Header** -- the file must start `{\rtf1`; `\ansi` or `\ansicpg1252` is
the only supported character set (anything else, e.g. `\mac`, `\pc`, or a
different codepage, is `WP_UNSUPPORTED` -- a real, documented gap, not a
silent mis-decode).

**Destinations recognized and their content NOT imported as document
text**: `\fonttbl`, `\info` (and everything inside it, including
`\title`/`\author` -- see "no title/author capture" gap below),
`\stylesheet`, `\pict`, `\object`, `\footnote`, `\header`, `\footer`, and
any `\*`-marked group.

**Destinations recognized and captured**:
- `\colortbl` -- up to 32 `\redN\greenN\blueN;` entries, referenced later
  by `\cfN`. More than 32 colors is `WP_RANGE`.

**Paragraph formatting** (`wp_para_style`): `\pard` (reset to default),
`\par` (end current paragraph, start a new one), `\ql`/`\qr`/`\qc`/`\qj`
(-> `WP_ALIGN_LEFT`/`RIGHT`/`CENTER`/`JUSTIFY`), `\li`/`\ri`/`\fi`
(indent left/right/first, twips -- `\fi` may be negative), `\sb`/`\sa`
(space before/after, twips).

**Character formatting** (`wp_char_style`): `\b`/`\b0`, `\i`/`\i0`,
`\ul`/`\ulnone`, `\strike`/`\strike0`, `\super`/`\sub`/`\nosupersub` (->
the matching `wp_style_flags` bits), `\fsN` (half-points -> `size_twips =
N*10`), `\cfN` (-> `color_r/g/b` from the color table).

**Text**: literal bytes under the `\ansi`/cp1252 charset (0x00-0x7F pass
through; 0x80-0xFF map via a fixed cp1252-to-UTF-8 table), `\'hh` (hex-
escaped byte, decoded through the same table), `\{` `\\` `\}` (literal
characters), `\tab` (U+0009), `\emdash`/`\endash`/`\lquote`/`\rquote`/
`\ldblquote`/`\rdblquote`/`\bullet` (their Unicode codepoints -- common
enough in real-world RTF to be worth the small fixed lookup).

## Deliberate simplifications (documented, not silent)

- **No font name wiring.** `\fonttbl` is parsed only enough to be
  correctly skipped; nothing populates `wp_char_style.font_name`. This is
  the same gap T3.1 already documented (the field is a bare `wp_str`
  offset with no length, and nothing in the codebase sets it yet) --
  RTF import doesn't change that, just doesn't pretend to.
- **No title/author capture**, for the identical reason:
  `wp_document.title`/`.author` are also bare `wp_str` offsets with no
  paired length field anywhere in the locked struct, so there is no safe
  way to record how many bytes a stored title/author string occupies.
  Inventing a NUL-termination convention just for these two fields would
  contradict the string pool's own explicit design (`wp_strpool.bytes`
  is documented as "UTF-8, not NUL-terminated internally" -- every other
  piece of text uses an explicit offset+length `wp_span`). `\info` is
  therefore a skipped destination like `\fonttbl`: its `\title`/`\author`
  subgroups are parsed enough to be correctly skipped, not stored.
- **`\line` (soft line break) is treated as a paragraph break**, same as
  `\par`. The document model has no concept of a break *within* a
  paragraph (paragraphs are the atomic block unit -- see
  `docs/document-model.md`), so there is no lossless way to represent
  one. An approximation, not a crash or a dropped character.
- **`\~` (non-breaking space) becomes a regular space**, `\-` (optional
  hyphen) is dropped entirely. Neither concept exists in the document
  model.
- **Unicode (`\uN`) is not supported.** Classic RTF 1.x's primary text
  path is `\ansi` + `\'hh`, which this importer handles fully; `\uN`
  (RTF 1.1+ Unicode escapes with a fallback char) is a real, separate
  gap -- a fixture RTF using it will import the fallback character(s)
  literally rather than the intended Unicode codepoint, since the
  fallback text still arrives as ordinary literal text.

## Export

`format_rtf_export` is the inverse: walks the document emitting
`\pard` per paragraph, one brace-scoped run group per style change
(`{\b\i\fs24 ...text...}`), `\par` between paragraphs, and a minimal
one-entry `\fonttbl` (`{\f0 Helvetica;}`, always referenced as `\f0` --
honest, since nothing sets a real font name anywhere in the codebase to
export). `\qc`/`\qr`/`\qj`/`\li`/`\ri`/`\fi`/`\sb`/`\sa`/`\cfN` are
emitted only when they differ from the paragraph/character default, to
keep output reasonably compact. The `\colortbl` is emitted with exactly
the colors actually used, plus the implicit auto-color entry 0.

Round-trip contract: model -> RTF -> model preserves every *supported*
attribute exactly (alignment, indents, spacing, bold/italic/underline/
strike/super/sub, size, color) for documents built entirely from that
supported subset -- the same "byte/attribute-identical for canonical
input" contract `format_txt` already has, not a claim about
arbitrary real-world RTF files (which may use unsupported features on
either side of the round trip).

Verified two ways: (1) `host/tests/test_format_rtf_export.c`'s own
model->RTF->model round-trip, and (2) independently -- exported RTF fed
to `soffice --headless --convert-to txt` (LibreOffice) produced exactly
the expected plain text, including a cp1252 hex-escaped accented
character and an `\emdash`, confirming the output is genuinely
well-formed RTF readable by real-world software, not just self-
consistent with this project's own importer.

Implementation note: `format_rtf_export`'s emission logic (escaping,
color-table lookups, conditional control words) runs through ONE shared
function for both the size-measuring pass and the real write pass
(`rtf_writer` in `format_rtf_export.c`), rather than two independently-
maintained implementations the way `format_txt.c`'s simpler `export_size`
helper does it -- RTF emission has enough real logic that two hand-synced
copies would be a real drift risk. Nothing is written to the caller's
buffer unless the whole document fits, matching `format_rtf.h`'s "nothing
written if too small" contract exactly (tested explicitly with a sentinel-
filled buffer).
