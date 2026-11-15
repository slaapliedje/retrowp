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

- [x] T12.1 `platform/mac`: Toolbox shell, event loop, menus/dialogs.
      AC: App runs under BasiliskII; clean quit. **Met.** Real
      `m68k-apple-macos-gcc` (Retro68) toolchain and a disposable
      BasiliskII System 7.5.3 fixture, plus a from-scratch HFS catalog
      B-tree injector (`host/tools/mac_hfs/`) that places a tiny
      permanent launcher app into `Startup Items` -- BasiliskII has no
      equivalent to amiberry's host-directory-mountable boot volume, so
      Amiga's own CI-signal trick (`amiga_ci_lib.sh`) doesn't transfer
      directly and this was built new, with three real bugs found and
      fixed along the way (one a 100%-reproducible boot-time crash).
      The launcher hands off execution via a real `LaunchApplication`
      call to whatever the CI harness has just copied onto the ExtFS
      share as a plain file -- no per-run disk surgery, no Finder
      interaction at all (Phase 0). Real memory/file/clipboard/timer
      `platform.h` seams (Phase 1), verified on-target via
      `wp_mac_file_clipboard_selfcheck()`. A real Toolbox shell
      (Phase 2): a window, a menu bar built dynamically from
      `platform/common/actions.c`'s `g_wp_actions[]` (no `.r` resource
      file at all), a `WaitNextEvent` loop, and clean quit -- verified
      end-to-end via real screenshots under BasiliskII: the "RetroWP"
      window with a working "File/Edit/Format/Tools/Help" menu bar
      (a real dropdown proven by opening File -> Open/Save/Quit), vi-mode
      `j`/`k` Normal-mode cursor movement, `i`/typed-text/`Escape` Insert
      mode (beyond this task's own stated AC), and `:q<Return>` quitting
      cleanly back to a real Finder desktop. A real, reproducible input
      bug was found and fixed this phase: `WP_MOD_SHIFT` was being set
      whenever the Toolbox's own shift bit was set, but shift is already
      baked into the charCode itself (`:` vs `;`), and
      `engine/src/vimode.c` requires `mods == 0` for its printable-
      command dispatch -- every shifted keystroke was silently rejected
      until this was found by testing real keystrokes, not by
      inspection. `host/tools/ci_mac.sh`, `make mac-engine-check`
      (link-free strict-C89 portability check, mirroring
      `atari-engine-check`/`amiga-engine-check`), and `make test`
      (371/371) all stay green throughout. Full account, including
      every byte offset, the dead-end Finder-alias attempt that led to
      the working launcher design, and every gotcha (missing `Scrap.h`,
      `FSRead`'s `eofErr`-as-clean-EOF semantics, `GetVInfo` having no
      linkable glue implementation, the shift/mods bug, no-window-
      manager input focus under bare Xvfb): `docs/mac-platform.md`.
- [x] T12.2 QuickDraw metrics/render impl of `platform.h`.
      AC: On-screen wrap matches engine layout using real Mac fonts.
      **Met.** Real `GetFNum`/`TextFont`/`TextSize`/`TextFace`/
      `GetFontInfo`/`TextWidth` metrics, a dedicated always-valid
      16x16 bitmap-backed measuring `GrafPort` (avoiding the same class
      of bitmap-less-port memory corruption `metrics_amiga.c` found on
      Amiga), and an exact `/20`/`*20` twips<->pixel conversion (no DPI
      query needed -- classic Mac OS's whole QuickDraw coordinate
      system is built on a 72-DPI convention by design). `draw_document`
      now paints real `wp_paragraph_relayout` output. A real,
      100%-reproducible `SIGSEGV` was found and fixed: the smoke test's
      `main()` never called `InitGraf`/`InitFonts` before this phase
      added QuickDraw-dependent calls. `wp_mac_layout_selfcheck()`
      passes on both ROM images; `RETROWP.APPL` still renders and quits
      cleanly. Full account: `docs/mac-platform.md`.
- [x] T12.3 Mac Serial Driver transport hook.
      AC: Same offload features work as on ST/Amiga. **Met**, with one
      honestly documented gap mirroring T9.3's own precedent:
      `transport_mac.c`'s real Device Manager async I/O implementation
      (`OpenDriver(".AIn"/".AOut")`, `PBReadAsync`/`PBWriteAsync`,
      `SerReset`/`SerHShake`) is proven via `wp_mac_transport_selfcheck`
      and, manually, against a real socat-created PTY wired into
      BasiliskII's own `seriala` prefs key -- unlike amiberry (which
      rejected any non-hardware serial backing for T9.3), BasiliskII's
      serial emulation accepted the PTY outright, and open/reset/
      handshake/write all genuinely worked against it. A real,
      source-verified BasiliskII limitation (its own serial read thread
      blocks on a plain host `read()` and can't honor `KillIO` while
      that call is still pending against a genuinely idle line) means
      the automated selfcheck deliberately does not attempt a live
      read-timeout cycle -- found and root-caused by reading
      BasiliskII's own source, not assumed. `KillIO` itself, like
      `GetVInfo` (T12.2), is declared in Retro68's headers with no
      linkable glue -- worked around via the raw `PBKillIOSync`
      parameter-block call. Full account: `docs/mac-platform.md`.

