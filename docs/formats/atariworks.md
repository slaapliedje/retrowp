# Atari Works (`.STW`)

**Status: mapped (2026-11-11); importer in progress (M17).** The map
below was read off the 263-document corpus, and a research walker that
follows it extracts every document's text word-for-word identical to its
`.TXT` (256 of 262 scored; the rest are differences in the `.TXT`s
themselves -- see "The map, scored"). The first-read notes after it are
kept as the record of where this started.

## Why this format

Atari Works (1992) is the integrated word processor / database /
spreadsheet Atari shipped for the ST, TT and Falcon. It is the closest
thing the Atari line got to an office suite, and it is the usability bar
this project set itself: *as usable as Atari Works, but reading and
writing formats the rest of the world still uses.*

The Amiga never got an equivalent at all, which is part of why the
document model is platform-independent — one importer here serves every
backend.

It is also the natural companion to the WordStar reader
([wordstar.md](wordstar.md)): both are vintage-native formats holding
documents that exist today and cannot currently be opened at all.

## The specimens — this format has a Rosetta Stone

`~/Documents/Atari/atariworks-specimens/` (5.5MB, 848 files), recovered
from the Atari HQ Archive #1 ISO:

- `Documents/Atari/patton/word/{STW,RTF,Text}/` — Atari's own legal
  department's files, **263 documents present in ALL THREE formats.**
- `Documents/Atari/Legal/STWorks/` — a further set of `.STW` files.

**That triple is the whole reason this format is tractable.** For 263 real
documents the correct output is already known, in RTF — which this project
already imports and exports — and in plain text. An importer can be scored
against a corpus rather than against a model of the format.

Compare `wordstar.md`'s honest admission: its fixtures "encode the format
as documented, so they test the parser against the model, not the model
against reality." Here that gap can be closed from day one, and it should
be: build the RTF/TXT comparison harness BEFORE the parser.

## The map

All numbers are big-endian, as on the 68000 that wrote them. Offsets
are into the file.

| Offset | Content |
| --- | --- |
| 0 | `"10"` or `"11"` -- two versions, about half the corpus each |
| 2..33 | words: window geometry (x, y, w, h) and similar; not needed to read |
| 34..81 | six reals: paper width, paper height, left, right, top, bottom margin, inches. Version 11: IEEE doubles. Version 10: an IEEE single in the first four bytes of each eight, **four times** the inches |
| 154 | the default face: 32-byte name, then id, size, flags, a double (tab interval?) |
| 272 | the same again (a second default) |
| 318 (0x13e) | the info block, words: 26, flags, 1, **pages**, **paragraphs** (one record each), **lines**, **objects**, ?, ... |
| 368 (0x170) | the line table: one 16-byte entry per line (offset in its paragraph, width, height, ...): a layout cache, not needed to read |
| then | the paragraph records, `records` of them |
| then | a word (the line count: one past the last record's first line), the last record's carried tabs and fields |
| then | `objects` objects |
| then | the body |
| then | the font table, to the end of the file |

**Paragraph records**, one a paragraph, in order. Each starts with a
word -- the paragraph's first line, as an index into the line table --
then that record's tab
stops (one word each), then its merge-field descriptors (54 bytes each),
then 30 bytes: left indent, first-line indent, right edge, *the next
record's tab count*, a format word, font id, size, three words, the
paragraph's text length, *the next record's field count*, three words.
The two counts for a record are carried in the record before it -- the
first record has none -- which is why no record can be read on its own.
Indents, the right edge (measured from the left margin) and tab stops
are in 1/91.4 inch: 640 of them span seven inches, 15.75 twips each.
The format word's high byte: low nibble the alignment (1 left, 2 right,
4 centre, 8 justified), high nibble the line spacing.

**Merge-field descriptors**: 8 bytes, the database file (14 bytes, e.g.
`LABELS.STD`), the field (32 bytes). The body holds a 0x01 where each
field goes, in the order the descriptors appear; Atari Works' own text
export writes `LABELS.STD:Company` there.

**Objects** are 176-byte headers: a word, a kind byte, a sequence
byte, position and size, a 32-byte name (`BOX_LOGO.GEM`) and the
original path (`D:\GEM\FUJ_LOGO\BOX_LOGO.GEM`). Kind 5 (0x85 with a
flag set) is a GEM metafile and kind 6 a GEM IMG; both are followed by a
32-bit length and that many bytes -- the whole metafile, but only the
IMG's 16-byte header, its pixels left in the external file. Other kinds
(1, 3: frames, boxes, rules) are the header alone.

