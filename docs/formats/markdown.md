# Format: Markdown (`format_md`)

`engine/include/format_md.h`, `engine/src/format_md.c`. An importer and an
exporter around the document model, like plain text and RTF (CLAUDE.md
rule 2); pure C89, no platform calls, on every target. The file flow picks
it by name: `.md` on Open and Save, `:e name.md`, `:w name.md`.

## What survives

The model holds paragraphs of styled runs; Markdown holds a good deal
more. The rule both ways is: what the model can represent is converted,
what it cannot is kept as text, and nothing is dropped.

| Markdown | In the model | Back out |
|---|---|---|
| `# H1`, `## H2`, `### H3` | a paragraph whose runs are all bold at 24/18/14 pt | the same heading |
| `#### H4` and deeper | bold at the body size | `**bold**` paragraph |
| `**bold**`, `*em*`, `***both***`, `__`, `_` | run styles | `**`, `*`, `***` |
| blank line | ends the paragraph | one blank line between paragraphs |
| a line with no blank line before it | continues the paragraph, joined with a space (what a reader shows) | one line per paragraph |
| `&nbsp;` alone on a line | an empty paragraph | `&nbsp;` |
| `- item`, `1. item`, `> quote`, `---` | its own paragraph, marker kept as text, indentation kept | as it was |
| ```` ``` ```` fence | the fence and each line inside are paragraphs of unstyled text, one per line | the same lines, one per line, nothing escaped |
| `` `code` `` | kept with its backticks; no emphasis inside | `` \`code\` `` |
| `\*` and any other escaped punctuation | that character | re-escaped where a reader would take it as markup |
| links, images, HTML, tables | text | text |
| underline, colour, size (other than a heading's) | kept in the model from wherever it came | nothing: Markdown has no spelling for them |

An image paragraph goes out as `![image](path-of-the-cached-pixels)`: the
one place a reader is told where the pixels are rather than given them.

## Round trip

Export then import gives back the same paragraphs, text and bold/italic,
for any document. The exporter writes the spelling the importer reads
exactly, so a `.md` saved by RetroWP and opened again is the same
document; `host/tests/test_format_md.c` holds a canonical file that goes
`import -> export` byte for byte. Two normalisations are deliberate:

- Styles open and close at word edges. Blanks between words of one style
  stay inside the markers; a blank at a style's edge goes outside, since
  a reader will not close emphasis before whitespace. A run that is only
  whitespace loses its style on the way out.
- Trailing blanks on a line are dropped on the way in, and up to three
  leading blanks are not a reason to treat a line differently (a reader
  ignores them before a heading or a list marker).

## Emphasis

Markers are resolved per paragraph in two passes: first every run of `*`
or `_` is found with whether it could open (text follows) or close (text
precedes; `_` also wants a word edge outside, so `snake_case` is text);
then closers are matched to the nearest open markers of the same
character, innermost first, and whatever is left over opens. A run that
closes and has markers left opens with them, so `**bold***em*` reads as
bold then italic, which is what a CommonMark reader shows and what the
exporter writes for two adjacent runs. Openers nothing ever closes were
text all along: `5*3 and **unclosed` is one unstyled run.

The exporter nests bold outside italic and, when bold changes underneath
italic, closes and reopens the italic, so every change between the four
style states reads back the same. What is not CommonMark's "rule of
three": `*a**b**c*` comes in as one italic run rather than italic with
bold inside, a form prose does not use.

## Escaping on the way out

Only what the importer would otherwise read as markup: `\` and `` ` ``
always; `*` unless the word is nothing but stars; `_` when a reader could
open or close with it (so `snake_case` goes out as written); a `#` that
starts a paragraph; and `&nbsp;` as a whole first word, so it is not taken
for the blank-line idiom. List markers are not escaped: the importer keeps
them as text, and a reader still sees the list.

## Sizes

Headings use the RTF importer's own scale, in twips: 480 (24 pt), 360
(18 pt), 280 (14 pt). A paragraph whose runs are all bold and all one
size of 14 pt or more goes out as the heading of the nearest level, so an
RTF heading saved as Markdown comes out as `#` rather than as bold text.

## Memory

The importer's per-paragraph tables (64 marker runs and their open
stack) live on the heap with the two growable buffers, not on the stack:
an Amiga program launched from `RUN` has 4 KB of stack in all.
