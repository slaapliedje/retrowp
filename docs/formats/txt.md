# Format: Plain Text (`format_txt`)

## Paragraph Policy

**Each newline-terminated line is exactly one paragraph.** This is the only
policy that makes `export -> import -> export` byte-identical without any
lossy reflow decisions (word-processor-style "blank line separates
paragraphs, single newlines are soft wraps" would require guessing at
reflow on export, which plain text has no way to represent losslessly).

- One line == one `wp_paragraph`, containing exactly one `wp_run` spanning
  the whole line (styling is not representable in plain text, so every run
  uses `doc->default_cstyle` and every paragraph uses `doc->default_pstyle`).
- An empty line (two consecutive newlines, or a leading/trailing newline)
  produces an empty paragraph: one run whose span has `length == 0`.
- Import always creates exactly one `wp_section`, regardless of input
  length (including zero paragraphs for an empty file). Plain text has no
  concept of a page or section break.
- Export walks *every* section and paragraph in the document, concatenating
  all of a paragraph's runs' text (ignoring style/run boundaries, since
  plain text carries no styling) followed by one `\n`. This makes export
  meaningful for documents built by other means (not just `format_txt`
  import), not only ones that came from a `.txt` file.

## Encoding

Per `skills/encoding.md`, the pool is UTF-8 and so is plain text by
default. `format_txt` does not do charset conversion -- that only happens
at the platform edge (Edge A) when text reaches a display or native file
API with a non-UTF-8 charset. This module deals in edge B: bytes in, UTF-8
spans out.

## Import Leniency vs. Export Canonicalization

Import is lenient; export is strict and canonical:

- **Line endings:** import accepts both `\n` and `\r\n`; a `\r` immediately
  before a `\n` is stripped and not included in the paragraph's text.
  Export always emits bare `\n`.
- **Final line:** import accepts a final line with no trailing newline
  (whatever remains after the last `\n` becomes one more paragraph).
  Export always appends exactly one `\n` after every paragraph, including
  the last.

Because of this, **only input already in canonical form** (LF-only line
endings, every line including the last terminated by `\n`) round-trips
byte-for-byte through `export -> import -> export`. The fixture set in
`host/tests/fixtures/` is split accordingly:

| Fixture | Round-trips? | Exercises |
|---|---|---|
| `simple.txt` | yes | one paragraph, canonical form |
| `empty.txt` | yes | zero paragraphs (0-byte file) |
| `multi_para.txt` | yes | several paragraphs |
| `blank_lines.txt` | yes | an empty paragraph in the middle |
| `unicode.txt` | yes | multi-byte UTF-8 content (2/3-byte codepoints) |
| `no_trailing_newline.txt` | no (import-only) | final line without `\n`; export adds one |
| `crlf.txt` | no (import-only) | `\r\n` line endings; export normalizes to `\n` |

## API

`engine/include/format_txt.h`:

```c
wp_status format_txt_import(const u8 *bytes, u32 len, wp_document *doc);
wp_status format_txt_export(const wp_document *doc, u8 *out, u32 out_cap,
                             u32 *out_len);
```

`format_txt_import` initializes `doc` itself (calls `wp_document_init`) --
callers must not pass an already-initialized document.

`format_txt_export` is buffer-based, not streaming: it always reports the
exact required size via `*out_len`, and returns `WP_RANGE` without writing
anything if `out_cap` is too small. Plain text documents are small enough
in the 1MB tier that this is not a memory concern; streaming export is
reserved for formats where it matters more (RTF, PDF -- see CLAUDE.md
Memory Rules).