Beyond the plan's original three tasks, this milestone also delivered a
`platform/common` retrofit (Phase 5): config-driven keymap-scheme
selection (`WP_MAC_CONFIG_PATH`, a bare filename resolving via vRefNum 0
-- the real Mac equivalent of Amiga's `PROGDIR:` convention), a
persistent status line (`wp_statusline_paint`, reserved off the bottom
of the document view exactly like both other backends), and the real
help pager (F1 or the Help menu, over the same compiled-in
`docs/help/retrowp.help` content every backend ships). A real on-target
rendering bug was found and fixed along the way: `draw_mac.c`'s
`plat_fill_rect` ignored its requested color entirely and always filled
with the port's current (black) pen pattern -- invisible on the
document body (which clears via a direct `EraseRect` instead) but a
real, solid-black, no-visible-text help pager until fixed via a real
luminance-threshold pattern selection extending `draw_atari.c`'s own
2-tone approach to a third (gray) tier. `wp_mac_config_selfcheck`/
`wp_mac_help_selfcheck` both pass via `host/tools/ci_mac.sh`; verified
visually via real screenshots (status line showing `NORMAL`, the help
pager showing real linked content and its hint bar, both closing/
quitting cleanly). Full account: `docs/mac-platform.md`.

Also delivered (Phase 6): `host/tools/ci_mac_matrix.sh`, mirroring
`ci_atari_matrix.sh`'s/`ci_amiga_matrix.sh`'s own shape -- both ROM
images pass all five selfchecks, reproduced across multiple clean runs.

Artifact: Mac build "indistinguishable by function." Engine/services
reused unchanged -- confirmed by diff: `engine/` and `services/` show
zero changes across the entire milestone (`git diff --stat
0295f9a..HEAD -- engine/ services/` is empty); every change lives under
`platform/mac/`, `host/tools/`, `docs/`, and `Makefile`. Three genuinely
novel problems (none present in the Atari/Amiga precedent) were solved
along the way: a from-scratch HFS reader/writer safely splicing a file
into a live catalog B-tree; a fully hands-free CI automation mechanism
for an emulator with neither a Native-Features-style exit trap nor a
host-directory-mountable boot volume; and two real Retro68 header/glue
gaps (`GetVInfo`, `KillIO`) found only by attempting to link against
them. Confirmed: `make test` stayed at 371/371 throughout M12 (no
engine/services test count regressions); `atari-engine-check`/
`amiga-engine-check` and the full 6-image Atari TOS matrix/3-image
Amiga Kickstart matrix all stayed green through every phase, a pure
regression gate for the whole milestone, matching M9/M10/M11's own
standing discipline. Full account, every real bug found and fixed,
every honestly-documented gap: `docs/mac-platform.md`.

---

## M13+ — Typography Polish / Apple IIGS
- [x] T13.2 Typography polish: kerning/justification options where supported.
      AC: real on-screen JUSTIFY, not just PDF's. Investigation found
      all three real backends (Atari/Amiga/Mac) silently treated
      JUSTIFY as LEFT (each `*_align_offset_twips` only ever returned a
      nonzero offset for CENTER/RIGHT) -- confirmed a real, three-way
      gap, not hypothetical. Fixed at the engine level, once, in new
      `engine/src/justify.c`/`layout.h`: `wp_line_next_fragment` (a
      shared iterator splitting a line into same-class byte ranges at
      both run boundaries and space/non-space transitions) and
      `wp_justify_space_extra_twips` (per-gap slack, 0 for non-JUSTIFY
      or a paragraph's last line, matching PDF export's own established
      convention). All three backends' paint loops now walk fragments
      instead of concatenating a whole line into one buffer/font --
      which also fixed a real, separate latent bug the old code had:
      a line spanning a style change was drawn entirely in the *last*
      run's font, since `line_style` was overwritten per run and only
      the final value used. Kerning is a **documented non-goal**: none
      of GDOS bitmap/vector fonts, AmigaOS diskfont metrics, or classic
      QuickDraw bitmap fonts expose real pair-kerning tables through
      this project's existing metrics seam, and adding one would be a
      nontrivial per-platform extension disproportionate to the value
      here -- "where supported" is, honestly, nowhere cheaply today.
      **Met.** `make test` grew from 371 to 376 (5 new host-side
      `justify.c` unit tests); each backend cross-compiles clean under
      its own strict toolchain (m68k-atari-mint, m68k-amigaos,
      Retro68's m68k-apple-macos) with the new paint loop, and each
      backend's on-target layout selfcheck gained a JUSTIFY assertion
      (interior-line slack never negative, last line always exactly 0
      -- real font metrics on real hardware can't be hand-computed the
      way the host stub's fake monospace metrics can, so these check
      bounds rather than exact widths).
- [x] T13.3 PDF polish: font embedding tiers, image support.
      AC: investigation found PDF's two documented gaps were very
      different sizes. Font embedding was a real, tractable gateway-
      side addition: `offload/gateway/odt_gateway.py` now backs each of
      its 4 style slots (regular/bold/italic/bolditalic) with either
      Tier 0 (today's standard-14 Helvetica, unchanged, zero
      dependencies) or Tier 1 (real embedded TrueType via `fonttools` +
      Liberation Sans -- metrically Helvetica-compatible by design,
      verified directly), chosen independently per slot and gated on
      `fonttools` being importable, the real font file being found on
      this host, AND that slot actually being used in the document
      (embedding an unused font is pure byte waste) -- any miss falls
      that one slot back to Tier 0. A shared `/ToUnicode` CMap fixed a
      real bug found during verification: `pdftotext` silently
      truncated output partway through a subsetted embedded font
      without it, even though rendering was already correct. Image
      support, by contrast, is NOT a PDF-gateway-sized task: there is
      no image concept anywhere in the core document model
      (`engine/include/docmodel.h`) and no real import path that could
      ever produce one (RTF's `\pict` is discarded on import; ODT has
      no frame handling here) -- doing it properly means extending the
      document model first, a cross-cutting lift on the order of a
      platform milestone, not PDF polish. Deferred, not half-
      implemented; tracked as its own future item below (T13.5).
      **Met** (font embedding half). `pdffonts`/`pdftotext`/`pdftoppm`
      (real `poppler-utils`, not this project's own code checking
      itself) confirmed: a used slot embeds correctly (`emb yes uni
      yes`) while unused slots correctly stay Tier 0 (`emb no`); a
      real multi-line justified paragraph's text extracts fully and
      correctly through the embedded font; the Tier-0-only fallback
      path (`fonttools` simulated missing) reproduces byte-for-byte
      the same small, zero-embedding PDF this gateway always produced
      before this work.
- [x] T13.5 Image support: extend the core document model with a real
      image-run concept (constrained, per existing project guidance, to
      vintage-displayable low-color/palette formats, not modern ones),
      at least one real import path to ever get an image into a
      document (currently none exists in any format), and PDF/on-screen
      rendering. A platform-milestone-sized lift, not PDF polish --
      split out of T13.3 once investigation showed its real scope.
      **Promoted to its own milestone, M15, once picked as the next
      concrete target -- the same way `T13.1` (was DOS/M14) and Amiga's
      own bucket entry each were.** See M15 below.
- [ ] T13.6 `platform/iigs`: Apple IIGS backend, GS/OS System 6.x
      (65816 -- a genuinely new CPU architecture and toolchain for this
      project, not a variant of the 68k Mac backend). User has real
      Apple IIGS hardware available for on-target testing, unlike every
      other backend so far (emulator-only) -- plan for a real-hardware
      verification pass alongside whatever emulator (if any) is used
      for headless CI, the same way M12's BasiliskII fixture work
      should inform but not fully replace real-hardware checks here.

      Scoping research (2026-07-24): real, current toolchain path is
      **ORCA/C + Golden Gate** (github.com/byteworksinc/ORCA-C +
      juiced.gs/store/golden-gate) -- Golden Gate runs ORCA/C directly
      on Linux and produces real GS/OS Toolbox binaries without an
      emulator for the build step itself, closer to a real cross-
      compiler than a from-scratch stack; ~$10, not FOSS. cc65 cannot
      target 65816/GS/OS; llvm-mos has no IIGS support yet (open issue,
      unresolved). CPU is **little-endian** (unlike every 68k backend
      so far) -- needs the same byte-swap-on-I/O treatment `platform/
      dos` already has for x86; engine logic itself needs zero changes
      (endian-neutral by construction, CLAUDE.md rule already covers
      this). GS/OS Toolbox (Window/Menu/Event/Dialog/Scrap Manager,
      QuickDraw II, Font Manager) looks like a real architectural
      sibling of classic Mac Toolbox -- `platform/mac/app_shell.c` is
      a much closer structural template to adapt than a from-scratch
      design, even though CPU/toolchain are unrelated. RAM tier is a
      genuine fit: GS/OS System 6 itself requires 1MB minimum, matching
      this project's own 1MB baseline almost exactly. Real hardware has
      two Toolbox-accessible RS-232 ports on a real Z8530 SCC -- solid
      basis for the offload link. Emulators (KEGS, GSplus) are real and
      maintained but neither documents a headless/CI mode the way
      Hatari/DOSBox do; the same Xvfb+xdotool approach already proven
      for four other backends is a reasonable bet, unverified. Apple's
      ROM is a real licensing gray area (not freely redistributable,
      only informally-circulated archives) -- real hardware access
      sidesteps this for the verification pass that actually matters.

      Progress (2026-09-30), toolchain chosen and the engine compiling:
      - **Toolchain: ORCA/C 2.2.1** via Golden Gate 2.1.0 (the user's
        licence), run as its Windows build under Wine -- the wrapper is
        gem4xe's `ref/iix`. Calypsi 65816 was the alternative and was
        not chosen: it has no IIGS runtime, no toolbox headers, and its
        linker cannot write OMF. ORCA/C has a **16-bit `int`** and 32-bit
        pointers -- the DOS build's int width, which the engine already
        survives.
      - **`make iigs-engine-check`** compiles engine/ + offload/transport
        + services/ (27 files) under `#pragma lint -1`: all clean, no
        source changes needed. It proves itself first on a fixture that
        must be reported, and was shown to fail on a real error in
        `justify.c`, on a file-name collision, and to SKIP without `iix`.
      - ORCA/C has no `-I`; a quoted `#include` searches the **current
        directory** (not the including file's) and then
        `Libraries:ORCACDefs:`, which ships its own `types.h`,
        `memory.h`, `font.h`, `event.h` and `window.h`. The check stages
        sources flat and runs from inside that directory; the backend's
        real build will need the same care.
      - **Emulator: MAME's `apple2gs` runs headless**, superseding the
        Xvfb bet above: `mame apple2gs -sl7 cffa2 -hard1 <image>` boots
        GS/OS 6.0.4 to the Finder in ~75 emulated seconds, and a Lua
        `-autoboot_script` takes the screenshots. Image and script live in
        `~/dev/toolchains/iigs/`. (Needs `-noplugins`, not `-plugins 0`,
        which MAME reads as a software name.)
      - Not started: an OMF link of the engine, running the host test
        suite under Golden Gate (it can execute text-mode GS/OS
        programs, so 16-bit-int behaviour can be tested, not just
        compiled), and `retroplat/backends/iigs`.
- [ ] T13.7 `platform/c900`: Commodore 900 backend, Coherent (a Unix
      work-alike by Mark Williams Company) on a Zilog Z8001. Requested
      2026-07-24. The most architecturally different backend this
      project would ever build -- not a GUI-toolkit port at all, but
      this project's first genuine Unix-tty backend: cursor positioning
      via terminal escapes through a real tty layer, not a direct video
      memory poke (`platform/dos`) or a GUI event loop (GEM/MUI/
      Toolbox). CPU is **big-endian** (Z8000 family, like 68k) -- zero
      endian-adaptation needed, unlike the IIGS's little-endian 65816.

      Scoping research (2026-07-24): public specs for the real C900 are
      thin and conflicting across sources (clock speed cited as both
      6 MHz and 10 MHz; RAM cited as both 512KB and 1MB/1024KB stock)
      -- only 50 prototypes were ever built, none sold commercially, so
      there is no authoritative spec sheet the way there is for a mass-
      market machine. None of that uncertainty matters much in practice
      though: `github.com/MichalPleban/commodore-900-emulator` (MIT-
      licensed, plain C99, `make`-buildable) is a real, purpose-built,
      **headless-by-design** Z8001+Z8010 emulator explicitly built "for
      software cross-development on the host machine" -- 1MB RAM, 20MB
      disk, one serial line that redirects straight to the host
      console, `--input` for scripted keystrokes, `--max=N` instruction
      caps, and a `--selftest` mode needing no ROM/disk at all. This is
      a better native CI fit than any of the four existing backends'
      own GUI emulators (Hatari/amiberry/BasiliskII/DOSBox), all of
      which need the Xvfb+xdotool workaround this one wouldn't. It
      ships a real BIOS dump and a bootable Coherent 0.7.3 disk image
      directly in the repo, under a genuine BSD-style redistribution
      license from Robert Swartz (the real Mark Williams Company
      rightsholder) -- legally clean, unlike the IIGS ROM situation.
      Only models one serial line total, almost certainly needed for
      the Coherent console/tty itself -- testing the offload link
      specifically may need more serial modeling than this build has;
      real Model 1 hardware had multiple RS-232 ports.

      **The one real open question, and it's a hard gate, not a detail:
      the toolchain.** No modern Z8000 cross-compiler exists (a ~1998
      Cygnus GCC backend is long dead/unmaintained). The realistic path
      is compiling *on* the emulated machine using Coherent's own
      bundled native C compiler (the repo's `tools/disk.py` can copy
      files on/off the disk image for exactly this) -- meaning
      iteration happens at emulated-Z8001 speed, not a fast host cross-
      compile, a genuinely different workflow than every other backend
      here. Worse, Coherent 0.7.x predates ANSI C89 (ratified 1989) --
      Mark Williams' compiler is plausibly K&R-only, and it's genuinely
      unknown whether this project's strict-C89 engine compiles as-is
      or needs a real compatibility pass for this one target.

      **Recommended before any real platform/c900 work starts**: a
      Phase 0 spike, the same shape M15's own Pillow go/no-go gate had
      -- prove Coherent's native `cc` can build a trivial C program
      exercising a few of this project's real patterns (`const`,
      function prototypes, the fixed-width `types.h` typedefs) inside
      this emulator. That answers the one question that actually gates
      the rest of the milestone, cheaply, before committing to a full
      plan.

---

## M14 — Fourth Platform: DOS (Text-Mode, Open Watcom)
Goal: Validate the portability thesis a fourth time, against a target
with no real GUI at all -- a fixed 80x25 character-cell screen, no
window manager, no proportional fonts, no OS-level clipboard -- proving
the engine's twips-based layout degrades correctly to the monospace
case rather than needing a special code path for it. Promoted out of
`M13+`'s bucket (was `T13.1`) once picked as the next concrete target,
the same way `T12.1` (Mac) and Amiga's own entry each were.

- [x] T14.1 `platform/dos`: video-mode setup, direct-video-memory text
      output, BIOS-keyboard event loop, clean exit.
      AC: builds via Open Watcom (`-bt=dos`, real 16-bit real-mode
      `.EXE`); boots under DOSBox against the Phase 0 fixture; a real
      captured screen shows live text output and correct keyboard echo.
      **Met.** `platform/dos/app_shell.c` sets 80x25 text mode (`INT
      10h`), writes a title bar + typed text directly to `B800:0000`
      video memory (no BIOS teletype -- the standard, expected DOS
      text-mode technique), and reads keys via `INT 16h` (`<i86.h>`'s
      `int86`/`union REGS`/`MK_FP`, not inline assembly). No engine/
      platform.h linkage yet -- a deliberately raw hardware-seam-only
      scope, same as Phase 0's own smoke test. Verified with real
      keyboard interaction, not just a build: a new headless-DOSBox
      keystroke-injection + screenshot technique (`xdotool key
      --window` + `import`, found this phase -- see the new "DOSBox
      keyboard injection technique" project memory) confirmed printable
      characters, Enter (new line), Backspace (erase), and Escape (clean
      exit -- screen clears, control returns to the DOS prompt) all
      work correctly on real captured screens, not just by reading the
      code. `make test` unchanged at 376/376 (no engine diff, expected
      -- this phase is platform/dos-only); `dos-engine-check` and
      `ci_dos.sh` both still green.
- [x] T14.2 `metrics_dos.c`/`draw_dos.c`: fixed-cell font metrics +
      direct `B800:0000` video-memory rendering.
      AC: `wp_dos_layout_selfcheck` (mirroring the other three
      backends' own selfchecks) passes; on-screen wrap matches engine
      layout for the fixed character cell.
      **Met.** Real, historically-grounded monospace convention (10
      characters/inch, 6 lines/inch -- 144/240 twips per cell, not an
      arbitrary ratio); `plat_font_metrics_get`/`plat_measure_text`
      ignore the font's requested `size_twips` (plain BIOS text mode
      has exactly one fixed ROM cell -- a documented gap, not silent);
      `plat_draw_text`/`plat_fill_rect` write straight to `B800:0000`,
      with a real nearest-match across the 16 standard CGA/EGA/VGA
      colors (genuinely more than the monochrome ST/Mac backends can
      do). `app_shell.c`'s new `render_fixture_paragraph` proved the
      WHOLE pipeline on a real captured DOSBox screen, not just
      `wp_dos_layout_selfcheck`'s own pass/fail: a real fixture
      paragraph, laid out by the real engine, wrapped and rendered
      correctly at a real column width. Because DOS's cell is exactly
      fixed (unlike real GDOS/diskfont/QuickDraw fonts), the selfcheck
      itself asserts exact hand-computed widths, not just bounds -- a
      stronger check than the other three backends could manage.
      Two real bugs found and fixed during bring-up: (1) linking the
      full engine under Watcom's small memory model (`-ms`) overflowed
      the one 64KB code segment by ~16KB at the very first link
      attempt -- fixed by switching to medium model (`-mm`); (2)
      `wp_paragraph_relayout`'s 512-entry `token_t` stack array
      (`engine/src/linebreak.c`) silently overflowed Open Watcom's
      default DOS stack, crashing the guest partway through the first
      relayout call with no error -- fixed with a real, larger linked
      stack (`-k32768`), not by shrinking the engine's own buffer; (3)
      `plat_window_create` was redundantly re-invoking BIOS SetVideoMode
      (which always blanks the display, even when re-requesting the
      already-active mode), silently wiping the title bar whenever
      called after startup -- fixed by never setting video mode there
      at all, since app_shell.c's own startup sequence already owns
      that. All three are exactly the kind of host-only assumption
      (ample code segment size, ample stack, "setting a mode twice is
      free") this project's cross-compile discipline exists to catch.
- [x] T14.3 File I/O, clipboard, timer.
      AC: real file round-trip via Open Watcom's own libc; in-process
      clipboard buffer (DOS has no OS-level clipboard service to
      integrate with -- `platform.h`'s clipboard interface doesn't
      require cross-app sharing, so this is a conformant implementation,
      not a shortfall); timer via `INT 1Ah`/libc clock.
      **Met**, alongside T14.2 rather than after it: file/clipboard/
      timer turned out to be a hard prerequisite for linking the engine
      at all (every backend links its whole `ENGINE_OBJ` as one
      indivisible unit), so their real implementations already existed
      by the time T14.2's build succeeded. `wp_dos_file_clipboard_
      selfcheck` (mirroring `wp_atari_file_clipboard_selfcheck`'s exact
      shape) proves it for real, not just that it compiles/links --
      confirmed passing on real DOSBox via `ci_dos.sh`.
- [x] T14.4 Serial transport (offload parity).
      AC: mirrors T6.3/T9.3/T12.3's own shape -- `INT 14h`/direct UART
      on real hardware, DOSBox's `--socket` null-modem-over-TCP for CI.
      Zero changes expected to `offload/transport`, `offload_client.c`,
      or `offload.h`.
      **Met.** `transport_dos.c` implements the real BIOS `INT 14h`
      serial services (init/send/receive/status), same `"serial:
      <device>:<baud>"` endpoint convention every prior backend uses;
      zero changes to `offload/transport`/`offload_client.c`/
      `offload.h`, confirmed. A real, confirmed DOSBox limitation was
      found and fully diagnosed, not just noticed: the default
      `serial1=dummy` device's `AH=01h` (send) unconditionally reports
      a timeout despite `AH=03h`'s own status poll correctly showing
      "ready" -- reproduced with an isolated raw `INT 14h` program with
      no dependency on this project's own code, and confirmed
      independent of CPU speed and of manually asserting DTR/RTS/OUT2
      via a direct Modem Control Register write (so not a fixable
      timing or handshake issue on this side; DOSBox's own `--socket`
      nullmodem alternative was also tried and needs a live peer
      connection to even proceed, adding complexity without yet
      proving the underlying send path works either). Same category of
      finding as Mac's own BasiliskII serial-read-hang gap -- honestly
      documented (`docs/dos-platform.md`), not silently worked around.
      `wp_dos_transport_selfcheck` (mirroring `wp_amiga_transport_
      selfcheck`'s shape) still exercises write for real but doesn't
      assert `WP_OK` from it, while asserting what this environment CAN
      fully verify as hard requirements: open succeeds; a subsequent
      read correctly times out within the deadline logic's own
      generous bounds; and `offload_init`/`offload_available`/
      `offload_shutdown` all behave correctly with no live gateway
      attached. Passes reliably via `ci_dos.sh`. `make test` (376/376)
      and Atari/Amiga/Mac engine-checks stay green throughout.
- [x] T14.5 `platform/common` retrofit: keys, actions, status, help.
      AC: real keyboard-event translation wired through the same
      `platform/common/keymap.c` dispatch every other backend already
      uses, with zero scheme-side changes -- mirrors T8.5.9/M9/M12's
      own retrofit scope.
      **Met.** `dos_translate_key` (real BIOS `INT 16h` scan+ASCII+
      shift-state) feeds `wp_keymap_feed_key` -- vi-mode by default,
      zero scheme-side changes, confirmed by the fact platform/common/
      keymap.c/actions.c/config.c/statusline.c/help_doc.c/help_view.c
      all cross-compiled and linked completely unmodified. New
      `dialog_dos.c` implements `plat_dialog_message`/`plat_dialog_
      choice` as a modal bottom-row prompt -- the "numbered text prompt
      on DOS" shape `platform.h`'s own doc comment already anticipated.
      The document body paint loop uses the real `wp_line_next_
      fragment`/`wp_justify_space_extra_twips` walk (T13.2) from day
      one, not the older approach the other three backends had to
      later fix. Two real bugs found and fixed, both via genuine
      interactive screenshot verification, not selfchecks alone: (1)
      typed text never appeared on screen at all -- root cause,
      confirmed by direct code search, is that NOTHING in the shared
      engine/keymap dispatch path calls `wp_paragraph_relayout` after a
      per-keystroke edit (only a whole-document replace does); this is
      plausibly a **latent gap on Atari/Amiga/Mac too**, not DOS-
      specific -- simply never caught there since none of their own
      interactive verification passes actually typed new text into an
      already-loaded document and checked a real screen (see the new
      "Missing relayout after edit bug" project memory). Fixed for DOS
      by relayouting once before every paint. (2) status-line text was
      rendering solid black-on-black -- `plat_draw_text` was
      unconditionally forcing a black background on every cell,
      silently overwriting the status bar's own light-gray fill (a
      failure mode with no equivalent on a real bitmap display, where
      drawing text never touches surrounding pixels at all); fixed by
      reading back and preserving each cell's existing background
      nibble. Interactively verified end-to-end on real captured
      DOSBox screenshots: real vi-mode Insert-mode typing, `NORMAL`/
      `INSERT` status switching, the help pager's real content and
      link navigation, and a clean Ctrl+Q quit. Three new selfchecks
      (`wp_dos_key_translate_selfcheck`, `wp_dos_config_selfcheck`,
      `wp_dos_help_selfcheck`, mirroring their Atari/Amiga counterparts
      exactly) pass via `ci_dos.sh`. `make test` (376/376) and every
      other backend's engine-check stay green throughout.
- [x] T14.6 CI matrix + docs + milestone closeout.
      AC: `host/tools/ci_dos_matrix.sh` (machine-type matrix: hercules/
      cga/ega/svga) green; new `docs/dos-platform.md`; `make test` and
      every prior backend's own CI matrix stay green throughout (pure
      regression gate); `engine/`/`services/` show zero diff, verified
      via `git diff --stat`, the same proof M9/M12 each used.
      **Met.** `host/tools/ci_dos_matrix.sh` mirrors `ci_atari_matrix.sh`/
      `ci_amiga_matrix.sh`/`ci_mac_matrix.sh`'s own shape. Hercules was
      tried first (per the plan's own scope) and dropped for a real,
      confirmed reason rather than kept: a headless-DOSBox screenshot of
      real `RETROWP.EXE` under `--machine hercules` shows a solid black
      screen, and DOSBox's own log explains why -- it silently boots
      "Hercules 640x350 monochrome text mode 07h", not this backend's
      requested 80x25 16-colour mode 03h, since real Hercules hardware
      never supported color text mode 03h at all, only mode 7 (mono
      text at video segment B000, not the B800 this backend's
      `draw_dos.c` hardcodes). Same category of exclusion as Amiga's
      Kickstart 3.0 and Atari's TOS 1.00 (a genuine structural
      incompatibility, honestly documented, not silently dropped) --
      and not a surprise: `docs/dos-platform.md`'s own Architecture-fit
      section had already named MDA/Hercules-at-B000 a real, documented,
      not-required-for-v1 gap before this phase ever ran. `cga`/`ega`/
      `svga_s3` -- the three DOSBox profiles that share mode 03h's
      color/B800 model -- each verified with a real screenshot of
      `RETROWP.EXE` (correct title bar, word-wrapped sample text, status
      line) before the matrix script existed, then via `ci_dos_matrix.sh`
      itself: `DOS MACHINE MATRIX: 3 passed, 0 failed`. Full regression
      swept and green: `make test` (376/376); `ci_atari_matrix.sh` (6/6);
      `ci_amiga_matrix.sh` (3/3); `ci_mac_matrix.sh` (2/2).
      `git diff --stat -- engine/ services/` shows exactly one file
      (`engine/include/layout.h`), confirmed via `git log` to be
      pre-existing T13.2 (M13a) work already on disk before this
      milestone began, not introduced by any DOS phase -- no `engine/`/
      `services/` file was touched by this milestone at all, the same
      portability proof M9/M12 each gave. M14 is complete.

---

## M15 — Image Support (Placed Bitmaps, Offload-Downconverted)
Goal: give the document model its first non-text content type. Promoted
out of `M13+`'s bucket (was `T13.5`, split out of `T13.3` once
investigation showed extending the core document model was a
platform-milestone-sized lift, not PDF polish) once picked as the next
concrete target, the same way `T13.1` (DOS/M14) and Amiga's own entry
each were. Scope was set by the user directly: a source image is
decoded and downconverted **off-device** on the Pi gateway -- mirroring
how ODT/PDF conversion already works -- with "even just a frame for
placement" deliberately setting the bar low for a first cut on the
lowest-color-depth targets. `platform/dos` was found, during planning,
to have zero pixel-addressable graphics active at all (pure 80x25
text-mode, every draw call a direct `B800` character-cell poke) --
asked and answered directly by the user: DOS gets a text-only
bracketed placeholder marker, the same honest, permanent,
documented-not-silent exclusion this project already gives Hercules/
Kickstart 3.0/TOS 1.00, rather than a parallel pixel-graphics subsystem.

**Unlike M9/M12/M14, this milestone shows real, substantial diff in
`engine/`** (`docmodel.h`/`.c`, `editing.h`, `layout.h`, `undo.c`,
`linebreak.c`, `justify.c`, `platform.h`, `offload.h`) -- expected and
correct, not a regression from the "zero engine diff" precedent those
three milestones set. Those milestones each proved the portability
thesis (new backend, same unmodified core); this one's entire point is
extending the single source of truth itself (`CLAUDE.md` rule 2) to a
genuinely new content type. `services/` stays untouched throughout.

- [x] T15.1 Pillow spike (go/no-go gate).
      AC: a standalone, throwaway script proves Pillow decode + resize +
      `quantize(colors<=16)` and `.convert('1', dither=FLOYDSTEINBERG)`
      on real source images, each downsampled to <=320x200, produces a
      visually reasonable low-color result -- settling whether Pillow is
      a reasonable real gateway dependency before any wire-format byte
      layout is locked in.
      **Met.** Go. Nothing from this spike ships -- by design, it's a
      throwaway proof, not part of `make test` or any permanent gateway
      module -- but it directly unblocked T15.4's own wire-format byte
      budget (see below) and confirmed a 4-bit packed palette index is
      enough headroom for a recognizable result at the ST low-res
      ceiling.
- [x] T15.2 Document model + insert-image API + undo.
      AC: a `wp_document.images[]` side table mirroring `cstyles`/
      `pstyles` exactly; a small, always-resident `wp_image_desc`
      descriptor (never raw pixel bytes inline, mirroring
      `services/spell/spell.c`'s own "descriptor + read-on-demand"
      precedent); a `kind`/`image` field pair on `wp_paragraph`, zero-
      init defaulting to `WP_PARA_TEXT` so every pre-M15 consumer stays
      correct without a code change; a new `wp_edit_insert_image`
      (declared in `editing.h`, implemented in `undo.c` alongside the
      undo/redo machinery it's paired with) splitting the paragraph at
      a position, same shape `wp_edit_split_paragraph` already has;
      undo/redo via a new `WP_OP_INSERT_IMAGE`, reusing the 16-bit
      `style_before` field
      to stash the registered image handle -- the same field-reuse trick
      `WP_OP_SPLIT_PARA`/`MERGE_PARA` already established, so `wp_op`
      itself needed no new fields. Host tests only; zero changes to any
      pre-existing test file.
      **Met.** `make test` grew by 4 (`image_add_get_roundtrip`,
      `image_add_survives_growth`, `edit_insert_image_splits_paragraph`,
      `edit_insert_image_undo_redo`); every pre-existing suite unchanged.
      See `docs/document-model.md`'s new Image section for the full
      struct-level picture.
- [x] T15.3 Layout integration.
      AC: an early `kind`-branch in `wp_paragraph_relayout` bypassing
      `tokenize()` entirely for an image paragraph; `kind`-guards inside
      the shared `wp_line_next_fragment`/`wp_justify_space_extra_twips`
      primitives (`justify.c`) rather than trusting four independent
      per-backend paint loops to remember the check; an explicit
      regression proof that a mixed text+image document paginates
      correctly using only `height_twips`, proving `paginate.c` itself
      needed zero changes.
      **Met.** `make test` grew by 5 (`image_layout_relayout_produces_
      one_line_from_descriptor`, `image_layout_missing_handle_degrades_
      to_zero_size`, `image_layout_fragment_iter_returns_false_
      immediately`, `image_layout_justify_extra_is_zero`,
      `image_layout_paginate_mixed_text_and_image`); every pre-existing
      suite unchanged. `engine/src/paginate.c` has zero diff, confirmed
      -- the "minimal recompute" proof point `docs/layout.md`'s new
      paragraph-kind section documents in full.
- [x] T15.4 Wire protocol + gateway; Insert-image UI action, all four
      backends.
      AC: real `OFFLOAD_IMAGE` (request = target color/flags/max-w/max-h
      + source bytes; response = width/height/palette/packed 4-bit
      pixels), sized so the worst case stays under the confirmed
      existing `OFFLOAD_MAX_RESPONSE_PAYLOAD` (32768 bytes) with zero
      protocol-layer changes to `wire.h`/`caps_codec.h`/
      `offload_client.c`; a new Pillow-backed `offload/gateway/
      image_gateway.py`, gated exactly like `odt_gateway.py`'s existing
      `fonttools`-optional-dependency pattern; `mock_gateway.c` gains a
      deterministic canned-fixture `IMAGE` handler; `test_offload.c`'s
      two "always unsupported" fixture assertions repointed from
      `OFFLOAD_IMAGE` (now real) to `OFFLOAD_CONVERT_IMPORT`; a new
      `WP_ACT_INSERT_IMAGE` action wired on all four backends, each
      reusing its own existing native file picker, capability-gating on
      the CAPS bit before submitting (mirroring T8.3 CHAT's own
      pre-check discipline, not submit-and-hope), writing the response
      to a local pixel-cache file, then calling `wp_edit_insert_image`.
      **Met.** `OFFLOAD_MAX_RESPONSE_PAYLOAD` stayed exactly 32768 --
      confirmed no protocol-layer file needed touching. `offload.h`'s
      own `OFFLOAD_IMAGE` doc-comment (a stale M8/T8.3 placeholder
      reading "art generation (optional)") is corrected to describe the
      real, shipped feature. `make test` grew by 6
      (`offload_capabilities_advertise_image`,
      `offload_image_returns_canned_fixture_bitmap`,
      `image_convert_real_pillow_color_round_trip`,
      `image_convert_real_pillow_grayscale_round_trip`,
      `image_convert_pillow_unavailable_reports_unsupported`,
      `image_convert_malformed_request_is_error`) -- the middle two
      round-trip a real fixture PNG through the real Pillow-backed
      gateway process, not a hand-fed byte array. Each backend's own
      transport selfcheck gained an `OFFLOAD_IMAGE` case against
      `mock_gateway`'s fixture, and real interactive verification on
      each backend (including DOS, whose own confirmed DOSBox
      serial-write limitation from M14 T14.4 meant this had to lean on
      the same fixture-injection technique later phases also used)
      confirmed a correctly-sized image paragraph lands after a real
      file pick.
- [x] T15.6 Rendering: real bitmap content on Atari/Amiga/Mac, honest
      text placeholder on DOS.
      AC: a new `plat_draw_image(gc, x, y, w, h, pixels, src_w, src_h,
      palette, palette_count)` seam in `platform.h` -- deliberately not
      required on every backend the way `plat_dialog_choice` is, so DOS
      (which never calls it) needs no stub; each of Atari/Amiga/Mac
      first proves the whole pipeline with a solid placeholder rect
      (the palette's dominant color, zero new `platform.h` surface),
      then replaces it with a real native blit; DOS instead prints
      `[image: <source_name> <width_px>x<height_px>]` via the
      *existing* `plat_draw_text` seam, no new surface, no video-mode
      switch; every backend's own layout/image selfcheck gains a
      position/size assertion (`wp_paragraph_relayout` produces exactly
      the descriptor's `display_width_twips`/`display_height_twips`);
      every rendering phase gets a real captured screenshot under the
      relevant emulator, never selfcheck alone.
      **Met**, with three real, non-obvious bugs found and fixed along
      the way, none of them hypothetical:
      (1) **Amiga**: `WritePixelArray8` returns the actual pixel count
      copied, not a status code -- the initial `!= 0` success check
      flagged every genuinely successful blit as a failure, root-caused
      via a diagnostic build showing `plat_draw_image` returning
      `WP_ERR` even though the file read and depth detection were both
      already correct. Fixed by comparing the return value against the
      expected `w * h` pixel count; verified via a real amiberry
      screenshot showing genuine per-pixel diagonal-pattern content,
      not a placeholder rect.
      (2) **Atari**: this project's real, established boot convention
      (`docs/atari-platform.md`) is Medium resolution (4 colors total),
      not Low (16) -- an initial 16-color assumption would have
      silently overflowed the ST's actual palette hardware. Fixed with
      a new, real, runtime-queried `atari_screen_max_colors()` (VDI
      `work_out[13]`, mirroring `metrics_atari.c`'s own DPI-query
      precedent), dynamically sizing both the `OFFLOAD_IMAGE` request's
      target-color budget and the palette-programming loop in
      `plat_draw_image` to whatever the real screen actually supports --
      reserving pens 0/1 for the existing G_WHITE/G_BLACK text VDI
      already owns, and gracefully returning `WP_ERR` (triggering the
      existing placeholder-rect fallback) on a genuinely mono screen
      with no spare pen at all. Verified via a real Hatari screenshot
      showing a genuine checkerboard pattern.
      (3) **Mac**: a 4x4 ordered (Bayer) dither reduces each pixel's
      palette RGB to this backend's hard-capped 1-bit `CopyBits` tier;
      the initial `bayer_val * 17` threshold scaling mapped the dither
      matrix's max coefficient to exactly 255, which could misclassify
      a pure-white pixel (luminance 255) as black at that one dither
      cell (`255 <= 255` is true) -- fixed to the standard `bayer_val *
      16 + 8` scaling (max threshold 248, strictly below 255). A second,
      separate robustness gap found while designing the no-crash
      selfcheck: `plat_draw_image` had no `mgc->window == NULL` guard
      before calling `SetPort`, which would crash on real hardware if
      ever invoked before a window exists -- added for parity with
      Amiga's own `screen == NULL` guard. Verified via a real BasiliskII
      screenshot showing genuine dithered gray/textured content.
      DOS's own marker path needed no bitmap work at all, only new
      `dos_format_image_marker`/`dos_append_bytes`/`dos_append_decimal`
      helpers (mirroring `platform/common/statusline.c`'s own
      no-sprintf discipline) feeding the existing `plat_draw_text` seam;
      `wp_dos_layout_selfcheck` gained the same position/size assertion
      plus an exact byte-for-byte marker-text assertion. Verified via a
      real DOSBox screenshot: `[image: PHOTO.PNG 320x200]` rendered on
      its own line directly below the surrounding text, in the correct
      position, with cursor motion across/around the image paragraph
      confirmed stable and non-crashing over repeated single-key presses
      (an initial apparent full-screen blank after a rapid, undelayed
      5-key burst was isolated to a screenshot-capture timing artifact,
      not an application fault -- a single subsequent keypress
      immediately restored the correct, stable frame, and step-by-step
      single-key repeats never reproduced it). All temporary
      debug-fixture-injection scaffolding used for interactive
      verification on all four backends was fully removed afterward,
      confirmed via `grep -rn "TEMPORARY" platform/` returning empty.
- [x] T15.7 CI matrix + docs + milestone closeout.
      AC: all four backends' existing CI matrices stay green throughout
      (pure regression gate, not new coverage); new `docs/document-
      model.md` Image section and `docs/layout.md` paragraph-kind
      section; `docs/milestones.md` M15 section with an explicit
      call-out that real `engine/` diff is expected here, unlike M9/M12/
      M14's zero-diff precedent.
      **Met.** Full regression swept and green: `make test` (391/391,
      grown from 376 at the start of this milestone -- 15 new image-
      related tests across T15.2/T15.3/T15.4, zero regressions to any
      pre-existing suite); `ci_atari_matrix.sh` (6/6 TOS images);
      `ci_amiga_matrix.sh` (3/3 Kickstart versions); `ci_mac_matrix.sh`
      (2/2 ROMs); `ci_dos_matrix.sh` (3/3 machine profiles). `git diff
      --stat -- engine/ services/` shows real, substantial diff across
      `docmodel.h`/`.c`, `editing.h`, `layout.h`, `offload.h`,
      `platform.h`, `linebreak.c`, `undo.c` (the `kind`-guards this
      milestone added to `justify.c` land in a file T13.2/M13a already
      introduced, so they don't show as a new file here, only as part
      of `layout.h`'s own diff) -- deliberately not a repeat of
      M9/M12/M14's "zero engine diff" portability proof, since this
      milestone's entire point was extending the single source of truth
      itself (`CLAUDE.md` rule 2) to a genuinely new content type;
      `services/` shows zero diff, confirmed. M15 is complete.

---

## M16 — Page Furniture: Boxes, Tables, Frames, Vector Art
Goal: documents that are laid out, not just written -- the user's test
case is a tabletop RPG character sheet (Powers & Perils, GURPS 3rd
edition): boxed sections, grids of stats, ruled write-in lines, a
decorated heading. Today the model is paragraphs of text plus picture
paragraphs (M15), so none of that can be built. Decided with the user
(2026-11-09):

- **Paper matches the screen.** Whatever a document shows is what every
  printer path draws -- GDOS (pageprint), PostScript and PCL rules where
  they can, box-drawing characters where only text can be sent. No
  printer-only substitutions (the default font prints as the screen's).
- **Boxes and tables first**, then frames, then vector art, then
  clipart: each step is useful alone, and the cheap ones cover most of a
  character sheet.
- **RTF carries all of it.** Every addition reads and writes standard RTF
  (borders, tables, frames, metafile pictures), so a sheet made in Word
  or another word processor opens here, and one made here opens there.

Rules carried from earlier milestones: the model is the single source of
truth (each feature starts in `docmodel.h`, not in a painter); every
backend draws it or degrades honestly and documents how -- DOS text mode
gets code page 437 box-drawing characters, never a parallel pixel layer
(M15's precedent); 1 MB ST stays the baseline; `format_prn` degrades the
same way the screen does. Fixtures are our own layouts "in the spirit
of" those sheets, not copies -- the published sheets are copyrighted.

- [x] T16.0 Print pictures. pageprint (GDOS) drew a picture paragraph's
      frame only; it now draws its pixels at the printer's resolution:
      scaled to the box, dithered to black and white by brightness (a 4x4
      ordered dither -- a printer has two colours and the screen's
      palette pens are not its), 32 rows at a time through a 1-bit call,
      so a full-page picture costs a band of bits, not a page.
      The 1-bit call is the device's choice (wp_page_device.draw_mono):
      SpeedoGDOS's FX80 driver ignores vrt_cpyfm entirely, so without
      NVDI the ST draws a picture as runs of filled bars; NVDI's drivers
      take the raster copy, which printed the test picture in 30 s
      against 5.5 min as runs, pixel for pixel the same. The test is
      NVDI's cookie, since NVDI brings its own GDOS. Open: NVDI with its
      GDOS switched off in favour of SpeedoGDOS would take the copy and
      could print pictures blank; FSMC cannot tell (NVDI also points it
      at '_SPD', version 5.00, where this SpeedoGDOS says 4.00), and
      NVDI's configuration word (0x0215 here) is undocumented.
      AC met: an RTF with a 3 x 1.5 inch grey-ramp picture printed under
      SpeedoGDOS (FX80, TOS 1.04) and under NVDI with MagiC (PCL, 300
      dpi, from the user's Falcon disk), both decoded and checked; host
      test for the band, the runs and the frame fallback.
      Found on the way, not fixed here: a picture paragraph ignores its
      alignment (a centred one sits at the left margin, on screen and on
      paper); on the Falcon's 16-bit true-colour screen a picture draws
      scrambled (plat_draw_image's palette-pen path assumes planes).
- [x] T16.1 Paragraph borders and shading. A box, or any of its four
      sides, around a paragraph: single, double or thick rule, a width,
      the gap to the text; and a grey or colour fill behind it.
      Consecutive paragraphs with the same borders share one box (Word's
      rule), which is how a boxed section of several paragraphs is made.
      Model: border and shading fields in wp_para_style (a sides mask,
      kind, width and spacing in twips, fill colour/percent). Layout:
      the box's space added to the first and last line boxes.
      Drawing: every painter (ST, Amiga, Mac by plat_fill_rect; DOS by
      box-drawing characters); pageprint; format_prn (PostScript and PCL
      draw rules and fills, Epson and plain text box-drawing characters
      in code page 437 or ASCII +-|).
      RTF: \brdrt \brdrb \brdrl \brdrr \box, \brdrs \brdrdb \brdrth,
      \brdrwN \brspN, \shadingN \cbpatN.
      UI: Format > Borders (plat_dialog_choice: none, box, top, bottom,
      top and bottom; single, double, thick; shading none/10/25/50%).
      Not journaled, like every paragraph-style change today.
      AC: host tests for layout geometry, merging, RTF round-trip and
      format_prn output per driver; screenshots on ST, Amiga, Mac and
      DOS; a GDOS printout matching the screen.
      Done (2026-11-09). Each paragraph rules its own sides, so layout
      stays local; RTF's \box-on-every-paragraph is joined into one box
      on import. Shading draws through a new retroplat call,
      plat_fill_shade (VDI and QuickDraw patterns; a real grey on colour
      screens; a blue background on DOS), so it survives mono screens
      and printers; ST text is now drawn transparent so it does not
      punch holes in shading, and VDI's fill perimeter is off for it.
      Verified: ST screen and SpeedoGDOS printout of a test sheet (title
      under a double rule, a shaded three-paragraph box, a thick rule, a
      shaded paragraph) match; DOS shows the box in code page 437 (its
      conversion now covers 437's upper half, accents included), opened
      as RETROWP TEST.RTF -- DOS and the Amiga now take a file on the
      command line. The Amiga, given TEST.RTF in its Shell, shows the
      same rules and box, its shading an area pattern (Workbench's four
      pens have no light grey) under text now drawn JAM1. The Mac builds
      with the painter wired; its screenshot waits for T16.6's fixture
      (it opens no file at start-up yet).
      DOS sets borders with Alt+R (decided with the user: its button
      bar has no room, 80 columns for 26 buttons; gem4dos will give DOS
      the ST's buttons and menus later). Open: character printers print no shading, and the spacing
      lines inside a multi-paragraph box carry no side rules; top and
      bottom rules on DOS need a row of spacing.
- [x] T16.2 Tables. Rows of cells, each cell its own paragraphs, cell
      widths and borders per row, rows never split across a page (first
      cut). The model follows RTF's own flat shape rather than nesting:
      a paragraph carries the table, row and cell it belongs to, and a
      row-definition table holds each row's cell edges and borders, so
      the editing, undo and paragraph-walking code keeps its one list.
      T16.2a writes that design into docs/document-model.md before code.
      Layout: a row's cells laid out side by side at their widths, the
      row as tall as its tallest cell. Editing: Tab and Shift+Tab move
      between cells; Insert Table (rows x columns), insert and delete
      row and column. Drawing: cell borders as T16.1's rules; DOS and
      character printers by box-drawing characters.
      RTF: \trowd \cellxN \clbrdrX \intbl \cell \row \trgaphN \trleftN.
      AC: host tests (layout of a 3x4 table, cursor movement between
      cells, RTF round-trip of a Word-made table fixture); screenshots on
      every target; a character-sheet stat grid built and printed.
      [x] T16.2a the design, in docs/document-model.md ("Tables").
      [x] Stage 1 (2026-11-09): the model (wp_paragraph.trow/tflags, the
      document's row shapes), RTF \trowd \trleft \trgaph \cellx
      \clbrdr* \clshdng \intbl \cell \row in and out (tables used to
      import as tab-separated paragraphs), layout at each cell's width,
      the table pass placing every line (cell edges, offset in its row,
      the row's height, an advance that walks a table-blind walker past
      it, rows kept on one page), and drawing on every screen and the
      page printer -- a shared line walker that sums each line's
      rounded height (a converted total rounds differently: DOS), cell
      borders and shading at a row's first line with shared edges ruled
      once; DOS rules cells with vertical box characters. Clicks find
      the cell by x; Enter in a cell keeps both halves in it; Backspace
      at a cell's start joins nothing. Seen on the ST (screen and
      SpeedoGDOS print), Amiga and DOS; the Mac builds.
      [x] Stage 2 (2026-11-09): Tab and Shift+Tab move between cells in
      every scheme (keymap.c, ahead of the schemes; Shift reaches Tab on
      every machine now, DOS through the BIOS's back-tab), Tab past the
      last cell adds a row. Format > Table... (a ruler button, the Amiga
      and Mac Format menus, Alt+T on DOS): insert a table, insert or
      delete a row or a column (engine/src/table.c), each one undo step
      over three new journaled pieces -- a paragraph's table fields
      (WP_OP_SET_TABLE) and an empty paragraph put in or taken out
      (WP_OP_INSERT_PARA, WP_OP_REMOVE_PARA). The ruler bar narrows its
      gaps rather than drop a button where they would not fit (the ST).
      Seen on DOS: a table inserted with Alt+T, typed across with Tab,
      Shift+Tab back.
      [x] Stage 3 (2026-11-10): format_prn lays a row out side by side,
      each cell at its width into its columns of shared line buffers,
      with rules at the cell edges and rule lines above each row and
      below the table (box drawing in code page 437, else + - |); a row
      that does not fit starts a page. Spacing inside cells, shading and
      cells past 64 lines are left out (docs/formats/prn.md).
- [x] T16.3 Frames. A text box placed at a fixed spot on the page --
      a form's labelled fields, a sidebar -- with T16.1's borders. First
      cut: frames sit at their page position and the body text does not
      flow around them (wrap-around is a later task, noted here).
      RTF: frame paragraphs (\posxN \posyN \absw \absh \phpg \pvpg).
      AC: host tests for placement and RTF round-trip; a two-column
      character-sheet header made of frames, on screen and printed.
      Done 2026-11-10: test_table.c's frame cases (placement, printed
      positions, RTF round trip, boxes kept per frame, a scrolled view);
      the header shown and printed on the ST under SpeedoGDOS. Still to
      come (docs/document-model.md, "Frames"): wrap-around, clicks in a
      frame, a Format command to make one, the set height (\absh) in
      layout, frames placed on character printers.
- [x] T16.4 Vector drawings. Scalable art kept in the document as a list
      of primitives in twips -- line, polyline, filled polygon,
      rectangle, rounded rectangle, ellipse, arc -- with a line width and
      a fill; drawn on screen and printed at the printer's own
      resolution, so it stays sharp on paper. Saved in RTF as a Windows
      metafile picture (\pict\wmetafile8), which Word and other word
      processors read; read back from the same.
      platform.h gains drawing calls (polyline, filled polygon), in
      retroplat for every backend: ST VDI (thick lines drawn as polygons
      -- SpeedoGDOS faults on wide v_pline), Amiga graphics.library, Mac
      QuickDraw, host stub recording; DOS shows a labelled placeholder,
      as it does for pictures.
      Getting art in: the gateway converts SVG (and WMF/EMF) into the
      primitive list (offload capability, as images are); the ST reads
      GEM metafiles (.GEM, GEM Draw's format) natively, and its GDOS
      printer plays them. A drawing editor is out of scope here.
      AC: host tests for the primitive list, scaling and WMF round-trip;
      an SVG heading inserted through the gateway, shown on ST, Amiga
      and Mac, printed through GDOS and PostScript.
      Progress 2026-11-10: stage 1 (model, metafiles, RTF) and stage 2
      (drawing on screens and paper) done -- retroplat's
      plat_fill_polygon / plat_draw_polyline on every backend, wide
      lines stroked as polygons in the engine; shown on the ST and Amiga,
      "[drawing]" on DOS; printed through SpeedoGDOS (FX80, polygons by
      spans: its v_fillarea crashes) and NVDI (PCL). The Mac builds but
      is not yet seen. Stage 3 done the same day: Insert Image reads GEM
      and Windows metafiles on every machine (engine/src/drawfile.c,
      tested on Kandinsky's tiger, 747 and Columbia), sends SVG to the
      gateway (OFFLOAD_DRAWING, svg_drawing.py), and PostScript prints
      drawings as paths. The heading through the gateway on screen and
      paper: the host test drives the real gateway; the ST printed the
      tiger and the 747 through SpeedoGDOS.
- [x] T16.5 Clipart. Bitmap clipart opened directly on each machine, no
      gateway needed: GEM .IMG (ST), IFF ILBM (Amiga), MacPaint/PICT
      (Mac), PCX/BMP (DOS), converted to the model's indexed image. A
      small bundled set of our own chapter headings, borders and
      flourishes (vector, T16.4) under clipart/, and Insert Clipart,
      which browses it.
      AC: each target opens its native format; the bundled set inserts
      and prints.
      Progress 2026-11-10: stage 1 done -- the readers (engine/src/
      bitmapfile.c: IMG/XIMG, ILBM/PBM, MacPaint, PCX, BMP, every
      machine reads every format), Insert Image using them, a shared
      picture cache (imgcache.c) that also lets the Amiga and Mac keep
      an RTF's pictures. Tested on BLOWUP's and Calamus's IMGs, a CT60
      XIMG, an ILBM game screen, Calamus's BMP, cc65's PCX; an RTF of
      two shown on the Amiga. DOS keeps bitmaps on the gateway (its
      64 KB blocks; retroplat's mem_alloc now refuses more rather than
      corrupting). Stage 2 done the next day: 24 pieces of our own
      (clipart/, drawn by clipart/tools/make_clipart.py, as metafiles
      every machine reads) and Tools > Insert Clipart, category then
      piece; a ruler bar star on the Atari, Alt+K on DOS.
- [x] T16.6 The worked example: an original character sheet (stat grid,
      skill table, boxed notes, ruled write-in lines, decorated title)
      kept as a fixture, opened, screenshotted and printed on every
      target as part of the gates -- the milestone's acceptance test.
      Done 2026-11-11: host/tests/fixtures/CHARSHT.RTF (made by
      make_charsheet.py: a clipart banner with the title set over it in
      a frame, an identity table, attributes and skills side by side in
      one table with a shaded head row, a boxed equipment section, ruled
      write-in lines as a one-column table, a closing flourish).
      host/tests/test_charsheet.c checks what it holds, that it saves and
      reads back the same, prints on one page and to PostScript;
      host/tools/ci_charsheet.sh opens it on the Atari, the Amiga, DOS
      and the Mac (through its new Apple Event handler -- docs/
      mac-platform.md), checks each says it holds the same (a summary
      line in its CI marker, platform/common/cisummary.c), keeps a
      screenshot of each in build/charsheet/, and prints it on the
      Atari. Printed by hand
      through SpeedoGDOS (FX80) and NVDI (PCL), matching the screen.
      Found and fixed on the way: RTF import gave a picture the default
      paragraph style (a centred banner went to the margin) and made an
      empty paragraph of the \par after it (one more on every round
      trip); typographic quotes, dashes and accented letters printed as
      "?" on machines whose sets lack them (retroplat's
      utf8_ascii_fallback now gives the nearest plain character).
      Still open: the character printers draw a partly bordered table
      cell as a full box. (PostScript went through format_prn's
      character grid until 2026-11-11; it now draws the page printer's
      page -- platform/common/psdev.c, docs/formats/prn.md.)

Artifact: a character sheet made in RetroWP, saved as RTF that Word
opens, printed through GDOS, PostScript and PCL, matching the screen.

---

## M17 — Atari Works Documents
Goal: open the word-processor documents of Atari Works (1992), the
office suite Atari shipped for the ST, TT and Falcon and the usability
bar this project set itself -- documents that exist today and could not
be opened at all. Chosen with the user (2026-11-11). The specimen
corpus (Atari's own legal department: 263 documents present as .STW,
.RTF and .TXT, docs/formats/atariworks.md) is the measure: the
importer is scored against it, not against a model of the format.

- [x] T17.1 Map the format and read text, styles and page.
      AC: the map in docs/formats/atariworks.md, read off the corpus
      with a research walker (host/tools/stw/probe.py) before any C:
      paragraph records one a paragraph (their tab and merge-field
      counts carried in the record before), placed objects (GEM
      metafiles and IMGs with a 32-bit length, boxes and frames
      header-only), the body (Atari set, 8-byte style changes), the
      font table; page reals as IEEE doubles (version 11) or singles of
      four times the inches (version 10). engine/src/format_stw.c reads
      text, runs (face, size, bold, underline, italic from the face's
      name), paragraphs (alignment, indents, line spacing, tab stops
      with their kinds) and the page; a merge field reads as
      "LABELS.STD:Company". `make stw-score` runs RetroWP's own importer
      over the corpus: **256 of 262 texts identical word for word** to
      Atari Works' .TXT (the six: two .TXT wraps inside merge fields, a
      wrapped underline, two words around a tab, two .TXTs of a
      different revision); styles against Atari Works' .RTF over
      354,049 characters: **alignment, underline and size 100%**, bold
      96.5% and italic 99.0% (the RTF export names every face "System
      Font", so a bold or italic *face* reads plain there). All 47
      Legal/STWorks files walk clean. Open finds Atari Works by content
      (its "10"/"11" is not distinctive alone; the whole structure must
      walk); RetroWP does not write .STW, so LETTER.STW saves as
      LETTER.RTF beside it, never over it. Found on the way: RetroWP's
      RTF reader refused `{\rtf0` -- Atari Works' and other early
      writers' -- and now takes it. The engine gained the Atari ST
      character set (WP_CODEPAGE_ATARI_ST). Seen on the ST and the Mac.
- [x] T17.2 Pictures: the placed objects, at their places, as frames.
      AC: an object's header gives x, y (32 bits, down the whole
      document), width and height; y is in the line table's measure,
      whose page marks (bit 0 of a line's flags) number the pages and
      whose heights total the info block's word 12 in all 265 files.
      Each object follows the last paragraph to end above it, in a
      paragraph-relative frame that far below (from its page's top
      margin where that paragraph began a page earlier), so pictures
      stay by their text on every machine. GEM metafiles go through
      drawfile.c; boxes, rules and shapes Atari Works drew become
      drawings. Found on the way: drawfile.c refused a metafile whose
      coordinate system is one line high -- Atari Works' signature
      lines -- and now reads it as a rule. Seen through the PostScript
      page writer (`stw2txt --ps`) and on the ST and the Mac. Not
      read: IMG pixels (in a file of their own), and text inside a
      metafile -- many letterheads and headlines are GEM text, so they
      show bars without words (T17.4).
- [x] T17.4 Text in drawings.
      AC: a TEXT record in the drawing model (point, height, angle,
      alignment, style, family, colour, UTF-8); read from GEM metafiles
      (v_gtext, v_ftext, v_justified, with vst_point through the
      header's page, vst_rotation, vst_effects, vst_color,
      vst_alignment) and from Windows metafiles (TextOut, ExtTextOut,
      in the font CreateFontIndirect made), written to WMF so RTF keeps
      it; drawn by the platform's face at the picture's scale (a
      quarter-turned line as upright letters along it) and by the
      PostScript writer turned properly. AFFIDAVT's letterhead and
      OFFER's game list read on the ST, the Mac and in PostScript.
      Found on the way: the one-bit Mac drew every text colour black
      (retroplat now draws a light one white, srcBic), and the WMF
      reader gave a CreateFontIndirect too short to read no object slot,
      which would have misnumbered every object after it. Tests: TEXT's
      rendering, its WMF round trip, GEM text with every attribute; each
      shown to fail with its reader broken.
- [x] T17.3 Headers and footers.
      AC: they were what the map had taken for two default faces: the
      header's 72 bytes of text at 0x52 and its face at 0x9a, the
      footer's at 0xc8 and 0x110 -- present exactly where the .RTF has
      \header or \footer, in all 263. Their #C and #R (centre and
      right parts) and #P (the page number) become RetroWP's '|' parts
      and '#'; #D, the date, has no counterpart and is left out. JAG_LICS
      prints "Jaguar Licensees List ... Page 1 of 10" and its
      "Confidential" footer. (The info block's remaining words are still
      unread; nothing in the corpus has needed them.)

M17 done: every Atari Works word-processor document in the corpus
opens, its text identical to Atari Works' own export in 256 of 262 (the
rest differences in the exports), its styles, page, pictures, the
lettering inside them, and its header and footer.

---

## M18 — Mail Merge
Goal: letters, labels and forms filled from a list -- what Atari Works'
word processor did with its database, and the first of the
word-processing fundamentals still missing (styles, footnotes, columns
to follow). Chosen 2026-11-12 (the user's "continue" after M17; mail
merge picked because Atari Works documents already carry merge fields).

- [x] T18.1 Fields and the merge.
      AC: a field is text, «Name» (merge.h); Tools > Insert Field types
      one; RTF writes Word's MERGEFIELD and reads it back as the same
      text; Atari Works' fields import as «Company» (stw-score
      normalises the .TXT's LABELS.STD:Company to compare: still 256 of
      262). The data is a CSV/semicolon/tab text file (quotes, line
      breaks in quotes, CRLF, a byte-order mark; UTF-8 or the machine's
      own set); File > Mail Merge merges in place -- the template's
      sections once a record, a new page each, everything else shared --
      and hands the result over as a new unsaved document. Every machine
      asks through its own picker (`wp_app_ctx.pick_path`); the ST's GEM
      menu gained File > Mail Merge... and Tools > Insert Field... (two
      objects appended, its self-check extended). The host backend
      gained plat_native_to_utf8 (Latin-1), which it lacked. Tests: the
      field finder, CSV and tab data, the merge, RTF fields, the action
      end to end; case-insensitive names shown to fail when broken.
- [x] T18.2 Labels.
      AC: File > Mail Merge asks Letters or Labels; for labels "across x
      down" (3x8 offered) in the status line. The label is the first
      section's paragraphs; the result a table of them, each row
      exactly the text height over `down` -- a new row height,
      `wp_table_row.height` / RTF `\trrh` (at least, or exactly) --
      each label the column over `across`, the last row filled out.
      Found on the way: the line walker every painter and the printer
      share, and `wp_table_row_extent`, sized a row by its cells' text
      only, so a row's height reached the layout but not the page; both
      take it now. Seen through the PostScript writer: five records two
      across, a label's pitch apart. Tests: the table's shape, the
      layout, the walk and the extent at the pitch (shown to fail with
      the walker unfixed), \trrh through RTF, the action end to end.
- [x] T18.3 Merging straight to the printer, a record at a time.
      AC: Mail Merge's third choice, "Letters, printed one at a time":
      the data read, then File > Print's own choices once and a copy a
      record -- the record filled into the document and taken out again
      (`wp_merge_apply` / `wp_merge_restore`, the string pool wound back
      by the new `wp_strpool_truncate`), so neither the merged document
      nor the pool grows with the list. A character printer takes the
      copies one after another down the open port; PostScript one job
      (the writer split into begin / document / end, pages numbered
      through the job); GDOS and the gateway a job a copy. Tests: apply
      and restore (the pool's length exactly as before -- shown to fail
      without the rewind), the action to a character printer and to
      PostScript (one job, two pages, the template on screen after). Not
      exercised on the host: the GDOS path (it needs the ST's
      workstation), which takes the same loop.

M18 done: letters, labels and long lists, from any CSV. (Found in M19:
DOS has no menu bar, so Mail Merge and Insert Field were unreachable
there until Alt+M and Alt+I.)

---

## M19 — Paragraph Styles, Footnotes, Columns
Goal: the word-processing fundamentals still missing, worked through in
order (the user, 2026-11-12: "paragraph styles, then just work through
the list").

- [x] T19.1 Named paragraph styles.
      AC: a stylesheet in the document (name, paragraph format,
      character format); a paragraph is in a style by its format's
      style_id (the unused style_name field, repurposed), so layout is
      untouched and direct formatting keeps the style. RetroWP's own six
      (Normal, Title, Heading 1-3, Quotation) come in on first use, so
      an unstyled document saves byte for byte as before. Format > Style
      and Format > Define Style (redefining restyles every paragraph in
      it); Enter at the end of a heading gives Normal; an empty styled
      paragraph types in its style. RTF writes \stylesheet and \sN and
      reads Word's (\cs character styles passed over). The ST's Format
      menu gained both items (appended, self-check and leaf count
      updated); DOS reaches them by Alt+S and Alt+Y, and -- found doing
      this -- Mail Merge and Insert Field by Alt+M and Alt+I, which DOS
      had lacked. Tests: built-ins, define and redefine, next style and
      typing, RTF both ways and Word's stylesheet, the actions; next
      style and \sN import shown to fail when broken.
- [x] T19.2 Footnotes.
      AC: a reference is a raised run naming its note (the char
      format's spare field); a note is paragraphs naming it, kept after
      the paragraph that refers to it and laid out as frame lines at
      the foot of that page, its height kept clear of text -- so every
      painter and the printer draw notes with no code of their own.
      Notes number by where their references stand, renumbered before
      every layout; an orphaned note shows "*". Tools > Insert Footnote
      (journaled: Undo takes it back) moves the caret into the note;
      typing beside a reference stays plain. Superscript and subscript
      now draw at two thirds size. RTF writes Word's \chftn/\footnote
      and reads it. ST Tools menu item appended (self-check and leaf
      count updated); DOS Alt+N. Tests: insert, type, undo; renumbering
      and orphans; page foot on one page and two; RTF both ways and
      Word's form; typing beside a reference. Each shown to fail when
      its mechanism is broken.
- [x] T19.3 Page View, with a layout grid.
      Why: the screen shows the text as one continuous scroll with page
      breaks ruled across it, which cannot show where things sit on the
      page -- footnotes drew over later pages until they were moved into
      the flow (e58bf73), and columns side by side are impossible in it.
      NeXT's WYSIWYG came from drawing the screen with the printer's own
      imaging model (Display PostScript); RetroWP takes the principle, not
      the interpreter (the user, 2026-11-12): the screen becomes one more
      device for the page printer (pageprint through devout), so what is
      on screen is what prints because the same code draws both. GUI ports
      only; DOS keeps its text view.
      Parts:
      a. platform/common: page geometry on a scrolling canvas (pages
         stacked with a gap, centred), zoom (fit width, 100%, fit page),
         pixel scrolling; pageprint split so one page draws at any zoom,
         each word placed at its laid-out position (a font opened at the
         zoomed size, its width scaled back); hit-testing and the caret's
         place in page coordinates; selection drawn over the page.
      b. The grid (the user, 2026-11-12: DTP belongs in the word
         processor now): View > Grid -- show it, its spacing, snap to it;
         drawn on the page in page view; frames and pictures moved by the
         mouse snap to it.
      c. A View menu on every GUI port (Page View, Zoom, Grid); the ST's
         hand-built tree gains the title in order, self-check updated.
      d. Each shell -- ST (and gem4xe), Amiga, Mac -- paints, clicks,
         scrolls and follows the caret in page view.
      AC: page view shows each page whole at its size and zoom, margins,
      header, footer, footnotes at the foot, frames where they print;
      clicking and typing work in it; the grid draws and snaps; every gate
      green; screenshots on each GUI machine checked by eye.
      Done: as above; printing proven byte-identical across the page
      printer's refactor; greeking where a screen has no face small
      enough (the ST at fit-width); the ST window gained a horizontal
      slider, the Amiga and Mac follow the caret (they have no scroll
      bars); View, zoom and grid saved in the config file. Snapping
      applies to the ruler's markers -- nothing drags frames with the
      mouse yet. Tests: fit and stacking, words drawn where laid out,
      click and caret agreeing, a note at the paper's foot, following
      the caret, selection strips, snapping, the view's clip, the config
      keys; each shown to fail when broken.
- [x] T19.4 Columns (in page view, side by side; DOS one after another).
      AC: a section in one to four columns (wp_section.columns,
      col_gap); text wraps to a column; on paper and in page view a full
      column goes on at the top of the next, then a new page, a page
      break always a new page; footnotes at the foot of their column;
      the plain view and DOS show columns one after another. Format >
      Columns (ST menu item appended, self-check and leaf count; DOS
      Alt+L). RTF \cols and \colsx both ways. Tests: flow and the
      printing walk's column restart, page view caret, click and
      drawing in column two, a note in column two, a page break from
      it, RTF; each shown to fail when broken. Not yet: balancing at a
      section's end, a column break, the ruler showing every column.

- [x] T19.5 Paper metrics.
      Found checking columns on target: the ST and Amiga measured with
      screen fonts sized to the system cell, so pages held too much and
      the ST's PostScript printed small. Layout now measures as it will
      print on every graphics screen: retroplat papermetrics (Adobe Core
      14 widths for Times, Helvetica, Courier x 4 styles, 1.2 em lines,
      generated from the URW clones), each backend answering from it
      when plat_metrics_paper(); config metrics=paper|screen (paper).
      Page view greeks by the drawing face's own size. On target: the
      ST and Amiga page views match the Mac's, and the ST's PostScript
      prints true 10-point Times in two columns. Tests: published widths,
      faces by name and family, Windows-1252 and missing characters,
      line height, layout under the mode, the config key; the backend
      hook shown to fail when broken.
- [x] T19.6 Paper sizes, envelopes, disc labels.
      AC: Page Setup offers 21 named sizes (engine paper.h) -- sheets
      (Letter, Legal, Executive, Half Letter, Tabloid, A3-A6, B5),
      envelopes (#10, DL, C5, C6, Monarch), index cards, a CD/DVD label
      (the disc's 120 mm square), CD booklet and tray card -- turned
      portrait or landscape (an envelope offers landscape first), or a
      size typed in inches, cm or mm (now converted exactly); margins
      that leave no room shrink to half an inch, a disc's to a
      centimetre. Page view draws a disc's edge and hole. Tools >
      Envelope (ST menu item appended; DOS Alt+E): the selected address
      offered, a return address remembered in the config file, the
      envelope added as the document's first section, landscape, the
      address block where a postal service reads it, one undo step --
      and undoing a section split now gives the joined section its own
      page setup back (it took the first half's). PostScript already
      names its page size to the printer. Tests: the size lists and
      turns, custom sizes, the envelope and its undo, disc guides,
      matching sizes either way round; split-undo shown to fail when
      broken. Next: printing to the Canon's disc tray through the
      gateway once its CUPS queue is set up (its InputSlot and media),
      and label-sheet templates.
- [x] T19.7 UI scale, colour toolbar, Amiga polish.
      AC: the toolbar, ruler and status line at 1x, 2x or 3x -- config
      uiscale, auto from the screen's width (2 from 1200 pixels, 3 from
      1800): icons pixel-doubled, the ruler's marks and grab tolerance
      scaled. On 16 colours and more the icons are coloured by rule --
      closed shapes filled (their inside found by a flood fill), some
      inked -- seen on a TT in TT-medium. The Amiga: white paper (the
      backend's white is the Workbench's SHINEPEN, not its grey
      background, and its four pens are chosen by brightness), outline
      faces shaped for the screen's pixels (TA_DeviceDPI: they came out
      half as wide on 640x256), the status line in the screen's own font
      size, the toolbar's raised buttons in the Workbench's pens (a new
      bevel option), and nothing drawn past the status line (MUI gives
      Draw no clip). Found along the way: a drawing's text placed by
      paper metrics but drawn in a screen face ran its words together or
      apart -- it is measured in its own face again. Tests: the scale
      choice, the panel's icon count with colour, drawing text under
      paper metrics; the last shown to fail when broken.
- [x] T19.8 Label sheets.
      AC: Mail Merge's Labels offers named sheets -- Avery Letter
      (5160, 5161, 5162, 5163, 5164, 5167, 5371) and A4 (L7160, L7161,
      L7162, L7163, L7165, L7173, L7651) -- from engine labels.h, each
      the maker's figures (US in exact thirty-seconds of an inch); the
      section takes the sheet's paper and margins, the table's cells a
      pitch apart from the first label, each label's text a twelfth of
      an inch in from its own edges (the gap between labels split
      either side of a cell's boundary). "Across x down" stays for
      sheets not listed. A document with no fields makes a sheet of
      copies of its text (return addresses). Tests: every sheet inside
      its paper with even margins and an exact pitch once applied; 5160
      merged -- paper, margins, cells, gap, 30 to a page and the 31st
      on the next; a sheet of copies on L7163; the first two shown to
      fail when the gap is broken.
- [x] T19.9 Undo for page setup; column breaks.
      Found: Redo after Tools > Envelope brought back the break but not
      the envelope's page or its address block -- page setup and
      paragraph styles were set outside the journal. Now journaled:
      wp_edit_set_section (WP_OP_SET_SECTION, the setup before and
      after) and wp_edit_set_pstyle (WP_OP_SET_PSTYLE, defined since M0
      and never used); the envelope, Page Setup's paper and margins,
      Format > Columns and the header and footer go through them, each
      one undo step. Column break: page_break_before WP_BREAK_COLUMN,
      the paragraph starting the next column (past the last, or with
      one, a new page; the character printer, whose columns run one
      after another, ignores it); Format > Columns' "Break to next
      column" at the caret (mid-paragraph, the part after it), no menu
      tree change; RTF writes Word's \column and reads it, inside a
      paragraph too. Balancing columns at a section's end is not done:
      every section here starts a page, and Word balances only above a
      continuous section break. Tests: envelope redo and undo, Page
      Setup undo and redo, the break in two columns and one, RTF both
      ways and Word's form, the action and its undo; envelope redo and
      the column break shown to fail when broken.
      Page view's baseline: a run is placed by the ascent of the face
      the layout measured it in, not the drawing face's, which is bigger
      where a screen has no face as small as the zoom asks and hung the
      text low (retroplat's host backend gained a smallest-face knob to
      show it; the test fails without the fix). On the ST's character
      sheet at fit width the two render alike: there the table labels
      fill their rows because the ST's smallest face is nearly a row
      tall -- its size, not its place.
- [x] T19.10 Scroll bars on the Mac and the Amiga.
      Neither had any, in either view: both followed the caret only. Mac:
      the Toolbox's scroll bar controls along the window's right and
      bottom edges, the content rect less their strips; the arrows and the
      grey repeat while held (TrackControl with an action proc), the thumb
      goes where it is let go; a control's 16 bits count in units of a
      power of two of pixels when a canvas is taller. The size box works
      now (GrowWindow/SizeWindow, the bars moved, the window relaid). The
      window opens to the screen's width -- the fixed (40,60)-(520,340)
      ran 8 pixels off a 512-pixel screen, where the vertical bar lives --
      its top kept at 60, the title bar the Mac gates click to focus it.
      Amiga: MUI Scrollbars beside and below the editor, notifying the
      application on MUIA_Prop_First; the loop sets them back with
      MUIA_NoNotify so following the caret is not taken for a hand scroll
      (DeltaFactor a line of pixels in page view). The bars are set from
      the key handler after its paint, and from the loop after every pass,
      each following the caret first as the paint does: set only from the
      loop, a key's bar lagged one keystroke. Asking for them from inside
      Draw with MUIM_Application_PushMethod was tried and dropped (a
      paint looked stalled; later seen to be the A500's slow paint, but
      a Draw that sets nothing outside its object is the safer rule).
      Verified under BasiliskII by clicking every part of both bars in
      both views (an xdotool click is too short for TrackControl to see
      the button down: hold it 0.2 s), and under amiberry, which takes no
      clicks, by the knob following G in both views.
- [x] T19.11 One baseline a line; words held to their width.
      Every painter put each run's baseline its own ascent below the
      line's top, so 12 pt beside 36 pt hung from the line's top instead
      of standing on the big word's baseline. The layout now keeps a
      line's tallest ascent (wp_line.text_ascent_twips) and its deepest
      descent-plus-leading apart, the line as tall as the two together,
      and wp_run_baseline_twips answers where any run's baseline goes;
      the Atari, Amiga and Mac views and pageprint (page view and paper)
      all draw on it. Raised and lowered text, which had only looked
      raised because a smaller face hung from the top, now moves off the
      baseline by half / a third of its own ascent, and the layout makes
      room for it (a subscript deepens its line; a superscript beside
      text its own size costs nothing). Words held to their width: with
      paper metrics on (the default) a word's room is its printed width,
      and the Mac's Geneva is wider than Helvetica's published widths, so
      every word ran into the space after it ("plainline", the character
      sheet's "ATTRIBUTESANDSKILLS"). A word whose screen face overruns
      its room is now set a character at a time inside it
      (wp_dev_text_fit, devout.h), in the three normal views and on a
      screen's page view; one that fits is drawn whole, and paper is
      untouched. Also: the Mac, DOS and Atari
      gates' `xdotool search --name -i` -- this xdotool has no -i (its
      search ignores case anyway) and 2>/dev/null hid the error, so the
      window focus never ran; the gates passed on their title-bar click.
- [x] T19.12 Fitted words in pieces, not characters.
      Timed on an A500 (amiberry, a G/gg repaint of a long Times
      document, polled until the screen settled; four each): 16.6 s
      before T19.11, 18.8 s drawing an overrunning word a character at a
      time -- for the one word a line ("document") that overran there.
      A draw call is what the slow machines pay for, so a word is now
      drawn in as many pieces as it overruns by pixels, plus one, each
      joint taking up about a pixel: 17.2 s, inside the run-to-run spread
      of about 1.5 s, and the gaps between words kept. A word far too wide
      (the ST's banner at fit width) still comes out a character a piece.
---

## Cross-Cutting "Done" Checklist (apply to every task)
- [ ] Builds on host (+ target backend or stub).
- [ ] Platform-free logic unit-tested on host.
- [ ] No platform headers included under engine/ or services/.
- [ ] Degrades or tiers correctly at 1MB.
- [ ] Persistence/protocol uses explicit big-endian I/O.
- [ ] Public header documented; minimal surface.

