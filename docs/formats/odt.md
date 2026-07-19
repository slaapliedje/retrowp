# ODT Import/Export (M7)

`offload/gateway/odt_gateway.py` answers `OFFLOAD_CONVERT_IMPORT`/
`OFFLOAD_CONVERT_EXPORT` for `format_id = FORMAT_ODT (1)`. It is NOT
engine code -- it never links into `engine/`, `platform/atari/`, or
anything else the 1MB target runs. It is host-side (eventually Pi-side)
tooling, written in Python per CLAUDE.md's explicit allowance for
`offload/gateway`, converting ODF's ZIP+XML container to and from the
same RTF subset `docs/formats/rtf.md` already documents. See
`docs/formats/offload-protocol.md` for the wire contract and the design
reasoning behind using RTF as the interchange format at all.

## Scope

Reads/writes exactly the ODF subset that round-trips through the RTF
subset `format_rtf_import`/`format_rtf_export` already support:
paragraph alignment (`fo:text-align`), left/right/first-line indent and
space-before/after (`fo:margin-left`/`margin-right`/`text-indent`/
`margin-top`/`margin-bottom`), and run-level bold/italic/underline/
strikethrough/superscript/subscript/font-size/color (`fo:font-weight`,
`fo:font-style`, `style:text-underline-style`,
`style:text-line-through-style`, `style:text-position`, `fo:font-size`,
`fo:color`). Anything else in a real-world ODT (tables, images, lists,
headers/footers, tracked changes, embedded objects) is silently absent
from the imported document -- the same class of narrowing `rtf.md`
already accepts for RTF, not a new kind of gap.

**Units.** ODF lengths are CSS-like strings with an explicit unit
(`"0.5in"`, `"1.27cm"`, `36pt`, etc.) -- `twips_from_odf_len()` handles
`in`/`cm`/`mm`/`pt`/`px`, defaulting to inches if no suffix is present
(matching ODF's own convention of always expecting a unit; a bare
number is an out-of-spec input this gateway tolerates rather than
rejects). Twips (1/1440 inch) are the document model's own indent/
spacing unit (`docs/document-model.md`), so this conversion happens once,
at the ODF boundary, the same way `format_rtf_import`/`export` convert
RTF's own twips-native units directly with no intermediate unit.

**Minimal but real ODT output.** `paragraphs_to_odt_bytes()` writes only
`mimetype`, `META-INF/manifest.xml`, and `content.xml` -- no
`styles.xml`, `meta.xml`, `settings.xml`, or `Thumbnails/`, all optional
per ODF 1.2. Verified openable by real LibreOffice
(`soffice --headless --convert-to txt`, same cross-validation method
`rtf.md` used for RTF) during development, not just self-consistent with
this gateway's own importer. Styles are not deduplicated -- one
paragraph style and one text-run style per actual paragraph/run
instance (`P1`, `P2`, `P1T1`, `P1T2`, ...), the same pattern real
LibreOffice output itself uses; simpler to generate, still fully valid
ODF.

**The RTF reader here is deliberately narrow.** `rtf_bytes_to_paragraphs()`
only needs to understand the ONE shape `format_rtf_export()` actually
produces (documented in `rtf.md`'s "Export" section: `\pard` per
paragraph, `{`-delimited sibling run groups, a fixed `\fonttbl`/
`\colortbl` header) -- not arbitrary third-party RTF. The wire contract
guarantees CONVERT_EXPORT's RTF payload always comes from this project's
own `format_rtf_export()`, so this is a correctness boundary, not a
missing feature: a hand-crafted or third-party RTF file with different
formatting (e.g. real nested character-formatting scopes, which classic
RTF allows and this project's own exporter never produces) is out of
scope and not attempted.

## Real bugs found and fixed during development

Both were caught by a real, executable round-trip test before this
landed, not by inspection:

1. **Regex groups are `bytes`, not `str`.** `_RTF_TOKEN_RE` is a bytes
   pattern (`rb"..."`) matched against the raw RTF bytes, so every
   captured group (`word`, `brace`, `text`) comes back as `bytes`.
   Comparing `word == 'pard'` (bytes vs. `str`) is always `False` in
   Python 3 -- silently, no exception, no visible symptom beyond "the
   parser does nothing" (0 paragraphs recovered from a real, valid RTF
   file). Fixed by decoding `word`/`brace` to `str` immediately after
   unpacking each regex match.
2. **Runs weren't flushed on brace boundaries.** The first version only
   tracked `\fonttbl`/`\colortbl` destination scoping via a brace-depth
   stack; it never reset the "current run" state on `{`/`}`, so
   `{\b\i\ul\fs36 bold italic underline}{\strike\fs24\cf1 struck and
   colored}` -- two sibling run groups -- merged into a single run whose
   text was the concatenation of both and whose style flags were
   whichever ran last (`strike` present, but `underline` from the first
   run leaking through too). Fixed by flushing the current run into the
   paragraph (if it has text) and starting a fresh one on every `{` and
   every `}` -- correct because `format_rtf_export`'s runs are always
   sibling groups at paragraph level, never nested inside each other.

