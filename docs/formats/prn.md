# Printer output (`format_prn`)

What File > Print sends to a machine's own printer port when it prints
there rather than on the gateway's printer: the ST's `PRN:` (GEMDOS
handle 3, the Centronics port), the Amiga's `PRT:` (printer.device and
the writer's Printer preferences), DOS's `PRN`. `engine/src/format_prn.c`
makes it, from the document model like every other format, and it is
export-only -- nothing reads a printout back.

The other way to print, the gateway's printer, sends the document's RTF
to the gateway, which prints its PDF (`OFFLOAD_PRINT`,
docs/formats/offload-protocol.md). That one keeps fonts and styles; this
one is what a character printer can do on its own.

## Pitch and pages

A character printer prints a fixed pitch: `cpi` characters to the inch
across and `lpi` lines to the inch down -- 10 and 6, a dot-matrix
printer's power-on defaults, unless a caller says otherwise. Each
section's paper and margins are turned into that grid, rounding to the
nearest column or line:

- the text column is `(page_width - margin_left - margin_right) * cpi /
  1440` columns (at most 255), printed after `margin_left * cpi / 1440`
  spaces;
- a page is `page_height * lpi / 1440` lines: the top margin's lines,
  then the text lines, then the bottom margin's. Margins that leave no
  room keep one text line.

Every page ends with a form feed (0x0C), the last one too, so the printer
ejects it; nothing pads the bottom margin out with blank lines unless a
footer has to be printed there. A section begins a new page, as RTF's
`\sbkpage` does. A paragraph with "page break before" begins one too,
unless it is already at the top of a page.

Lines end CR LF for a printer (`crlf`), LF otherwise. Trailing spaces are
never sent.

## Paragraphs

- **Wrapping** is at spaces, greedily. A word longer than the line is cut
  at the line's end. Spaces at the start of a continuation line are
  dropped.
- **Indents**: left, right and first-line, each rounded to whole columns.
- **Alignment**: centre and right pad the line on the left; justify widens
  the gaps between words to fill the line, the extra spread evenly with
  the earlier gaps taking the remainder, never on a paragraph's last line.
  A line holding a tab is left as it is, since its columns are the tabs'.
- **Tabs**: every stop is treated as a left stop at its column, measured
  from the text column's left edge as the ruler measures it; past the last
  stop, or with none, every half inch. A tab past the line's end breaks
  the line.
- **Spacing**: space before and after, and the line spacing's extra below
  each line, are added up in twips and printed as whole blank lines when
  they reach one. So double spacing prints a blank line after every line,
  and 1.5 alternates single and double -- a line printer cannot move half
  a line. Spacing that falls at the top of a page is dropped, as a word
  processor drops a paragraph's space before there.
- **Pictures** print as `[Picture]`.
- **Character styles are not printed.** Bold, italic, underline, fonts and
  sizes would each be one printer family's escape codes (Epson's ESC/P is
  not the Amiga's printer.device codes, and DOS printers vary); a printout
  that only some printers can read is worse than a plain one.

## Headers and footers

A section's header prints on the middle line of the top margin and its
footer on the middle line of the bottom margin, in the text column. They
follow the same rule as everywhere else (docmodel.h): `|` splits the line
into parts -- one part is centred, two go left and right, three left,
centre and right, any further `|` belonging to the third -- and `#` is the
page number. Pages are numbered through the whole document. A margin with
no lines in it has no room, and its header or footer is not printed.

## Characters

The document's text is UTF-8; a printer takes bytes in its own character
set, given as `codepage` in codepage.h's numbers:

- **437** on the ST and DOS. An Epson-compatible printer's default set is
  the PC's, and the ST's own character set largely follows it.
- **1252** on the Amiga, whose printer.device takes ISO 8859-1. The bytes
  0x80-0x9F, where 1252 puts the euro and typographic quotes, are never
  sent: 8859-1 reads them as control codes (0x9B is CSI, an escape
  sequence's start).
- **0**, plain 7-bit ASCII.

A character the code page cannot hold is printed as the nearest ASCII:
typographic quotes as `'` and `"`, dashes and minus as `-`, bullets as
`*`, non-breaking and typographic spaces as a space, an ellipsis as `...`;
a soft hyphen is dropped. Anything else, and any malformed UTF-8, prints
as `?`. Each character is one column, after this mapping, which is why the
mapping happens before the line is laid out.

## Memory

The printout is never held whole. It is sent to the caller's write
function a line or less at a time, so it can go straight to the printer
port; the export keeps one paragraph's printer bytes (three a source byte
at most) and a few lines of working space, all on the heap rather than the
stack, which on the Amiga is 4 KB.
