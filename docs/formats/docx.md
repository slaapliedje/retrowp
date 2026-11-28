# Word (.docx) Import and Export

`offload/gateway/docx_reader.py`, called by `odt_gateway.py`, answers
`OFFLOAD_CONVERT_IMPORT` for `format_id = FORMAT_DOCX (3)`: the file's
bytes go to the Pi, and the RTF that comes back is read by
`format_rtf_import` like any other (`docs/formats/offload-protocol.md`).
`docx_writer.py` answers `CONVERT_EXPORT` for the same id (M24 T24.6,
2026-11-27): the engine's RTF goes to the Pi and a `.docx` comes back
(see "Writing Word" below). A document opened from Word still saves as
an `.RTF` beside it, because the writer has only what RetroWP's document
holds -- Word's lists, tables and pictures would be lost written back
over the original; Save As a `.docx` name (or `.DOC`, below) writes Word.

The reader uses Python's standard library only (`zipfile`,
`xml.etree`), as the ODT reader does; the Pi has neither python-docx nor
LibreOffice, and needs neither. Elements are matched by local name, so
both Transitional (Word's default) and Strict Open XML read.

## Which files go

On the machine (`wp_action_load_file`): a name ending `.docx` goes by its
name. A `.DOC` goes only if it is a ZIP -- that is what an 8.3 disk makes
of a `.docx` -- and so does a name with no extension (a Mac's usual). Any
other `.DOC` (WordStar's, or text) is read on the machine as before, and
a Word 97-2003 `.doc` (an OLE2 compound file, `D0 CF 11 E0`) is told it
can't be read rather than shown as garbage.

## What is carried

| Word | Becomes |
|---|---|
| `w:jc`, `w:ind`, `w:spacing` before/after/line, `w:tabs`, `w:pageBreakBefore` | the paragraph's alignment, indents, spacing, line spacing (`auto` as RTF's `\slmult1` multiple; `atLeast`/`exact` as "at least"), tab stops, page break |
| `w:b`, `w:i`, `w:u`, `w:strike`/`w:dstrike`, `w:vertAlign`, `w:sz`, `w:color`, `w:rFonts` | the run's bold, italic, underline, strike, super/subscript, size, colour, face |
| `docDefaults`, `basedOn` chains, the default paragraph style, `w:rStyle` | applied in Word's order: defaults, paragraph style, character style, direct |
| theme fonts (`asciiTheme="minorHAnsi"`) | the theme's face (`theme1.xml`'s minor/major Latin) |
| `w:numPr` | the list's number (decimal, letters, roman, as `lvlText` spells it) or bullet, then a tab, with the level's indent |
| `w:tbl` | a row per paragraph, cells apart by tabs; a cell of several paragraphs breaks its row into them |
| `w:br w:type="page"` / `"column"` | a new paragraph starting on a new page; a plain `w:br` a new paragraph |
| `w:sectPr` | sections: page size and margins, the default header and footer |
| `PAGE` (simple or complex field) in a header or footer | the engine's page number; Word's cached number is dropped |
| other fields | their shown result; the instructions are skipped |
| `w:ins`, `w:hyperlink`, `w:sdt`, `w:smartTag`, `mc:AlternateContent` | their text (`mc:Choice`, never the repeated `mc:Fallback`) |

## Unicode, Greek and the Symbol font

Word stores text as Unicode, so Greek (or any script) reaches the
document model as itself, UTF-8; how a machine draws it is its own
backend's business. The exception is the **Symbol** font, whose
characters are stored as the Latin letters in its slots: Word's Insert
Symbol writes `<w:sym w:font="Symbol" w:char="F061"/>`, and older
documents type Greek in Symbol directly. Both become the Unicode
characters they show (`SYMBOL_TO_UNICODE`, Adobe's Symbol encoding), and
the run loses the Symbol face so it is drawn as Unicode. A list bullet in
Symbol or Wingdings (Word's own default bullet is Symbol's `F0B7`)
becomes U+2022.

## A section with no header of its own

Word gives a section that names no header (or footer) the one before it
(ECMA-376 17.10.5); a section's own empty header is how it says it has
none. The reader did not, until T24.6: the fixture's second section,
which names none, came in headerless where Word and LibreOffice show
"Σελίδα 3" on it. It now carries the header on, and the T24.1 test that
asserted the empty one says so.

## Writing Word

`docx_writer.py`, standard library only like the reader, writes what the
gateway's paragraph model holds:

| RetroWP | Word |
|---|---|
| alignment, indents, spacing before/after, line spacing, tab stops, page break before | `w:jc`, `w:ind`, `w:spacing` (a multiple as `lineRule="auto"` in 240ths, Word's own unit; "at least" as `atLeast`), `w:tabs`, `w:pageBreakBefore` |
| bold, italic, underline, strike, super/subscript, colour, size, face | `w:b`, `w:i`, `w:u`, `w:strike`, `w:vertAlign`, `w:color`, `w:sz`, `w:rFonts` |
| a run with Greek in it | `w:lang w:val="el-GR"`, so Word checks it as Greek |
| each section's page, orientation and margins | `w:sectPr` on the section's last paragraph (the last section's on the body) |
| each section's header and footer | `header*.xml` / `footer*.xml`, `\|` parts on a centre and a right tab stop, `#` a `PAGE` field; every section names its own, an empty one included, since Word would otherwise give it the one before |

The default face is Arial, 12 point, and Helvetica, Times and Courier --
the PostScript names an ST, an Amiga or a Mac writes -- become Arial,
Times New Roman and Courier New, which Windows has, Greek included.
`settings.xml` says `compatibilityMode` 15 so Word 2013 and later,
Word 2021 among them, open it in their own mode rather than
Compatibility Mode. Elements follow the schema's order inside `w:pPr`,
`w:rPr` and `w:sectPr`, which Word enforces. Every ZIP entry carries the
same date, so the same document makes the same bytes.

On the machine, Save under a `.docx` name, or `.DOC` -- the name an 8.3
disk (an ST's, a DOS machine's) gives Word's file, and the one the reader
already takes as Word when it is a ZIP -- goes through the gateway; with
no gateway it says so and writes nothing. A WordStar `.DOC`, opened and
saved under its own name, now goes to Word through the gateway too, where
it used to be written over as plain text.

Checked: the Greek fixture in, out as `.docx` and in again keeps its
text, paragraph and run formatting, sections, page sizes and headers
(the one change: a run with no face comes back naming the default, Arial);
LibreOffice reads the written file to the same text as Word's original,
less the original's list numbering and table (the model has neither) and
with real Greek where the original's Symbol-font letters extract as
Latin. Host tests `convert_docx_export_reads_back` and
`actions_word_files_open_through_the_gateway` (Save As `.docx` and `.DOC`).
Opening it in Word itself is the user's to do.

## Not carried

Deletions (`w:del`), field instructions, pictures and text boxes
(`w:drawing`, `w:pict`), footnotes, endnotes and comments, first-page and
even-page headers, columns, cell shading and borders.

## Tested

`host/tests/test_convert.c` (`convert_docx_*`) on
`host/tests/fixtures/sample_greek.docx`, which
`host/tests/fixtures/make_greek_docx.py` writes in the shape Word 2021
saves; `host/tests/test_actions.c` (`actions_word_files_open_through_the_gateway`)
for the file flow. On 2026-11-27 five real Word files (1,860 to 26,773
words) were compared word for word with LibreOffice's own reading of
them (`soffice --convert-to txt`): four match exactly, and in the fifth
the reader keeps 37 words of table rows that LibreOffice's text export
drops. Each one's RTF went through `format_rtf_import` under ASan and
UBSan without a fault.
