# Atari Works (`.STW`)

**Status: not implemented.** This is the research record for an importer,
written while the specimens were in front of me so the next person does
not start from a blank page.

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

## What is known about the format

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