Both were found by round-tripping a fixture built directly from
`test_format_rtf_export.c`'s own attribute-covering document (via a
throwaway C tool linking the already-built engine objects and calling
`format_rtf_export()` for real, not a hand-written RTF string) through
`rtf_bytes_to_paragraphs()` and comparing against the known-correct
source attributes.

## Verification

Two layers, deliberately different in what they can prove:

1. **Automated, deterministic** (`host/tests/test_convert.c`, part of
   `make test`): spawns `odt_gateway.py` via `gateway_harness.c`'s
   `gateway_start_bin()` (a small refactor of the existing mock-gateway
   spawn/port-capture helper to take an explicit binary path) and drives
   it through the real `offload_client.c` API.
   - `test_convert_odt_import_via_gateway`: a real, checked-in fixture
     ODT (`host/tests/fixtures/sample.odt`, produced by real LibreOffice
     from a known RTF source) round-trips through CONVERT_IMPORT into
     RTF, then through the real `format_rtf_import()`, asserting exact
     paragraph/run structure and every supported attribute.
   - `test_convert_odt_export_roundtrip_via_gateway`: builds a document
     directly via the structural mutation API (independent of the import
     test), exports to RTF, ships it through CONVERT_EXPORT to get ODT
     bytes back (checked for a real ZIP local-file-header magic,
     `PK\x03\x04` -- this project has no XML/zip library on the engine
     side to do a full `content.xml` parse, nor should it), then feeds
     those SAME ODT bytes back through CONVERT_IMPORT on the same
     connection and asserts the attributes survive the full
     model->RTF->ODT->RTF->model loop. This closed loop needs no
     external tool.
   - Unsupported format IDs and malformed (too-short, not-a-zip)
     payloads are asserted to return `OFFLOAD_UNSUPPORTED`/`OFFLOAD_ERR`,
     never a crash or a hang.
2. **Manual, real-world cross-validation** (done during development, not
   automated -- see the two bugs above for why an automated-only
   approach wasn't trusted on its own): a real LibreOffice-produced ODT
   (bold/italic/underline/color/alignment/indent, built by converting a
   hand-written RTF fixture through `soffice --headless --convert-to
   odt`) was imported, the resulting RTF fed to the real
   `format_rtf_import()` C function, and the attributes checked by eye
   against the source. Separately, a document exported to ODT by this
   gateway was opened with `soffice --headless --convert-to txt` and
   the extracted text checked against expectations -- confirming the
   output is genuinely well-formed ODF readable by real-world software,
   the same standard `rtf.md`'s own RTF export verification already
   holds itself to.

## Known gaps

- **PDF export (`format_id = FORMAT_PDF`) is not implemented.**
  `CONVERT_EXPORT` with `format_id = 2` returns `OFFLOAD_UNSUPPORTED`
  from this gateway. Unlike ODT (which just carries structured,
  already-tagged text -- the same shape RTF already has), a real PDF
  needs this gateway to do its OWN text layout: word-wrap, pagination,
  and explicit per-line positioning in the content stream, none of which
  the ODT work above needed. A genuinely separate, sizable piece of work
  from ODT import/export, deliberately not attempted in the same pass --
  see `docs/milestones.md`'s M7 entry.
- **No resync on a corrupted frame.** `odt_gateway.py` reimplements the
  wire protocol's framing independently in Python (there is no shared
  module a Python gateway can link against the way `gw_common.c` serves
  `mock_gateway.c`/`ollama_gateway.c`), but deliberately does NOT
  reimplement `wire.c`'s magic-byte rescan-on-corruption behavior -- a
  malformed frame just tears down the connection. Reasonable for host-
  side tooling on a reliable local transport (TCP loopback, or the FIFO
  bridge `ci_atari_serial.sh`-style tooling uses); a real Pi deployment
  over noisy RS-232 might want the fuller behavior, not implemented here.
- **UI wiring: done (M7 T7.4).** File Open/Save in `platform/atari/
  app_shell.c` now recognize `.ODT` the same way they already recognized
  `.RTF` -- see `docs/atari-platform.md`'s "ODT + PDF wired into the UI"
  section for the full writeup, including a real engine-side fix
  (`OFFLOAD_MAX_RESPONSE_PAYLOAD`) that turned out to be necessary along
  the way, and new real-serial verification
  (`ci_atari_convert_serial.sh`) that goes beyond what this document's
  own verification section above covers (a real whole-file transfer over
  real RS-232, not just TCP loopback or manual LibreOffice cross-checks).
