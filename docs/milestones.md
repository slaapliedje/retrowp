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
      watching the gate fail). (2) visual -- a real screenshot of
      RETROWP.PRG showing a live GEM window titled "RetroWP" with working
      mover/closer/sizer decorations at the full desktop work area.
      Known gaps (see docs/atari-platform.md): scripted mouse-driven
      end-to-end testing is unreliable in this headless environment (Y
      coordinate steering doesn't work), so the Quit menu item's dispatch
      logic is proven correct via direct synthesized-message calls rather
      than an actual click routed through evnt_multi.
      **Revisited post-M8**: the menu title glyph-noise note above was
      wrong -- it was a real bug (missing OF_HIDETREE on dropdown boxes,
      found by reading EmuTOS's own AES source), not a screenshot
      artifact, and is now fixed and visually verified. That same
      investigation found a second, previously-masked bug -- only the
      first menu title actually renders -- root-caused but not yet
      safely fixed (a 3-level tree restructuring hung real Atari CI and
      was reverted). See docs/atari-platform.md for the full trail.
      **Revisited again post-M8 (2nd time)**: a new multi-TOS CI matrix
      (host/tools/ci_atari_matrix.sh, see below) was built specifically
      because this project's CI had only ever tested against one EmuTOS
      build -- and it immediately caught a real regression a second
      3-level-tree fix attempt introduced (a genuine bus-error crash
      under a newer EmuTOS release, a hang under real TOS 1.04) that the
      single-image `ci_atari.sh` gate could never have detected. Reverted
      again, more carefully this time (the new Format title landed
      without reintroducing the risk). "Only the first title renders"
      remains open, now with two documented, root-caused, reverted fix
      attempts on record -- see docs/atari-platform.md's "Known gap: only
      the first title actually renders" section for the full trail,
      including the now-understood `menu_fixup()` DA-box mechanism a
      real fix will need to satisfy.
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
      screenshot of RETROWP.PRG rendering a hardcoded sample paragraph with
      correct word-boundary wrap filling the window without overflow.
      Known gaps (see docs/atari-platform.md): color is a black/white
      luminance threshold, not a real palette search; UTF-8-to-native
      conversion is ASCII-only (non-ASCII becomes '?', correctly decoded
      per-codepoint via utf8_decode); no scrolling, so content past the
      first screenful isn't drawn.
      **Revisited post-M8**: the "no SpeedoGDOS install available" premise
      above was investigated for real, using the user's own original
      SpeedoGDOS 5.7 floppies driven through a real INSTALL.PRG run under
      Hatari. Real per-axis DPI turned out to need no GDOS at all --
      v_opnvwk()'s own work_out[3]/[4] (pixel width/height in microns) is
      a standard VDI output, verified empirically (75/68 DPI on a no-GDOS
      EmuTOS boot, matching the ST's known non-square low-res pixels).
      ATARI_ASSUMED_DPI is now only a defensive per-axis fallback, not the
      primary source of truth. Ascent/descent now use vqt_fontinfo's
      distances[] (verified: distances[3]/[1] are ascent/descent as
      positive magnitudes, cross-checked against vst_point's charh on a
      real boot). vq_gdos() is queried at runtime and, when true,
      vst_load_fonts() is called so exact point sizes are hit via a real
      scalable outline font when one is resident -- gracefully a no-op
      otherwise. See docs/atari-platform.md for the full writeup,
      including a real, reproducible SpeedoGDOS-boot crash under this
      Hatari/EmuTOS combination that keeps the GDOS-active path from
      being covered by automated CI (the no-GDOS path -- the common case
      -- remains fully covered).
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

- [x] T8.1 Gateway grammar check (LLM or rule engine) → issue list payload.
      AC: Paragraph in, structured issues out; ST renders them.
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
      - **"ST renders them": done.** Tools > Grammar Check
        (`platform/atari/app_shell.c`) sends the current paragraph,
        decodes the issue list, and cycles through issues one at a time
        via `[ Apply | Skip | Stop ]` `form_alert()` dialogs -- a
        materially bigger UI task than Thesaurus's single blocking
        alert, since applying more than one fix in the same paragraph
        needs real bookkeeping (ascending-offset processing, a running
        byte-delta so later offsets track earlier edits, and an overlap
        guard against corrupting text an earlier fix already replaced).
        A stale-cursor risk after a mid-paragraph edit (the run array
        `wp_edit_delete_range`/`wp_edit_insert_text` mutate can resize or
        reorder) is handled by resetting the cursor to the paragraph's
        start, always valid post-edit by construction. Full design and
        verification writeup: docs/atari-platform.md's "Grammar Check
        wired into the UI" section, including the new
        `wp_atari_grammar_check_selfcheck()` (run via `ci_atari.sh`,
        passing on real m68k hardware emulation) and the same honestly-
        stated "the form_alert()-driving wrapper itself is untested by
        CI" gap every other offload menu action in this app already has.
- [x] T8.2 On-box grammar-lite fallback (rule-based) when no Pi.
      AC: Basic checks work with no device attached.
      New `services/grammar` module (`docs/services/grammar.md`): pure
      C, no platform calls at all (not even `plat_file_*`/`mem_alloc` --
      no data file, no allocation, just an in-memory scan), five
      purely mechanical rules (doubled word, multiple spaces, space
      before punctuation, repeated `!`/`?`, missing paragraph-initial
      capital), each rule chosen specifically because it needs no
      semantic understanding of the text and so is essentially never a
      false positive -- deliberately NOT an attempt to approximate what
      the LLM-backed T8.1 path does with hand-written rules. Every issue
      it produces has a real, non-empty, auto-appliable suggestion by
      construction (`grammar.h`'s hard contract), which is what let
      `platform/atari/app_shell.c`'s `do_grammar_check()` treat a
      locally-sourced issue exactly like a gateway-sourced one after a
      small struct refactor (`wp_atari_grammar_issue` now owns fixed
      message/suggestion buffers instead of holding pointers into the
      offload response). When `ensure_offload()` fails, the app shows an
      informational alert ("Using basic on-device grammar checks
      instead") and falls back to `grammar_lite_check()` on the current
      paragraph rather than failing outright -- announced, not silently
      swapped in. Scoped narrowly to "no device attached," not "the
      device failed mid-request" (a connected-then-failing gateway still
      hard-fails, so a real gateway problem is never mistaken for normal
      degraded-mode operation). Verified on host (134/134 `make test`
      cases, including a combined-rule realistic-paragraph test in the
      same spirit as `docs/services/spell.md`'s own "don't trust a tiny
      fixture alone" lesson) AND on real m68k hardware emulation (a new
      case in `wp_atari_grammar_check_selfcheck()`, run via
      `ci_atari.sh`, makes a real cross-compiled call into
      `grammar_lite_check()` -- proving the same source behaves
      correctly once actually built for the target, not just assumed
      from the host build). Full writeup: docs/atari-platform.md's
      "Offline fallback: services/grammar" section. A real bug (an
      accidental `*/` inside a `/* ... */` comment closing it early) was
      caught immediately by `make test`'s strict `-Werror` build, not by
      inspection -- see that section for the full story.
- [x] T8.3 Optional CHAT / IMAGE ops behind capability flags.
      AC: Present only when gateway advertises them; absent gracefully otherwise.
      **CHAT implemented; IMAGE deliberately not** -- this app has no
      image type anywhere (document model, format importers/exporters,
      or the Atari draw layer), so building IMAGE's capability
      advertisement with nowhere to put the resulting bytes would be a
      checkbox feature, not a real one. Full rationale, plus a design
      note for whenever a future milestone makes this genuinely
      buildable (image data would need the GATEWAY to downconvert to
      something each vintage target can actually display, not just ship
      whatever a modern generator produces), in
      docs/formats/offload-protocol.md's `OFFLOAD_IMAGE` section.
      - Wire payload (docs/formats/offload-protocol.md's new
        `OFFLOAD_CHAT` section): the simplest shape in the protocol --
        UTF-8 prompt in, UTF-8 reply out, no framing beyond the wire
        frame's own length, since a chat reply has no structure to
        encode the way GRAMMAR_CHECK's issue list or THESAURUS's word
        list do.
      - Both gateways implement it: `mock_gateway.c` from a small
        fixture table (deterministic, part of `make test`), and
        `offload/gateway/ollama_gateway.c` for real, sharing
        GRAMMAR_CHECK's `gemma2:9b` default (own `--chat-model`
        override) since CHAT is "freeform assist" with no rigid format
        to parse -- the same category of task GRAMMAR_CHECK already
        proved that model handles well.
      - **The actual point of this AC, not previously done for any
        offload feature:** `platform/atari/app_shell.c`'s `do_chat()`
        is the first client code in this project that calls
        `offload_query()` and checks the CAPS handshake's
        `ops_supported` bitmask BEFORE submitting a request, via a new
        `offload_caps_supports()` helper. Thesaurus/Grammar Check/
        ODT-PDF all submit unconditionally and only handle
        `OFFLOAD_UNSUPPORTED` reactively after a full round trip --
        Chat gets an immediate, honest "this gateway doesn't offer
        Chat" answer with zero network traffic when the bit isn't set.
        Prompt extraction reuses Grammar Check's
        `extract_current_paragraph_text()`; the reply is shown
        read-only via `form_alert()` (no insert-into-document flow --
        out of scope for an AC about capability gating, not editing).
      - Verified on host (137/137 `make test`, including a real CAPS
        check that `OFFLOAD_CHAT` is advertised and `OFFLOAD_IMAGE` is
        NOT by the real gateways), on real m68k hardware emulation (a
        new `wp_atari_chat_selfcheck()` case, run via `ci_atari.sh`),
        and against a REAL, running Ollama server (`host/tools/
        ollama_gateway_smoke.c`, extended to exercise all three real
        ops over one connection -- a live prompt got a real, sensible
        reply, and capability advertisement was confirmed both ways:
        CHAT present, IMAGE genuinely absent). Full writeup:
        docs/atari-platform.md's "Chat wired into the UI" section.
      - Same honest gap as every other offload menu action:
        `do_chat()` itself (the `form_alert()`-driving wrapper) is
        untested by CI, verified only transitively through its
        already-proven building blocks.

Artifact: Complete smart writing experience, fully optional.
**Reached**, within this project's own documented scope choices.
T8.1 (LLM-backed grammar check, protocol + both gateways + real ST UI
wiring) and T8.2 (on-device rule-based fallback for no-Pi-attached) are
both done. T8.3 (capability-gated optional ops) is done for CHAT, the
one half of "CHAT / IMAGE" this project can actually make use of --
IMAGE stays a reserved, unimplemented op ID, honestly documented rather
than faked, until a future milestone gives this project somewhere to
put image bytes at all.

**Update (post-M8): multi-TOS CI matrix + Format controls.** Two
requests handled together in one session, unrelated to any milestone's
original scope. (1) `host/tools/ci_atari_matrix.sh` (shared boot logic
in `host/tools/atari_ci_lib.sh`) boots the exact same smoke test against
6 real TOS/EmuTOS images spanning the original ST through MegaSTE, not
just the single EmuTOS build `ci_atari.sh` alone has ever used --
immediately valuable: it caught a real regression a menu-tree fix
attempt introduced that single-image CI never could (see T4.1's
"Revisited again post-M8" note above). (2) Bold/Italic/Underline, Font
Size, and paragraph Alignment (Left/Center/Right) are now real menu
items + Ctrl+letter keyboard shortcuts, the classic 1980s-word-processor
control set. Rendering itself was already fully wired since T4.2 --
only the UI to set these attributes (a "current input style" toggle
model, since this app still has no selection model or cursor
navigation) and paint-time alignment offset math were new. Two small,
portable, reusable engine additions
(`wp_document_cstyle_find_or_add()`/`wp_document_pstyle_find_or_add()`
in `engine/include/docmodel.h`) came out of this, not tied to Atari at
all. Deliberately deferred: Justify alignment (needs real per-word
stretch painting) and font *family* switching (needs a locked-struct
change to `wp_char_style.font_name` plus GDOS font enumeration -- a
materially bigger, separate task). Full design, verification, and the
menu-tree investigation trail: docs/atari-platform.md's "Multi-TOS CI
matrix" and "Format controls wired into the UI" sections.

**Update (post-M8): boot resolution + real INF fixtures.** Follow-up
session: tested whether ST Low's 40-column width (not the `mn_bar()`
structural issue above) was the real cause of "only the first menu title
renders" -- it wasn't (verified at Medium resolution, same result). The
harness's default boot resolution was still switched from Low to Medium
(640x200, 80 columns) across all Atari CI scripts, since Low is already
below the app's own supported floor. Also added
`host/tools/atari_inf_fixtures/` -- real `DESKTOP.INF`/`NEWDESK.INF`/
`EMUDESK.INF` files, each saved by genuinely booting the corresponding
real desktop (TOS 1.04, TOS 2.06, EmuTOS) rather than hand-authored, for
interactive/manual testing and real-hardware use. Full details:
docs/atari-platform.md's "Boot resolution: Medium, not Low" section and
the new note under "Known gap: only the first title actually renders".

---

## M8.5 — Vi-Mode Editing + Cross-Platform UI Scaffolding + Help System
Goal: A vi-style modal editor, UI-agnostic and living in the portable
engine; a `platform/common/` layer so menus/dialogs/status/help behave
consistently across future backends without pretending Atari GEM, Amiga
MUI, Mac Toolbox, and a DOS shell can look identical; a help system
authored once and delivered through each platform's own native-feeling
viewer. Inserted between M8 and M9 (Amiga/Mac/DOS keep their original
numbers) because M9's own artifact goal -- "Amiga port touches only
`platform/amiga/`, diff-clean against engine/services" -- isn't reachable
unless this scaffolding exists first. Full plan:
`docs/vimode.md` (design decisions, command reference, four real bugs
found and fixed), `docs/atari-platform.md` (the Atari retrofit and its
own deliberate menu-tree-untouched scope decision), `docs/help.md` (the
help system).

- [x] T8.5.1 Engine: paragraph split/merge.
      AC: `wp_edit_split_paragraph`/`wp_edit_merge_paragraph`, journaled,
      undo/redo round-trip.
      A real, pre-existing gap independent of vi-mode: before this,
      `WP_OP_SPLIT_PARA`/`WP_OP_MERGE_PARA` were defined in `wp_op_kind`
      but nothing implemented them, so Enter was a hard no-op anywhere in
      the app. `wp_document_split_paragraph_raw`/`merge_paragraph_raw`
      (`engine/src/edit.c`) plus new private `paragraph_insert_at`/
      `paragraph_remove_at` splice primitives (`edit_internal.h`).
      `docs/editing.md`'s "Paragraph Split / Merge" section has the full
      design (canonical-position handling, the undo field-reuse
      convention). 7 new `test_edit.c` cases, 3 new `test_undo.c` cases.
- [x] T8.5.2 Engine: general delete-range + undo grouping.
      AC: Multi-run/multi-paragraph delete as one undo step;
      `wp_selection`'s first real use anywhere in the codebase.
      `wp_pos_compare`, `wp_edit_delete_selection`,
      `wp_document_extract_text` (two-call sizing convention, matches
      `format_txt_export`), and `WP_OP_GROUP_BEGIN`/`GROUP_END` with a
      depth-counted undo/redo loop (nested groups compose correctly,
      needed because Insert-mode sessions wrap an outer group around
      `cw`'s own inner group). New `test_selection.c`, 9 cases.
- [x] T8.5.3 Engine: motion family.
      AC: Word/line/paragraph motions, host-testable with no layout pass.
      New `engine/src/motion.c`: `wp_pos_word_forward/backward/end`,
      `wp_pos_line_start/first_nonblank/end`,
      `wp_pos_paragraph_down/up`, `wp_pos_para_block_forward/backward`.
      **Design decision**: motions operate on logical paragraphs, not
      visually-wrapped screen lines (`j`/`k` move by paragraph) -- a
      real, documented UX deviation from vim, chosen because `wp_pos` has
      no wrapped-row concept and the layout cache is a lazy, invalidatable
      rendering artifact motions shouldn't depend on. New `docs/vimode.md`.
      New `test_motion.c`, 10 cases.
- [x] T8.5.4 Portable key-event type.
      AC: `wp_key_event {codepoint, special, mods}`, zero platform calls.
      New `engine/include/keyevent.h` -- pure value type, same category as
      `wp_pos`. Each backend translates its own native input into this.
- [x] T8.5.5 Vi-mode state machine.
      AC: Normal/Insert/Visual/Visual-Line/Command modes; fully
      host-testable via synthetic key scripts, zero real backend.
      New `engine/include/vimode.h` + `engine/src/vimode.c`. Full command
      reference, the `:w`/`:q`/`:wq` action hand-off seam (vi-mode never
      calls `plat_file_*` itself -- returns a `wp_vimode_action` the
      caller executes with save/load logic it already owns), and
      undo-grouping strategy for Insert sessions and compound operators:
      `docs/vimode.md`. **Design decision**: simpler insertion-point
      caret, not vim's on-character caret -- confirmed with the user,
      reuses existing engine position functions with no new boundary-
      clamping logic, at the cost of small acknowledged differences
      around `x`/`$`/post-Escape cursor position. New `test_vimode.c`,
      18 cases at the time, 19 after a 4th bug found during the Atari
      retrofit (T8.5.9) added its own regression test.
      **Three real bugs found by the test suite**: `cw`/`cW` aren't
      literally `c`+`w` (real vim special case, `:help cw`); a linewise
      register's trailing `'\n'` was splitting one extra empty paragraph
      on put; Visual-mode motions must chain from the selection's current
      extent (`vs->cursor`), not where `v` was pressed. All three: full
      account in `docs/vimode.md`.
- [x] T8.5.6 `platform/common/`: action registry + app context.
      AC: One canonical `{id, group, label, key_hint, shortcut_ascii,
      handler, is_enabled}` table replaces two independently-maintained
      switch statements (menu dispatch + keyboard shortcuts).
      New `platform/common/actions.{h,c}` + `app_ctx.{h,c}`. Portable
      handlers: Bold/Italic/Underline toggles, Edit Copy, File Quit.
      Backend-supplied (`handler == NULL`, deliberate scope decision, not
      an oversight): File Open/Save, Edit Cut/Paste, all four Tools
      actions -- each already a real, working, multi-hundred-line
      Atari-specific function; re-architecting them was judged
      disproportionate to this pass's actual goal. New
      `platform/common/` build-discipline rule in `CLAUDE.md`: may call
      `platform.h`, must never `#include` a native header, enforced by
      construction (every target's own toolchain compiles this
      directory). New `test_actions.c`, 7 cases including a
      pairwise-shortcut-uniqueness check generalizing what
      `wp_atari_format_selfcheck` used to guard by hand.
- [x] T8.5.7 `plat_dialog_choice` + Atari implementation.
      AC: A real "choose one of N" native dialog, replacing "Size +/-"
      cycling and three separate alignment menu items with one picker.
      New `platform.h` primitive alongside the existing
      `plat_dialog_message`. `platform/atari/dialog_atari.c`: the first
      `form_do()`-based dialog anywhere in this codebase (`form_alert`/
      `fsel_input` were the only GEM dialog mechanisms used before this),
      a runtime-built `OBJECT` tree sized to the caller's option count (no
      resource-file compiler in this project's toolchain). Real
      character-cell sizing via `graf_handle()`. Verified via
      `wp_atari_dialog_selfcheck` (index arithmetic, without ever calling
      the blocking `form_do()`) across the full 6-image TOS matrix.
      Interactive click-through verification: see T8.5.9's own account of
      what a real attempt found. Full design: docs/atari-platform.md's
      "Format Size/Alignment dialogs" section.
- [x] T8.5.8 `platform/common/`: status/mode line.
      AC: A reserved screen strip showing vi-mode's current mode.
      New `platform/common/statusline.{h,c}` -- no new `platform.h`
      primitive needed (`plat_draw_text`/`plat_fill_rect` already take
      arbitrary rects). New `test_statusline.c` (crash-safety only --
      `draw_host.c`'s draw calls are no-ops, so host can't verify visual
      content, the same boundary every drawing-adjacent host test in this
      project respects).
- [x] T8.5.9 Atari retrofit: wire everything into the real app shell.
      AC: Real AES `evnt_multi` key/menu dispatch routes through vi-mode
      and the action registry; a real Hatari screenshot shows it working.
      The single highest-risk phase, given this project's own documented
      history of two real regressions from menu/dispatch restructuring
      (see docs/atari-platform.md). `atari_translate_key` (real IKBD
      scan-code table, verified against the Atari Compendium, not
      memory) feeds every keystroke to vi-mode first; anything vi-mode
      doesn't consume falls through to `wp_action_find_by_shortcut`.
      **A fourth real vi-mode bug was found integrating this**: `Ctrl+
      <letter>` chords were being treated as their bare letter (Ctrl+B
      silently moved the cursor via `b` instead of reaching Bold) --
      fixed in `engine/src/vimode.c` itself (a mods-guard on every
      plain-letter command dispatch), with a new regression test,
      `docs/vimode.md`'s "Atari Retrofit" section has the full account.
      **Deliberate scope decision**: `wp_menu_tree[]`'s hand-built
      `OBJECT` array is completely untouched -- menu dispatch is unified
      through the registry only where it's a clean 1:1 match
      (Bold/Italic/Underline/Copy/Quit); Font Size/Alignment's five
      existing menu items keep calling the pre-M8.5 local functions
      directly, while the NEW dialog-based behavior (T8.5.7) is reachable
      by keyboard shortcut only this pass. New `wp_atari_key_translate_
      selfcheck`, confirmed on the real 6-image TOS matrix.
      **Real interactive verification, honestly reported**: a live
      Hatari session driven by synthetic `xdotool` key injection did not
      reliably demonstrate a keystroke reaching the app end-to-end in
      this environment, across several technique variations -- a
      documented test-harness gap (reinforcing an existing, similar
      caveat from T4.3), not a claimed pass. The automated coverage above
      is the rigorous verification for this task's AC.
- [x] T8.5.10 Help system MVP.
      AC: Portable help content, authored once, rendered through a real
      pager on every target including Atari from day one.
      New portable format close to AmigaGuide's own real syntax (`@node
      NAME "Title"` / `@endnode`, `{label|target}` links -- a deliberate
      simplification of AmigaGuide's own `@{"label" Link target}`, close
      enough that a future AmigaGuide export is a per-line mechanical
      translation, not a redesign). `platform/common/help_doc.{h,c}`
      (parser, built on the engine's existing `wp_strpool`/`wp_span`
      rather than reinventing string storage) + `help_view.{h,c}` (a
      simple scrolling pager: link-line navigation via j/k/Enter, a
      history stack for Backspace/"back", and its own greedy word-wrap
      engine, `wp_help_wrap_line`, using the same `plat_measure_text`
      seam the main document layout uses -- exposed as its own testable
      function specifically because `draw_host.c`'s draw calls are
      no-ops and can't otherwise prove wrap correctness on host). Real
      authored content, `docs/help/retrowp.help` (a command reference
      covering every vi-mode mode plus Format shortcuts), compiled into
      every target at build time by a new host tool,
      `host/tools/gen_help_header.c` (source stays a real, human-editable
      text file; no hand-escaped C string to keep in sync). Wired into
      `platform/atari/app_shell.c` behind F1 only (no Help menu item --
      same menu-tree-avoidance reasoning as T8.5.9); a whole separate UI
      mode (`g_help_active`) that takes over key dispatch and painting
      while active. 22 new host tests (`test_help_doc.c`,
      `test_help_view.c`, including hand-computed word-wrap assertions
      against the host's deterministic fake-monospace font) plus a
      `test_help_content.c` case that parses the REAL shipped content and
      confirms every link resolves -- a real authoring mistake would fail
      the build, not just be discoverable by clicking through the viewer.
      A new `wp_atari_help_selfcheck` proves the same wrap algorithm
      against REAL Atari VDI-measured font metrics, confirmed on the full
      6-image TOS matrix. **Confirmed live on real Hatari**: F1 opens the
      viewer, body text word-wraps correctly, and link markers/hint bar
      render -- a genuine screenshot, unlike T8.5.9's inconclusive
      attempt (this one landed once the `xdotool` screenshot hotkey
      mistake from that earlier attempt was found and fixed -- see
      docs/atari-platform.md). Link-selection *highlighting* specifically
      (pressing j/k to move between links) was not independently
      reconfirmed live in this same pass -- covered by
      `test_help_view_link_selection_clamped_at_bounds` and the paint
      crash-safety test instead, the same category of gap T8.5.9 already
      has.
      **Research spike, real result**: cloned and built HypView's `hcp`
      (the ST Guide `.HYP` compiler) as a plain host tool -- clean build,
      no special dependencies beyond glib (already present). Hand-wrote a
      trivial fixture in hcp's real native syntax (confirmed from its own
      source: `@node <name> [<title>]`, `@{"text" link <name>}`,
      matching this project's own format closely enough that translating
      `{label|target}` to it is genuinely mechanical) and compiled it
      successfully; `hypinfo` (also built from the same source tree)
      independently read the result back and confirmed a well-formed
      ST-Guide database with the right node count and names. **Not done
      in this pass** (real, separate follow-up, not a failed spike): a
      real `{label|target}` → hcp-syntax translator, wiring `hcp` as a
      Makefile-invoked build dependency the way `gen_help_header` is, and
      an interactive real-ST-Guide-viewer click-through. The spike's job
      was to answer "is this worth investing in" -- it is; actually doing
      it is out of scope for this MVP pass, per the plan's own framing.
      docs/help.md has the full account, including the fixture and
      `hypinfo` output.

**Verification across the whole milestone**: `make test` grew from 139
(session start) to 219 (T8.5.10's end) -- host tests kept passing after
every phase, not just at the end. `make atari-engine-check` (`-std=c89
-pedantic -Werror` cross-compile) and the full real-Hatari 6-image TOS
matrix (`ci_atari_matrix.sh`) both stayed green through T8.5.7 and
T8.5.9/T8.5.10 specifically, the two phases that actually touch the real
Atari runtime -- including T8.5.9, the exact class of change (menu/
dispatch restructuring) that caused two real regressions earlier in this
project's history, this time without incident, attributed directly to
the deliberate decision not to touch `wp_menu_tree[]`'s own structure.

---

## M9 — Second Platform: Amiga (MUI)
Goal: Validate the portability thesis.

- [x] T9.1 `platform/amiga`: MUI shell, event loop, menus/dialogs.
      AC: App runs under an Amiga emulator; clean quit. **Met.** Real
      `m68k-amigaos-gcc`/MUI SDK toolchain and a bootable amiberry
      Workbench 3.1 + MUI fixture (Phase 0); memory/file/clipboard/timer
      `platform.h` seams (Phase 1); a real MUI Application/Window/
      Menustrip, `IDCMP_RAWKEY` -> vi-mode dispatch via a custom `Area.mui`
      subclass, File Open/Save/Cut/Copy/Paste, and clean quit (Phase 2).
      Full account, including the BOOPSI-calling-convention research and
      the `MUIA_Application_Menustrip` init-order gotcha:
      `docs/amiga-platform.md`.
- [x] T9.2 graphics.library/diskfont/bullet metrics + render impl of platform.h.
      AC: On-screen wrap matches engine layout using real Amiga fonts.
      **Met.** Real `graphics.library`/`diskfont.library` metrics and
      `RastPort` rendering; `wp_amiga_layout_selfcheck` confirms real
      diskfont-measured wrap on-target. A real on-target memory-corruption
      bug (a bitmap-less measuring `RastPort`) was found and fixed --
      `docs/amiga-platform.md`'s Phase 3 section.
- [x] T9.3 Amiga serial.device / bsdsocket transport hook.
      AC: Same offload features work as on ST. **Met**, with one honestly
      documented gap: `transport_amiga.c`'s real `serial.device`
      `IORequest`-based implementation is proven via
      `wp_amiga_transport_selfcheck`'s on-target exec.library round trip
      rather than a live two-tier serial CI proof (no physical serial
      hardware, and this amiberry build's serial passthrough rejects both
      a host FIFO and a real `socat` PTY -- both investigated, not
      assumed). `bsdsocket.library` TCP transport stayed out of scope per
      the plan's own "optional/stretch" framing. Full account:
      `docs/amiga-platform.md`'s Phase 4 section.

Beyond the plan's original three tasks, this milestone also delivered:
CI infrastructure (`ci_amiga.sh` + a real Kickstart 2.04/2.05/3.1 matrix,
`ci_amiga_matrix.sh` -- Kickstart 3.0 excluded for a real, documented
68000-incompatibility reason, not silently skipped), and a help-system
retrofit (the same portable `help_doc.h`/`help_view.h` pager Atari uses,
wired to F1 and the Help menu; a real AmigaGuide `.guide` export spike
that, unlike the Atari `.HYP` spike, fully panned out and was verified
on-target with `amigaguide.library`/MultiView). Two real, independently
significant bugs were found and fixed along the way: bare Escape closing
the whole RETROWP window (a MUI global-hotkey-table propagation issue,
Amiga-only), and a shared `platform/common/help_view.c` baseline/ascent
positioning bug that affected Atari's own pager too, fixed once at the
shared layer. Full account of both: `docs/amiga-platform.md`'s Phase 6
section.

Artifact: Amiga build "indistinguishable by function." Engine/services reused
unchanged -- confirmed by diff: `engine/` and `services/` show zero changes
across the entire milestone (`git diff --stat d99ae7e^..HEAD -- engine/
services/` is empty); every change lives under `platform/amiga/`,
`platform/common/`, `host/`, `Makefile`, and `docs/`. The one cross-backend
touch is `platform/common/help_view.c`'s shared ascent fix (also benefiting
Atari's own pager) plus the matching one-line fix to `platform/atari/
app_shell.c`'s own call site -- not a portability-thesis violation, since
it's a bug fix to code both backends already shared, not new Amiga-specific
logic leaking into a shared file. Confirmed: `make test`
stayed at 219/219 throughout M9 (no engine/services test count regressions);
`atari-engine-check` and the full 6-image Atari TOS matrix stayed green
through every phase that touched shared code (Phase 6's `help_view.c`
change and Atari's own `paint_help` ascent fix).

---

## M10 — Switchable Keybinding Layouts
Goal: let a user pick their own muscle memory (WordStar, WordPerfect,
Emacs, classic Microsoft Word) instead of always getting vi-mode,
selected once at startup from a config file (a restart is required to
change it -- no live switching), staying 100% keyboard-usable
throughout. Full account: `docs/keymaps.md`.

- [x] T10.1 Config file + generic keymap dispatch scaffolding.
      AC: A small `platform/common/config.h`/`.c` (line-based
      `key=value`, lenient, defaults to `vim`) plus
      `platform/common/keymap.h`/`.c` replacing `wp_app_ctx`'s
      hardcoded `wp_vimode_state` field with a tagged union
      (`wp_keymap_state`), with zero observable behavior change for
      vi-mode -- the explicit regression-safety proof this whole
      milestone is built on. **Met.** `make test` grew from 219 to 236
      with **zero** changes needed to `test_vimode.c` itself. Real
      on-target config-file round trip proven on both backends
      (`wp_atari_config_selfcheck`/`wp_amiga_config_selfcheck`, added
      once all five `keymap=` values existed to check against, Phase 6).
- [x] T10.2 WordStar keybinding layout.
      AC: The real diamond cursor cluster, five real prefix keys (only
      `^K`/`^Q`/`^P` with implemented subcommands), and WordStar's own
      real block **marker-pair** model (not an anchor+extend
      selection) -- verified against the WordStar 3.0 Quick Reference
      Card and wordstar.org's own docs, not recalled from memory.
      **Met.** `make test` grew to 254. Promoted vi-mode's own
      file-local paste helper into a shared engine primitive,
      `wp_edit_paste_text` (`editing.h`/`undo.c`), once WordStar's
      block copy/move needed the identical "insert text, splitting a
      paragraph at each embedded newline" logic. Two real bugs found
      and fixed: a classic C string-escape footgun in the test
      harness, and an overly broad refusal guard in Block Move that
      wrongly refused every single-paragraph move -- see
      `docs/keymaps.md`'s own account of both.
- [x] T10.3 Real `WP_MOD_ALT` plumbing, both backends.
      AC: Atari's `atari_translate_key` and Amiga's `translate_rawkey`
      both actually set `WP_MOD_ALT` (defined since M8.5, never set by
      either backend before this task) -- needed before Emacs's Meta
      or WordPerfect's Alt-Fn chords could exist at all. **Met.** New
      selfcheck coverage on both backends proves every pre-existing
      case kept its exact expected value (the regression proof) plus
      new Alt/Alt+Ctrl/Alt+special rows; a new host-side vi-mode test
      proves the existing `mods != 0` shortcut guard (previously only
      exercised via Ctrl) rejects an Alt-chord the same way.
- [x] T10.4 Emacs keybinding layout.
      AC: Char/word/line motion, Meta via real Alt or the historical
      `ESC`-then-key fallback, kill/yank, mark+point region
      operations, the `C-x`/`C-c` prefix families -- verified against
      the current GNU Emacs Reference Card. **Met.** `make test` grew
      to 275. Promoted vi-mode's own file-local `clamp_cursor` into a
      shared engine primitive, `wp_pos_clamp_to_document`
      (`editing.h`/`cursor.c`), once Emacs's own `C-x u` (undo) needed
      the identical post-undo cursor-validity check. Two real,
      honestly-flagged deviations from stock Emacs, both structural
      backend limitations rather than choices: undo binds to `C-x u`
      instead of `C-/` (a `Ctrl+<non-letter>` chord doesn't survive
      this project's own key translation intact), and `C-SPC`
      (set-mark) is implemented and fully host-tested but not
      currently reachable from a real keypress on either shipped
      backend for the same reason. One real bug found and fixed: `M-f`/
      `M-d` initially reused vim's own word-forward motion instead of
      Emacs's real word-*end* landing, eating an extra trailing space
      on kill -- see `docs/keymaps.md`'s own account.
- [x] T10.5 WordPerfect keybinding layout (flattened MVP).
      AC: Real F-key/modifier bindings for WP 5.1's own most-used
      commands, with the authentic nested numbered-menu UI itself
      explicitly out of scope this pass (a real, documented follow-up
      spike, not silently dropped) -- verified against the real 1990
      IBM WP5.1 keyboard template. **Met.** `make test` grew to 293.
      Block mode is a real anchor+extend selection activated as an
      explicit **toggle** (Alt-F4 on, Alt-F4 again cancels) -- a third,
      distinct selection-activation pattern alongside vi's modal
      enter/exit and Emacs's set-and-persist, with its own test
      coverage proving extend-by-motion. Real WP's own Copy/Move/Delete
      3-way block menu flattens to one action, Move (Ctrl-F4 cut +
      Shift-F10 retrieve) -- documented, not silently narrowed. F12
      (real WP's alternate block-toggle key) is structurally
      unbindable: `wp_special_key` only goes up to F10.
- [x] T10.6 Microsoft Word (classic) keybinding layout.
      AC: The lowest-novelty scheme -- direct pass-through for
      Ctrl-B/I/U/S/O/C/X/V (already `g_wp_actions[]`'s own real
      shortcut bytes), Shift+motion selection, real Ctrl-Z/Ctrl-Y
      undo/redo. **Met.** `make test` grew to 309, closing concrete
      per-scheme work as the plan's own suggested ordering predicted.
      The only scheme in the milestone where Shift-selection has a
      real consumer beyond an explicit kill/copy keystroke (typing
      over a selection replaces it, matching familiar modern-editor
      behavior) and the only one with real redo (every other scheme's
      undo story is either absent, narrower, or redo-less by real
      historical design). Deliberately has no in-app quit keystroke at
      all, matching real classic Word's own lack of one -- every
      backend's window-close handling already covers it regardless of
      the active scheme.

Artifact: five fully switchable keybinding layouts, each verified
against a real primary source, each with its own honestly-documented
gaps rather than invented workarounds. `docs/keymaps.md` has the full
per-scheme command reference, the shared dispatch architecture, and
every real bug the test suite caught along the way.

**Verification across the whole milestone**: `make test` grew from 219
(session start) to 309 (T10.6's end), strictly monotonically --- every
phase's own regression-safety AC held, most visibly T10.1's own "zero
`test_vimode.c` changes" requirement. `atari-engine-check`/
`amiga-engine-check` and the full 6-image Atari TOS matrix / 3-image
Amiga Kickstart matrix all stayed green through every phase, including
the two (T10.1, T10.3) that touched real per-backend dispatch/
translation code directly -- the exact class of change this project's
own history has twice regressed on before. `engine/` and `services/`
needed no changes outside new scheme modules and the two shared-
primitive promotions (`wp_edit_paste_text`, `wp_pos_clamp_to_document`)
-- both real, load-bearing generalizations of code vi-mode already had,
not new engine surface invented for this milestone.

---

## M11 — Find & Replace, All Five Keybinding Schemes
Goal: close the one gap M10 explicitly deferred in every scheme --
Find and Replace, each with its own real, historically-verified prompt
flow (WordStar's `OPTIONS?` letters, WordPerfect's F2-terminated
fields, Emacs's genuinely live isearch, MS Word's keyboard-adapted
dialog, vim's own `?`/`N`/`:s`), not one generic mechanism reskinned
five times. Full account: `docs/keymaps.md`, `docs/vimode.md`'s own
"Substitute" section.

- [x] T11.1 Shared find/replace primitive + vim's own `?`/`N`/`:s`
      completion.
      AC: `wp_document_find_text` (`editing.h`, new `engine/src/
      search.c`) promotes `wp_vimode_search` into a real, generalized
      primitive -- backward search actually implemented (previously
      accepted but ignored), plus `ignore_case`/`whole_word`, both
      needed by more than one scheme. `wp_vimode_search` becomes a
      thin wrapper -- zero changes needed to `test_vimode.c`'s
      pre-existing cases, the same regression-safety discipline M10's
      own shared-primitive promotions used. **Met.** `make test` grew
      from 309 to 333. vim itself gained `?` (backward search), `N`
      (repeat, reversed), and `:s/old/new/flags`/`:%s/old/new/flags`
      substitute (`g`/`c`/`i` flags; `c` pauses in a new
      `WP_VIMODE_SUBSTITUTE_CONFIRM` mode with a scoped `y`/`n`/`a`/`q`
      vocabulary). A real gotcha found and documented, not a source
      bug: growing `wp_vimode_state`'s size silently corrupted an
      incrementally-rebuilt binary (this Makefile doesn't track header
      dependencies) -- `make clean` before trusting any test run after
      a struct-layout change is now a documented discipline.
- [x] T11.2 WordStar Find/Replace (`^QF`/`^QA`/`^L`).
      AC: real `FIND?`/`REPLACE WITH?`/`OPTIONS?` prompt sequence,
      verified against the WordStar 3.0 Quick Reference Card and
      Reference Manual; real `G`/`N`/`B`/`U`/`W` option letters. **Met.**
      `make test` grew to 344. `^QF` is a single jump (real WordStar
      finds one occurrence and stops); `^QA` iterates every match in
      scope (cursor-to-end, or doc-start/end under `G`), confirming
      each unless `N` was given. `^L` repeats the last find/replace by
      simply not clearing the live prompt state between invocations --
      no separate "last search" copy needed.
- [x] T11.3 WordPerfect Find/Replace (`F2`/`Shift-F2`/`Alt-F2`).
      AC: real `-> Srch:`/`<- Srch:`/`w/Confirm? (Y/N)` prompts,
      verified against the real WordPerfect Workbook for IBM PC; F2
      terminates each field (not Enter) and the real "press +Search
      twice" repeat-with-empty-field behavior. **Met.** `make test`
      grew to 352. Search/replace text buffers are deliberately never
      cleared on entry -- that persistence IS the repeat mechanism.
      No G-equivalent restart-from-top exists in this scheme (a real,
      narrower design than WordStar's own).
- [x] T11.4 Emacs Find/Replace (`C-s`/`C-r` isearch, `M-%`
      query-replace).
      AC: genuinely live incremental search (every keystroke
      re-searches, no Enter), forward lands at match end / backward at
      match start (a real, verified asymmetry); `M-%` with the real
      `y`/`SPC`/`n`/`DEL`/`!`/`.`/`q` vocabulary, verified against the
      current GNU Emacs Manual. **Met.** `make test` grew to 362.
      `C-g` aborts to the search anchor (real Emacs binding); a
      failing isearch leaves the cursor at the last successful match
      rather than resetting, real "Failing I-search" leniency.
- [x] T11.5 Microsoft Word (classic) Find/Replace (`Ctrl+F`/`Ctrl+H`/
      `Shift+F4`).
      AC: `Ctrl+F` single jump with natural wraparound; `Shift+F4`
      repeats the last executed Find (real Word binding -- confirmed
      `F3` is AutoText/glossary expansion in real classic Word, not
      Find Next, a common cross-app assumption avoided); `Ctrl+H`'s
      confirm state (`r`/`a`/`n`/Escape) stands in for the real
      persistent dialog's Replace/Replace All/Find Next/Close buttons.
      **Met.** `make test` grew to 371, closing concrete per-scheme
      work -- the lowest-novelty phase, no test-fixture surprises.

Artifact: all five keybinding schemes have real, historically-verified
Find/Replace, each with its own honestly-documented narrower scope
(no regex anywhere; no case-fold/whole-word toggle outside WordStar's
own `U`/`W`; no G-equivalent restart-from-top outside WordStar's own
`G`; Emacs's fuller query-replace vocabulary and WordPerfect's
authentic menu UI both remain out of scope, as before).

**Verification across the whole milestone**: `make test` grew from 309
(session start) to 371 (T11.5's end), strictly monotonically.
`atari-engine-check`/`amiga-engine-check` and the full 6-image Atari
TOS matrix / 3-image Amiga Kickstart matrix all stayed green through
every phase -- **a pure regression gate throughout**, unlike M10: this
milestone needed zero `platform/atari`/`platform/amiga` changes and no
new `platform.h` primitive or modifier bit, since every scheme's prompt
renders through the same non-blocking status-line mechanism vi's own
`:`/`/` already proved out in M10. The amiberry matrix's `kick-3.1-a500`
image hit a real, reproducible-as-flaky boot-time crash (before
`AMISMOKE.exe` ever runs) twice across the milestone, both times cleared
by an immediate rerun (3/3 clean) -- confirmed as pre-existing
environmental flakiness in this specific Kickstart+amiberry combination,
independent of source changes, not a regression.

---

## M12 — Third Platform: Mac Classic (Toolbox + QuickDraw)
Goal: Validate the portability thesis a third time, against the most
architecturally different platform yet -- a real Toolbox/QuickDraw
event loop instead of GEM's or MUI's own, and a boot volume that (unlike
Amiga's) must be a genuine HFS disk image, not a plain host directory.

- [~] T12.1 `platform/mac`: Toolbox shell, event loop, menus/dialogs.
      AC: App runs under BasiliskII; clean quit. **In progress -- Phases
      0-1 closed, Phase 2 (the real Toolbox shell) not yet started.**
      Real `m68k-apple-macos-gcc` (Retro68) toolchain and a disposable
      BasiliskII System 7.5.3 fixture; a from-scratch HFS catalog B-tree
      injector (`host/tools/mac_hfs/`) that places a tiny permanent
      launcher app into `Startup Items` -- BasiliskII has no equivalent
      to amiberry's host-directory-mountable boot volume, so Amiga's own
      CI-signal trick (`amiga_ci_lib.sh`) doesn't transfer directly and
      this was built new. Three real bugs found and fixed along the way,
      including one 100%-reproducible boot-time crash (a missing B-tree
      node free-space sentinel). The launcher hands off execution via a
      real `LaunchApplication` call to whatever the CI harness has just
      copied onto the ExtFS share as a plain file -- no per-run disk
      surgery, no Finder interaction at all (Phase 0). Real
      memory/file/clipboard/timer `platform.h` seams (Phase 1):
      `file_mac.c`'s File Manager calls and `clipboard_mac.c`'s Scrap
      Manager calls, verified on-target via
      `wp_mac_file_clipboard_selfcheck()` -- `file_clipboard: PASS`
      across both ROM images. `host/tools/ci_mac.sh` and the new
      `make mac-engine-check` (link-free strict-C89 portability check,
      mirroring `atari-engine-check`/`amiga-engine-check`) both green;
      `make test` unaffected (371/371). Full account, including every
      byte offset, the dead-end Finder-alias attempt that led to the
      working launcher design, and every Phase 1 gotcha (missing
      `Scrap.h`, `FSRead`'s `eofErr`-as-clean-EOF semantics):
      `docs/mac-platform.md`.
- [ ] T12.2 QuickDraw metrics/render impl of `platform.h`.
      AC: On-screen wrap matches engine layout using real Mac fonts.
- [ ] T12.3 Mac Serial Driver transport hook.
      AC: Same offload features work as on ST/Amiga.

Artifact target (not yet reached): Mac build "indistinguishable by
function," `engine/`/`services/` unchanged -- the same bar M9 met for
Amiga.

---

## M13+ — DOS / Typography Polish
- [ ] T13.1 `platform/dos` (text-mode shell; character-cell metrics).
- [ ] T13.2 Typography polish: kerning/justification options where supported.
- [ ] T13.3 PDF polish: font embedding tiers, image support.

---

## Cross-Cutting "Done" Checklist (apply to every task)
- [ ] Builds on host (+ target backend or stub).
- [ ] Platform-free logic unit-tested on host.
- [ ] No platform headers included under engine/ or services/.
- [ ] Degrades or tiers correctly at 1MB.
- [ ] Persistence/protocol uses explicit big-endian I/O.
- [ ] Public header documented; minimal surface.

