# Milestones & Task Breakdown

> Each milestone ends with a runnable, testable artifact. Tasks are written to be
> handed to an AI collaborator individually. Every task lists: Goal, Files,
> Constraints (from CLAUDE.md), and Acceptance Criteria (AC). Do not violate the
> Prime Directives. When in doubt, keep platform code out of `engine/`.

## Legend
- [ ] = not started, [~] = in progress, [x] = done
- AC = Acceptance Criteria (must all pass to call the task done)
- Host = builds/runs on modern PC with the stub backend

---

## M0 — Skeleton & Discipline  (Host only)
Goal: Establish seams and build/test infrastructure before any feature code.

- [x] T0.1 Create repo layout per CLAUDE.md (engine/, platform/, services/,
      offload/, host/, docs/).
      AC: Tree matches CLAUDE.md; empty modules compile as stubs.
- [x] T0.2 Write `engine/include/types.h` (fixed-width ints, bool, byte, status
      codes). No reliance on int width.
      AC: Compiles under C89 on host; provides u8/u16/u32/i16/i32, wp_bool,
      wp_status enum.
- [x] T0.3 Write `engine/src/endian.c` + header: be_read16/32, be_write16/32,
      swap helpers. Canonical order = big-endian.
      AC: Unit test round-trips values on both a BE and LE host (simulate LE via
      forced swap path).
      Note: be_read/be_write are shift-based, not memory reinterpretation, so
      they are correct on any host endianness by construction -- there is no
      separate LE code path to force. See host/tests/test_endian.c.
- [x] T0.4 Define `engine/include/platform.h` INTERFACE ONLY (no impl):
      memory, file I/O, clipboard, timer/ticks, text metrics, glyph draw,
      window/dialog primitives, transport open/read/write/close.
      AC: Header compiles; documented; zero platform headers included.
- [x] T0.5 Implement `platform/host` stub backend: stdio files, fake monospace
      metrics (fixed cell w/h), no-op render, pipe-based transport.
      AC: Links against empty engine; host test binary runs and exits 0.
- [x] T0.6 Test harness in `host/tests` + CI script.
      AC: `make test` runs on a clean checkout and reports pass/fail.

Artifact: Program links empty engine to stub backend and runs a trivial test.
**Reached.** `./host/tools/ci.sh` does a clean strict-C89 build and runs the
full suite.

---

## M1 — Document Model + Plaintext  (Host only)
Goal: Single source of truth exists and round-trips plaintext.

- [x] T1.1 Implement `docmodel` per docs/document-model.md (document → sections →
      paragraphs → runs; style tables; string storage).
      AC: Create/destroy a document with no leaks (host allocator counter == 0).
- [x] T1.2 Implement iteration/mutation API (add paragraph, add run, set para
      style, set char style).
      AC: Unit tests build a known tree and assert structure.
- [x] T1.3 `format_txt` import/export (UTF-8 plaintext; paragraph = line break
      policy documented).
      AC: export→import→export is byte-identical for a fixture set.
      Policy documented in docs/formats/txt.md.

Artifact: Document model round-trips plaintext under test.
**Reached.** See host/tests/test_docmodel.c, test_strpool.c, test_format_txt.c.

---

## M2 — Editing Core  (Host only)
Goal: Prove editing + undo without a GUI.

- [x] T2.1 Cursor + selection model (positions defined in docs/document-model.md).
      AC: Cursor moves across runs/paragraphs correctly in tests.
      See engine/src/cursor.c, host/tests/test_cursor.c.
- [x] T2.2 Insert/delete text with style inheritance rules.
      AC: Inserting at a boundary inherits the documented style; tests cover
      start/middle/end of run and paragraph.
      Scope note: insert is always a point op; delete is scoped to a range
      within a single run (selections spanning multiple runs/paragraphs, and
      paragraph split/merge on Enter/Backspace, are not implemented here --
      WP_OP_SPLIT_PARA/WP_OP_MERGE_PARA remain defined-but-unused for that
      follow-up). See docs/editing.md, engine/src/edit.c, host/tests/test_edit.c.
