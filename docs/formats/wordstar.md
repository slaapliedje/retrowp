# WordStar (document mode)

Reader for the **classic WordStar line**: 3.x on CP/M through 7.0 on DOS
(1992, the last of that lineage).

Not WordStar 2000 — despite the name it was a separate product with an
incompatible format, and a file from it will not parse as anything
sensible here. Not WordStar for Windows (1991–93) either, a different
codebase again.

## Why this format first

The project's purpose is to let a document start on a busy, online
machine and be finished on a quiet one built for writing. RTF and ODT
already carry documents *in* from the modern side. WordStar is the first
step of the other half — reading what already exists on the vintage side.

A sibling reader for Atari Works `.STW` is researched in
[atariworks.md](atariworks.md) — that one has a corpus of 263 documents
present simultaneously as `.STW`, `.RTF` and `.TXT`, so it can be scored
against reality in a way this reader cannot.

It is also the most tractable of the three vintage formats worth having
(WordStar, WordPerfect, Word `.DOC`): close to plain text, no compound
container, no binary record tables. And it is the one still in active
use — George R. R. Martin writes in WordStar 4.0 on DOS, which is exactly
the workflow this project exists to serve.

## The conventions

### Hard vs soft returns — the load-bearing one

WordStar records its own word-wrapping in the file:

| Byte | Meaning |
| --- | --- |
| `0x0D` | **Hard** return — the writer pressed Return. A real paragraph break. |
| `0x8D` | **Soft** return — WordStar wrapped the line here while typing. |

Everything else follows from that distinction. A soft return is where a
line broke on a screen that no longer exists, so the reader joins across
it; a hard return ends a paragraph.

Getting this wrong is not a cosmetic error. A reader that treated every
return as a line break would import a novel as several thousand one-line
paragraphs, each broken to some 1980s margin — and the damage would be
permanent, because once both kinds of break look alike there is no way to
recover which were the author's.

WordStar consumed the space at a wrap point, so the reader restores one,
and exactly one.

### Bit 7

In document mode WordStar sets bit 7 on soft spaces and on the last
character of a wrapped word. The character is the low seven bits either
way, so the reader masks it off.

**This is the reader's one real limitation, and it is inherent rather
than an oversight.** A file written in an 8-bit codepage has accented
characters that collide with the flag, and nothing in the file
distinguishes them. Seven-bit ASCII is what the classic line
overwhelmingly produced, so that is what is assumed.

### Character styles

Raw control bytes toggle formatting, and they map cleanly onto this
project's own style flags:

| Byte | WordStar keystroke | Maps to |
| --- | --- | --- |
| `0x02` | `^PB` | `WP_STYLE_BOLD` |
| `0x13` | `^PS` | `WP_STYLE_UNDERLINE` |
| `0x19` | `^PY` | `WP_STYLE_ITALIC` |

The `^P` in the keystroke is a literal-next prefix and never reaches the
file. Toggles produce real runs carrying real style handles — losing
every emphasis in an imported manuscript would be a silent, unrecoverable
edit to someone's work.

Other print-control codes (`^V` superscript, `^T` subscript and friends)
are dropped rather than rendered as garbage glyphs.

### Dot commands

A `.` in column one introduces a formatting directive running to the end
of the line: `.pa` page break, `.lm` left margin, `.he` header, and so
on. These are consumed and **dropped** — see the gaps below.

Only `.` followed by a *letter* counts. Prose beginning with an ellipsis
(`...and then`) or a decimal (`.22 calibre`) is text, and eating it would
delete a line of someone's writing.

## Detection

WordStar files carry no magic number and no reserved extension — they are
commonly `.WS`, `.DOC` (colliding with Microsoft Word), or nothing at
all. `format_ws_looks_like()` therefore sniffs by content, on the two
markers that are both distinctive and common: a soft carriage return, and
a dot command at the start of a line.

It is deliberately conservative and must never claim a plain ASCII file.
WordStar's own **non-document mode** produced exactly that, and the
plain-text importer already handles it correctly.

## Known gaps

Stated rather than implied, per this project's habit:

- **Dot commands are dropped.** This document model has no page-break,
  margin or header concept to put them in, and inventing one from a
  directive we cannot render would be worse than losing it.
- **No export yet.** Import only. Round-tripping back to WordStar is the
  natural next step and is what would actually close the loop for someone
  who wants to keep working in WordStar afterwards.
- **8-bit codepages are ambiguous**, as described above.
- **Notes, merge variables and other later-version constructs** are not
  handled; they degrade to text.

## Verification status

The parser is covered by nine host tests
(`host/tests/test_format_ws.c`), and the fixtures are built as byte
arrays in the test file rather than checked in as files — the interesting
parts are exactly the bytes an editor would normalise away.

**Those fixtures encode the format as documented above, so they test the
parser against the model, not the model against reality.** Only a genuine
WordStar file validates the model itself. Getting one — ideally from more
than one version of the line — is the highest-value next step for this
reader, and until then the conventions above should be read as
well-supported rather than proven.