**Where an object goes.** Its header's words 2..6 are x (signed, from
the left margin), y (32 bits, signed), width and height, all in the
same 1/91.4 inch. y runs down the whole document in the line table's
own measure: the line table gives every line's height (word 5), and a
line whose flag word (word 1) has bit 0 starts a page -- the count of
those is the info block's page count, and the heights add up to its
word 12, in all 265 files. Pictures copied onto each page of a letter
land within a few units of the same place on each, measured so.
RetroWP places an object after the last paragraph to end above it, in
a frame that far below it (`WP_FRAME_Y_PARA`), so it stays by its text
whatever the machine's fonts do to the lines; where that paragraph
began on an earlier page, from its own page's top margin instead.
Kind 1 with a height of 4 or less is a rule; drawn shapes have seq 0,
imported files seq 1 and a name. Many "pictures" in the corpus are
lettering -- GEM text (`v_ftext`, rotated by `vst_rotation`) -- which
drawfile.c does not draw yet, so a letterhead shows its bars but not
its words.

**The body** is text in the Atari character set: 0x0a ends a
paragraph, 0x09 is a tab, 0x01 a merge field, and 0x00 begins an 8-byte
style change -- 00, font id, size in points, GEM text effects (1 bold,
8 underline; italic comes from an italic face), 00. It ends with a lone
00 where the font table's first entry starts.

**The font table**: 36-byte entries, 00, id, the face's name (Speedo
names: `Humanist 521 Bold`, `Bitstream Charter`), NUL-padded with
leftovers. Every id the document uses is named here, in all 265 files.

## The map, scored

The research walker (`host/tools/stw/`) scored against the `.TXT`s by
words, since Atari Works' text export wraps lines at the screen:
256 of 262 identical. Of the six: two label sheets differ only where the
`.TXT` wraps inside a long run of fields; JPEG where it breaks a long
underline; JAG_MEM3 by two words around a tab; JAG_FAQ and JOB_DESC's
`.TXT`s are a different revision of the document. Page geometry matches
the `.RTF`'s `\paperw`/`\marg*` in all 263 that have one; alignment
matches the `.RTF`'s `\q*` (allowing for the walker's rough pairing of
paragraphs). All 47 `Legal/STWorks` files walk clean.

## What was first known about the format

From a first read of `ADAR.STW` (52,320 bytes) — verified against its own
`.TXT`, not assumed:

| Offset | Content |
| --- | --- |
| 0 | Magic `"11"` (`0x3131`), presumably a version marker |
| 2..33 | Unidentified. Word-sized fields, several plausibly counts/offsets |
| 34 | `8.0` — text area width, inches |
| 42 | `10.6` — text area height, inches |
| 50 | `1.0` — margin, inches |
| 58 | `1.0` — margin, inches |
| 66 | `0.75` — margin, inches |
| 74 | `0.75` — margin, inches |

Page geometry is stored as **big-endian IEEE-754 doubles**. Two things
follow. It is a 68k format through and through, so it matches this
project's own "big-endian is canonical on disk" rule (CLAUDE.md) rather
than fighting it. And the values map straight onto `wp_section`'s
`page_width` / `page_height` / `margin_*`, once converted from inches to
twips (× 1440).

Body text in that specimen begins at byte 19,529, after a large prelude
that is presumably font, style and formatting tables. The prose there
matches the `.TXT` version exactly, which is what confirms these really
are the same document.

## Known unknowns

Stated rather than glossed, per this project's habit:

- The ~19KB prelude is entirely unmapped. Style runs, fonts and tabs are
  in there somewhere.
- Nothing is known yet about character styling, paragraph alignment,
  tabs, headers/footers or page breaks.
- `"11"` is assumed to be a version. Other Atari Works versions may
  differ, and only one has been examined.
- Atari Works also produced database and spreadsheet documents. Out of
  scope: this project is a word processor.
- Whether `.STW` from the Legal/ set differs from the patton/word set has
  not been checked.

## Suggested order of work

1. Extract text-only from `.STW` and diff against the matching `.TXT`
   across all 263 triples. A pass rate over that corpus is the gate.
2. Map the prelude by diffing specimens that differ in one known way.
3. Add styling, scored against the matching `.RTF`.
4. Page geometry into `wp_section` — the one part already understood.

Export is explicitly not a goal until import is solid, and the same
staging WordStar used applies: a manuscript that opens imperfectly is
worth immeasurably more than one that will not open.