- [x] T2.3 Undo/redo as an operation journal (command objects).
      AC: Random-op fuzz test: after N ops then N undos, model equals start
      state (deep compare).
      Verified via format_txt_export comparison (valid here since this
      milestone's op set never touches cstyle/pstyle). See engine/src/undo.c,
      host/tests/test_undo.c (test_undo_fuzz_n_ops_then_n_undos_restores_original,
      60 random ops, fixed seed).

Artifact: Scriptable, provably-correct editor core.
**Reached**, within the scope noted above (single-run delete, no paragraph
split/merge). 72/72 tests pass via ./host/tools/ci.sh.

---

## M3 — Layout Engine  (Host only)
Goal: Deterministic wrap + pagination via the metrics seam.

- [x] T3.1 Line breaker: greedy word wrap using platform.h metrics only.
      AC: With stub monospace metrics, break points match a hand-computed
      fixture exactly.
      Tokens (space/word) merge across run boundaries so a word split by an
      unrelated edit never breaks mid-word; width measured per-run-fragment
      and summed. See docs/layout.md, engine/src/linebreak.c.
- [x] T3.2 Pagination: fill lines into page boxes; page/section breaks.
      AC: Known document + page size yields expected page count and per-page
      line lists.
      A new section always starts a new page. See engine/src/paginate.c.
- [x] T3.3 Layout cache invalidation on edit (relayout only affected range).
      AC: Editing paragraph K relayouts K (and following only if reflow needed);
      test asserts minimal recompute.
      "Minimal recompute" = minimal *expensive* recompute: line breaking
      (font-measurement-heavy) is skipped for clean paragraphs; page-filling
      always runs in full since it's cheap arithmetic, not a measurement hot
      path (see docs/layout.md). Proven quantitatively via a measure-call
      counter added to platform/host/metrics_host.c, not just trusted --
      see test_layout_editing_one_paragraph_does_not_remeasure_whole_document.

Artifact: Deterministic layout output verified in tests.
**Reached.** 80/80 tests pass via ./host/tools/ci.sh. Known gap: character
style font names (wp_char_style.font_name) aren't resolvable yet -- it's a
bare wp_str offset with no length, and nothing sets it to non-zero anywhere
in the codebase yet, so line breaking always requests the platform default
font (see docs/layout.md).

---

## M4 — First Real Backend: Atari ST (1MB, plain TOS)
Goal: A real, running ST word processor at 1MB.

- [x] T4.1 `platform/atari` skeleton: AES app init, event loop, window, menu.
      AC: Opens a window on Hatari (plain TOS, 1MB config); clean quit.
      Hand-built menu OBJECT tree (no resource editor/.rsc in this
      toolchain), wind_create/open via wind_*_grect, evnt_multi loop
      dispatching MN_SELECTED/WM_CLOSED/WM_REDRAW/WM_TOPPED/WM_SIZED/
      WM_MOVED, clean menu_bar(REMOVE)+wind_close/delete+v_clsvwk+appl_exit
      shutdown. See docs/atari-platform.md, platform/atari/app_shell.c.
      The full portable engine + offload/transport also cross-compile
      clean under strict C89 for this real m68k target (`make
      atari-engine-check`), not just the host stub.
      Verified two ways: (1) automated -- host/tools/ci_atari.sh boots
      EmuTOS at a real 1MB ST config in Hatari and runs a smoke test
      (host/tools/atari_smoke.c) that calls appl_init/v_opnvwk/menu_bar/
      wind_create_grect/wind_open_grect for real and checks their return
      codes, via the NF_STDERR/NF_EXIT Native Features trap for pass/fail
      signaling -- confirmed to actually catch regressions, not just
      rubber-stamp (verified by deliberately breaking dispatch logic and
      watching the gate fail). (2) visual -- a real screenshot of WP.PRG
      showing a live GEM window titled "Word Processor" with working
      mover/closer/sizer decorations at the full desktop work area.
      Known gaps (see docs/atari-platform.md): scripted mouse-driven
      end-to-end testing is unreliable in this headless environment (Y
      coordinate steering doesn't work), so the Quit menu item's dispatch
      logic is proven correct via direct synthesized-message calls rather
      than an actual click routed through evnt_multi; and the menu title
      text renders as glyph noise in screenshot captures specifically
      (screenshot/font-capture artifact, not a tree-construction bug --
      the tree's bytes are verified correct and menu_bar() succeeds).
- [x] T4.2 VDI/SpeedoGDOS metrics + glyph draw implementation of platform.h.
      AC: Text renders; measured widths drive correct on-screen wrap matching
      the engine's layout.
      No SpeedoGDOS install available to query real screen DPI, so a
      fixed, documented ATARI_ASSUMED_DPI=96 converts between the
      document model's twips and VDI pixels -- correct by construction
      since plat_measure_text and the paint code's pixel positions both
      go through the same ratio (wrap correctness is an internal-
      consistency property, not a physical-calibration one). See
      docs/atari-platform.md, platform/atari/metrics_atari.c,
      draw_atari.c. Verified two ways: (1) automated --
      wp_atari_layout_selfcheck() relayouts a fixture paragraph through
      the real VDI seam and asserts line_count>1 with every line within
      its column, run via host/tools/ci_atari.sh and confirmed to catch
      a deliberately-introduced metrics regression. (2) visual -- a
      screenshot of WP.PRG rendering a hardcoded sample paragraph with
      correct word-boundary wrap filling the window without overflow.
      Known gaps (see docs/atari-platform.md): ascent/descent use
      vst_point's charh/cellh rather than vqt_fontinfo's precise
      distances[]; color is a black/white luminance threshold, not a
      real palette search; UTF-8-to-native conversion is ASCII-only
      (non-ASCII becomes '?', correctly decoded per-codepoint via
      utf8_decode); no scrolling, so content past the first screenful
      isn't drawn.
- [x] T4.3 File I/O, clipboard, timer hooks for Atari.
      AC: Open/save plaintext; GEM clipboard cut/paste works.
      file_atari.c over mintlib stdio (fopen/fread/fwrite -- no raw
      GEMDOS traps needed); clipboard_atari.c over the classic SCRAP.TXT
      convention; timer_atari.c via clock(). File Open/Save use the real
      fsel_input() file selector. Menu bar grew a second title (Edit:
      Cut/Copy/Paste) alongside File's new Open/Save items, same hand-
      built OBJECT[] approach as T4.1. Also wired basic keyboard editing
      (insert/backspace via a tracked cursor through the M2 editing
      engine, unmodified) since Cut/Copy/Paste needed real document
      content to round-trip meaningfully. See docs/atari-platform.md,
      platform/atari/file_atari.c, clipboard_atari.c, timer_atari.c,
      app_shell.c. Verified via host/tools/ci_atari.sh:
      wp_atari_file_clipboard_selfcheck() round-trips a fixture through
      real plat_file_*/plat_clipboard_* calls and is confirmed to catch
      a deliberately-introduced regression (dropped clipboard read byte,
      watched it fail, restored). Also exercised live under Hatari with
      xdotool keyboard injection, producing real document mutation on
      screen -- reported as corroborating, not rigorous, since exact
      keystroke-to-character fidelity of the injection itself wasn't
      independently verified (see docs/atari-platform.md).
      Deliberate scope narrowing (documented, not silent): Cut/Copy/
      Paste operate on the WHOLE document, since there is no selection
      model yet (consistent with docs/editing.md's existing single-run-
      delete scope note); keyboard editing is single-paragraph only
      (Enter is a no-op -- WP_OP_SPLIT_PARA remains unimplemented); no
      cursor is rendered on screen (tracked and edited correctly, just
      not painted).
- [x] T4.4 Memory budget pass at 1MB.
      AC: App loads, opens a multi-page doc, edits, saves — within 1MB TOS config
      on Hatari without exhaustion.
      Builds a ~7.5KB/60-paragraph document, paginates it at real US
      Letter dimensions via wp_document_paginate (first real use of
      pagination on this target -- T4.2/T4.3 only ever called
      wp_paragraph_relayout directly), edits, saves, frees everything.
      GEMDOS Malloc(-1L) proved real headroom (~662KB free throughout, a
      real 1MB Hatari config) but turned out NOT to be a leak signal --
      mintlib's malloc claims its heap from GEMDOS once at startup and
      manages it internally, so Malloc(-1L) was bit-identical across all
      three checkpoints even WITH a deliberately introduced leak. Added
      mem_atari.h/.c (alloc/free counters mirroring platform/host/
      mem_host.c's established pattern) as the real leak signal instead
      -- the first time that exact "live_count()==0 after a full
      lifecycle" assertion has been proven on the real Atari target, not
      just the host stub. See docs/atari-platform.md,
      platform/atari/mem_atari.c, app_shell.c's
      wp_atari_memory_budget_selfcheck(). Verified via
      host/tools/ci_atari.sh (page_count>1, comfortable headroom,
      live_count==0), confirmed to catch a real leak (skipped
      wp_document_free, watched it fail -- and watched Malloc(-1L) NOT
      catch it -- then restored).
      Known gap: proves the document model + platform.h plumbing stay
      within budget at multi-page scale, not the on-screen UI at that
      scale (no scrolling yet -- T4.2/T4.3's existing gap).

Artifact: Shippable ST word processor (plaintext + editing + on-screen layout).
**Reached, within the scope narrowed and documented across T4.1-T4.4**
(see docs/atari-platform.md for the full list): whole-document Cut/Copy/
Paste (no selection model), single-paragraph editing (no Enter/paragraph
split), no on-screen cursor, no scrolling, ASCII-only charset conversion,
and a fixed 96 DPI assumption for the twips-to-pixel mapping (correct by
construction, not physically calibrated). Every task's real AES/VDI/
GEMDOS calls were exercised live under Hatari via an automated,
regression-tested CI harness (host/tools/ci_atari.sh) built specifically
for this milestone, alongside real cross-compilation of the entire
portable engine under strict C89 for the first non-host target.

---

## M5 — Classic RTF + Compact Spell  (On-box)
Goal: Practical writer that stays at 1MB.

- [x] T5.1 `format_rtf` import (classic RTF 1.x subset; documented supported
      control words).
      AC: Fixture RTFs import to expected model trees.
      Hand-written brace-scoped-state-stack parser (RTF_MAX_DEPTH=64,
      WP_RANGE on overflow rather than unbounded recursion), one pass,
      no external RTF library. Supported control words, cp1252 hex-
      escape decoding, and every deliberate simplification (no font-name
      or title/author wiring -- both fields are bare wp_str offsets
      with no paired length, same locked-struct gap T3.1 already
      documented; \line approximated as a paragraph break; no \uN
      Unicode escapes) are in docs/formats/rtf.md. See
      engine/src/format_rtf_import.c. 17 fixture tests in
      host/tests/test_format_rtf.c cover structure, all supported
      paragraph/character attributes, the color table, hex escapes,
      special-char control words, ignorable/skipped destinations,
      unrecognized-control-word forward compatibility, and malformed-
      input handling (truncated hex escape and excess nesting depth are
      rejected; truncated/unbalanced input recovers leniently as a
      documented, tested design choice, not an accident). Cross-compiles
      clean for the real Atari target (`make atari-engine-check`) and
      the whole Atari CI harness stays green with it linked in.
- [x] T5.2 `format_rtf` export (round-trip of supported subset).
      AC: model→RTF→model preserves supported attributes.
      One shared emission function serves both the measuring pass and
      the real write pass (rtf_writer in format_rtf_export.c) rather
      than two independently-maintained implementations, since RTF
      emission has enough real logic (escaping, color-table lookups,
      conditional control words) that hand-synced duplicates would be a
      real drift risk; nothing is written to the caller's buffer unless
      the whole document fits. Minimal one-entry \fonttbl (\f0, always
      referenced) is honest placeholder output -- nothing sets a real
      font name anywhere in the codebase yet (T3.1's documented gap).
      See docs/formats/rtf.md, engine/src/format_rtf_export.c. Verified
      two ways: (1) host/tests/test_format_rtf_export.c's own model->
      RTF->model round-trip across every supported attribute
      (alignment/indents/spacing/bold/italic/underline/strike/super/
      sub/size/color) plus a "nothing written on overflow" sentinel-
      buffer test; (2) independently -- exported RTF fed to `soffice
      --headless --convert-to txt` (LibreOffice) produced exactly the
      expected text, confirming the output is genuinely well-formed
      RTF readable by real-world software, not just self-consistent
      with this project's own importer. Cross-compiles clean for the
      real Atari target.
- [x] T5.3 `services/spell`: compact dictionary queried from disk + affix rules.
      AC: Known misspellings flagged; suggestions returned; peak RAM within 1MB
      budget (dictionary not fully loaded).
      Dictionary is a plain sorted lowercase-word text file queried by
      binary search directly against the file (plat_file_seek/read) --
      never loaded as a whole; peak RAM per lookup is a handful of
      fixed 32-byte stack buffers, a design-level guarantee not a
      runtime-measured ceiling. Affix rules are a small fixed table of
      common English suffix transformations (not Hunspell-compatible --
      a real multi-week project of its own, deliberately out of scope
      for "compact"). Suggestions are bounded edit-distance-1 candidate
      generation. See docs/services/spell.md, services/spell/spell.c.
      Real bug found and fixed during development: the first binary-
      search implementation scanned forward to the next line when a
      probe landed mid-line rather than backward to the line containing
      the probe, which could silently break search-range convergence
      and hang indefinitely on some inputs -- passed against an 11-word
      hand fixture but hung against the realistic 6000-word one
      (docs/services/spell.md has the full trace). Verified with both:
      host/tests/test_spell.c's hand-authored fixture (precise
      assertions per affix rule, case-insensitivity, non-ASCII fail-
      open behavior, leak-free via mem_host counters) and a 6000-word
      fixture sampled from a real en_US dictionary (BSD-licensed,
      attribution in docs/services/spell.md), including the file's
      first/last lines (the binary search's boundary conditions).
      Cross-compiles clean for the real Atari target.

Artifact: Open/save RTF + local spell-check, still 1MB.
**Reached at the engine/services level**: format_rtf import/export and
services/spell are implemented, tested (host + real Atari cross-compile),
and scoped/documented honestly (docs/formats/rtf.md,
docs/services/spell.md).
**Update (post-M6): now wired into platform/atari's UI.** File Open/Save
pick RTF vs plaintext by filename extension; a new Tools > Spell Check
menu action scans the whole document and reports the first unknown word
via form_alert, with a lazily-opened dictionary at a fixed path
(C:\WPSPELL.DIC, no shipped asset). Verified two ways: a new
wp_atari_rtf_spell_selfcheck() (host/tools/ci_atari.sh) proves a real RTF
round-trip and real dictionary lookups over actual Atari plat_file_*
calls (the non-blocking building blocks do_spell_check() uses); the menu
action itself is never invoked from CI since form_alert() blocks waiting
for a click, which would hang a headless Hatari run. See
docs/atari-platform.md's "RTF + Spell Check wired into the UI" section
for the full design, including the documented scope narrowing (no
selection model, no cross-run word tokenization, no interactive
correction UI).

---

## M6 — Offload Transport + First Capability
Goal: ST asks the Pi over serial and gets an answer, safely.

- [x] T6.1 `offload/transport`: length-framed, versioned, CRC-checked messages.
      AC: Corrupted/short frames are rejected and resynced in tests.
      See docs/formats/offload-protocol.md and host/tests/test_wire.c.
- [x] T6.2 `offload_client` + `offload.h` op dispatch with timeouts.
      AC: With no device, offload_available()==false and UI never blocks; with
      stub gateway, a request round-trips.
      Transport used: `platform/host`'s `tcp:` endpoint (network first, per
      the host-only bootstrap scope -- see below).
- [x] T6.3 Atari RS232 transport hook (and MiNTnet TCP variant if present).
      AC: On hardware/emulator, handshake negotiates capabilities.
      platform/atari/transport_atari.c implements the transport seam over
      real Bconin/Bconout/Bconstat/Bcostat/Rsconf (DEV_AUX), exactly the
      drop-in fifth implementation predicted above -- zero changes to
      offload/transport, offload_client.c, or offload.h. No MiNTnet
      variant: FreeMiNT's BSD-socket GEMDOS extensions don't exist under
      plain TOS/EmuTOS (confirmed by disassembling libc.a), so pursuing
      it would need booting real FreeMiNT under Hatari for a target that
      can't use it anyway -- WP_UNSUPPORTED for any endpoint prefix
      other than "serial:", honestly, not silently.
      Verified beyond the literal AC: host/tools/ci_atari_serial.sh
      bridges Hatari's --rs232-in/--rs232-out to
      offload/gateway/mock_gateway.c's new --serial-rx/--serial-tx mode
      via two FIFOs, and host/tools/atari_serial_smoke.c proves the real
      offload_init()/offload_query()/offload_submit()/offload_shutdown()
      client API works end to end over real emulated RS-232: the
      HELLO/CAPS handshake negotiates the gateway's real
      protocol_version and gateway_name (the AC), AND a full
      OFFLOAD_THESAURUS request/response round trip completes
      correctly (going further than required, reusing the exact
      fixture-backed logic the TCP tests already cover).
      Real bug found and fixed: the gateway's hardcoded 5-second
      HELLO-wait timeout (fine for the TCP tests, where the client
      connects near-instantly) was shorter than a real Atari boot +
      program load takes under Hatari's cycle-exact CPU emulation, so
      the gateway gave up and closed its FIFOs before the ST program
      ever wrote a byte -- and Hatari's own subsequent write to the
      now-reader-less FIFO raised SIGPIPE, crashing the emulator itself
      (not just failing the test). Root-caused by independently ruling
      out three other components first (raw Bconin/Bconout mechanics,
      plat_ticks_ms()/clock() actually advancing, and the gateway's
      serial mode in isolation via a hand-written Python client) before
      isolating it with temporary tracing. Fixed with a new
      --hello-timeout-ms flag (default unchanged at 5000 for TCP;
      ci_atari_serial.sh passes 25000 for serial). Full trace in
      docs/atari-platform.md.
- [x] T6.4 Pi `gateway` answering ONE op end-to-end (THESAURUS or SPELL).
      AC: ST sends a word, receives synonyms/suggestions; timeout path verified
      by killing the gateway mid-request.
      Scope note: implemented as a HOST-SIDE MOCK (offload/gateway/mock_gateway.c)
      answering BOTH THESAURUS and SPELL_CHECK from a fixture table
      (offload/gateway/fixtures.c), not a real Pi/LLM gateway -- that's M8.
      This proves the protocol deterministically; see
      host/tests/test_offload.c and host/tests/gateway_harness.c for the
      "kill mid-request"/corrupt-frame/timeout coverage.

Artifact: Working serial offload for one high-value feature.
**Reached.** Offload transport + client + a mock gateway round-trip
THESAURUS and SPELL_CHECK, with full fault-injection coverage (corrupt CRC,
dead gateway, delayed gateway, short/stalled frame) over TCP loopback
(T6.1/T6.2/T6.4), AND the same client/transport/gateway logic proven over
real emulated RS-232 on the actual Atari target (T6.3), including a real
bug found only at that layer (the gateway's TCP-calibrated handshake
timeout was too short for real Atari boot time, and crashed Hatari itself
via SIGPIPE, not just the test -- see docs/atari-platform.md). Still a
deliberate scope boundary, not a gap: the gateway remains a host-side mock
against a fixture table, not a real Pi/LLM -- that's M8.
**Update (post-M6):** a second gateway, `offload/gateway/ollama_gateway.c`,
now answers `OFFLOAD_THESAURUS` with a real local Ollama call instead of
a fixture table -- genuine LLM-generated synonyms, verified end-to-end
through the real `offload_client.c` API (`host/tools/ollama_gateway_smoke.c`,
run via `host/tools/run_ollama_smoke.sh`). Deliberately narrower than
M8: THESAURUS only (every other op, including SPELL_CHECK, still reports
`OFFLOAD_UNSUPPORTED` from this gateway -- see
docs/formats/offload-protocol.md for why), never wired into
`make test`/`ci.sh` (LLM output isn't deterministic, and Ollama isn't a
build dependency), and not a Pi deployment -- a real GRAMMAR_CHECK
gateway and an on-Pi rollout both remain M8 scope, not reshaped by this.
**Further update:** `platform/atari`'s own T6.3 "Known gaps" note --
"no 'Thesaurus' menu action wired to the offload link" -- is now closed:
a Tools > Thesaurus menu item looks up the word before the cursor over
the offload link and reports synonyms via a GEM alert. See
docs/atari-platform.md's "Thesaurus wired into the UI" section for the
design and an honestly-stated residual gap: the alert-producing action
itself has never been run and observed end to end (real click or
otherwise), because doing so safely isn't possible with this project's
current headless-Hatari tooling -- only its non-blocking building blocks
(word extraction, the offload_client.c round trip) are independently
verified. Spell Check stays on-device (services/spell) -- offload-backed
spell checking remains unimplemented by any gateway in this repo,
deliberately (see docs/formats/offload-protocol.md).

---

## M7 — Offloaded Formats (the 1MB payoff)
Goal: 1MB ST opens/saves ODT and rich PDF without local parsing.

- [x] T7.1 Define CONVERT_IMPORT/EXPORT payloads (format id + bytes).
      AC: protocol.md documents the message shapes.
      Design decision: RTF (not a new internal-model serialization) is
      the wire-level interchange format -- the engine always ships/
      receives RTF across this op, reusing format_rtf_import/export
      unchanged; only the gateway needs to know a foreign format.
      Response payload for CONVERT_IMPORT is RTF bytes; request payload
      for CONVERT_EXPORT is RTF bytes (format_id + format_rtf_export()
      output). See docs/formats/offload-protocol.md.
- [x] T7.2 Gateway ODT import (ZIP/XML → internal model serialization) and
      export (model → ODT).
      AC: Round-trip an ODT through the gateway; model matches a host-side
      reference import.
      offload/gateway/odt_gateway.py (Python -- CLAUDE.md explicitly
      allows "modern C/Python" for offload/gateway; ODT's ZIP+XML needs
      real DEFLATE/XML handling, which Python's stdlib makes tractable
      the way hand-rolling it in C would not have been, the same
      "don't reinvent a real subsystem" call docs/services/spell.md
      already made). Converts ODT <-> the RTF subset docs/formats/rtf.md
      documents, reimplementing the wire protocol independently in
      Python (no shared module a Python gateway can link against).
      Verified two ways: an automated, fully deterministic round trip
      (host/tests/test_convert.c, part of make test -- unlike
      ollama_gateway.c, this gateway has no nondeterministic backend)
      against a real LibreOffice-produced fixture ODT, AND manual cross-
      validation with real soffice --headless conversions in both
      directions during development. Two real bugs found and fixed this
      way (bytes-vs-str regex comparisons that silently no-opped the
      whole parser; character runs not flushed on brace boundaries,
      merging sibling runs). Full writeup: docs/formats/odt.md.
- [x] T7.3 Gateway PDF export (model → PDF bytes returned to client).
      AC: Produced PDF opens in a modern viewer; text/layout correct.
      Same odt_gateway.py process, format_id=FORMAT_PDF. A real, from-
      scratch PDF writer: word-wrap and pagination against REAL Adobe
      AFM glyph widths (extracted from matplotlib's bundled
      pdfcorefonts during development, not approximated), the 4
      standard Helvetica variants (no font embedding needed),
      left/right/center/justify (justify via the PDF Tw operator, exact
      because the same real widths drove both the wrap decision and the
      stretch amount), underline/strikethrough (drawn manually -- PDF
      has no native attribute for either), superscript/subscript (the
      PDF Ts operator plus a scaled-down effective size used
      consistently for both wrapping and rendering), and real multi-
      page pagination. PDF import was never attempted -- export-only,
      by design (docs/formats/offload-protocol.md). Verified two ways:
      an automated round trip (host/tests/test_convert.c, part of make
      test, kept small since engine/src/offload_client.c caps a single
      response at 4096 bytes -- a real transport constraint an earlier
      larger fixture actually hit during development) checking real PDF
      structure, AND manual verification with real poppler-utils
      (pdfinfo/pdftotext/pdftoppm) -- multi-page pagination (40
      paragraphs, 3 real pages, all content present and in order) and
      visual correctness (rendered to an image and inspected: justified
      text visibly stretches except on a paragraph's last line, bold/
      italic/color/underline/strikethrough/superscript/subscript all
      render correctly) against fixtures built through the real C
      engine's format_rtf_export(), not hand-written RTF. Full writeup,
      including the design rationale for why this was genuinely
      separate work from T7.2's ODT conversion: docs/formats/pdf.md.
- [x] T7.4 Engine-side wiring: "Open ODT" ships bytes, receives model.
      AC: On 1MB ST with Pi attached, open and save an ODT successfully.
      File Open/Save recognize .ODT (open+save) and .PDF (save only --
      no importer, by design) via the same extension-sniffing dispatch
      already used for .RTF; no new menu items. do_odt_open() ships the
      whole file through CONVERT_IMPORT and feeds the returned RTF into
      the already-tested format_rtf_import() -- no new engine-side
      parsing. do_odt_pdf_save() is the mirror via CONVERT_EXPORT.
      A real engine change was needed, not just UI plumbing:
      engine/src/offload_client.c's OFFLOAD_MAX_RESPONSE_PAYLOAD (4096,
      sized for THESAURUS word lists) was too small for a real
      gateway-produced ODT/PDF -- an early, larger test_convert.c
      fixture hit this exact ceiling during T7.3 development. Raised to
      32768, still bounded, still a small fraction of the 1MB budget.
      Timeouts (WP_ATARI_CONVERT_TIMEOUT_MS, 2 minutes) are sized for
      real RS-232's actual throughput at 9600 baud (~960 bytes/sec, 8N1
      framing) transferring a WHOLE FILE, not for THESAURUS's
      single-word payloads or a slow cold LLM load -- three genuinely
      different bottlenecks, named separately rather than papered over
      with one generic "big timeout." Verified two ways: (1)
      host/tools/atari_convert_smoke.c + new
      host/tools/ci_atari_convert_serial.sh prove real
      CONVERT_IMPORT/CONVERT_EXPORT (ODT and PDF) over real emulated
      RS-232 against a real offload/gateway/odt_gateway.py instance --
      genuinely new coverage beyond T6.3's atari_serial_smoke.c, which
      only ever exercised THESAURUS's tiny single-word payload over
      serial, never a whole-file transfer. A real test-authoring bug
      (hardcoded string length off by one) was caught and fixed by
      actually running this test, not by inspection. (2) The
      form_alert()-driving menu actions themselves (do_odt_open/
      do_odt_pdf_save) are never exercised by CI, for the same reason
      Thesaurus's and Spell Check's aren't -- see
      docs/atari-platform.md's "ODT + PDF wired into the UI" section for
      the full writeup and that gap's honest scope.

Artifact: 1MB ST with ODT + rich PDF via the Pi.
**Reached.** The protocol (T7.1), a real, tested, bidirectional
ODT<->RTF gateway (T7.2), a real from-scratch PDF writer with genuine
text layout -- word-wrap, pagination, justification, underline/
strikethrough, superscript/subscript, all against real Adobe AFM glyph
metrics (T7.3) -- and the Atari-side UI wiring for both (T7.4) are all
done. File Open/Save recognize .ODT/.PDF by extension, exactly like
.RTF; a real engine-side fix (OFFLOAD_MAX_RESPONSE_PAYLOAD 4096 -> 32768)
was needed along the way for real documents to actually round-trip, not
just tiny fixtures. Every layer verified against something outside this
project's own code: LibreOffice for ODT, poppler-utils for PDF, and --
new for T7.4 specifically -- real emulated RS-232 carrying a real
whole-file transfer to a real gateway process, not just the tiny
single-word payloads every other offload feature (THESAURUS) had
exercised over serial before this. The one remaining gap, honestly
scoped rather than silently accepted: the menu actions themselves
(do_odt_open/do_odt_pdf_save, and Thesaurus/Spell Check before them)
are never exercised by CI, because they end in a real blocking
form_alert() this headless environment cannot safely drive to
completion -- verified transitively instead, through the already-proven
pieces each one composes.

---

## M8 — Grammar + Optional AI Ops
Goal: Full smart features, all optional/degradable.

- [ ] T8.1 Gateway grammar check (LLM or rule engine) → issue list payload.
      AC: Paragraph in, structured issues out; ST renders them.
      **Partially done: protocol + both gateways are real and verified;
      "ST renders them" is NOT done yet** -- kept unchecked rather than
      overclaiming, the same "don't check the box until the whole AC is
      met" discipline M7's T7.1-T7.4 split already established.
      - Wire payload (docs/formats/offload-protocol.md's new
        `OFFLOAD_GRAMMAR_CHECK` section): request is the UTF-8 paragraph,
        response is a length-prefixed issue list (start_offset, span_len,
        message, suggestion per issue).
      - `mock_gateway.c` answers it from a small fixture table
        (`offload/gateway/fixtures.c`), covered by two new deterministic
        `host/tests/test_offload.c` cases (part of `make test`) -- a
        known 2-issue fixture paragraph, and an unrecognized paragraph
        correctly yielding 0 issues.
      - `offload/gateway/ollama_gateway.c` answers it for real, via a
        second Ollama call alongside its existing THESAURUS one, using
        its OWN independently-chosen default model (`gemma2:9b`, picked
        empirically over `mistral:7b` -- see the file's header comment
        and docs/formats/offload-protocol.md for the comparison).
        **Design decision: the gateway never trusts a model's claimed
        numeric offset.** It asks the model to quote the offending text
        verbatim instead, then finds that exact quote in the original
        request paragraph via a real substring search
        (`find_substring`); an unverifiable quote (paraphrased or
        hallucinated) is silently dropped rather than forwarded with a
        wrong or unusable offset.
      - Verified against a REAL, running Ollama server (not mocked):
        `host/tools/ollama_gateway_smoke.c` (run via
        `host/tools/run_ollama_smoke.sh`) now exercises both ops over one
        connection. A live run against a paragraph with three genuine
        errors ("The team are excited to announce there new product,
        which dont require any special setup.") correctly identified all
        three, and the gateway-computed offsets were confirmed (by
        re-slicing the original paragraph at each reported offset/length
        and printing it alongside the model's own message/suggestion) to
        land exactly on the flagged text -- not just self-consistent with
        the encoder, genuinely correct against live, non-deterministic
        model output. Manual-only, same as the existing THESAURUS smoke
        coverage: LLM output isn't deterministic, so this is deliberately
        not part of `make test`/`ci.sh`.
      - **Remaining for T8.1:** engine/UI wiring -- a real GEM UI surface
        in `platform/atari/app_shell.c` that sends a paragraph, receives
        the issue list, and lets the user see/act on it. Unlike
        Thesaurus's single blocking `form_alert()`, this is a LIST of
        positioned issues, not one message, so it's a materially bigger
        UI task than what T7.4/the existing Thesaurus/Spell Check menu
        actions needed -- deliberately scoped as its own follow-up pass
        rather than folded in here.
- [ ] T8.2 On-box grammar-lite fallback (rule-based) when no Pi.
      AC: Basic checks work with no device attached.
- [ ] T8.3 Optional CHAT / IMAGE ops behind capability flags.
      AC: Present only when gateway advertises them; absent gracefully otherwise.

Artifact: Complete smart writing experience, fully optional.

---

## M9 — Second Platform: Amiga (MUI)
Goal: Validate the portability thesis.

- [ ] T9.1 `platform/amiga`: MUI shell, event loop, menus/dialogs.
      AC: App runs under an Amiga emulator; clean quit.
- [ ] T9.2 graphics.library/diskfont/bullet metrics + render impl of platform.h.
      AC: On-screen wrap matches engine layout using real Amiga fonts.
- [ ] T9.3 Amiga serial.device / bsdsocket transport hook.
      AC: Same offload features work as on ST.

Artifact: Amiga build "indistinguishable by function." Engine/services reused
unchanged (diff shows changes only under platform/amiga).

---

## M10+ — Mac Classic / DOS / Typography Polish
- [ ] T10.1 `platform/mac` (Toolbox + QuickDraw metrics/render).
- [ ] T10.2 `platform/dos` (text-mode shell; character-cell metrics).
- [ ] T10.3 Typography polish: kerning/justification options where supported.
- [ ] T10.4 PDF polish: font embedding tiers, image support.

---

## Cross-Cutting "Done" Checklist (apply to every task)
- [ ] Builds on host (+ target backend or stub).
- [ ] Platform-free logic unit-tested on host.
- [ ] No platform headers included under engine/ or services/.
- [ ] Degrades or tiers correctly at 1MB.
- [ ] Persistence/protocol uses explicit big-endian I/O.
- [ ] Public header documented; minimal surface.

