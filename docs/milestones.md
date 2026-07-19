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

- [ ] T5.1 `format_rtf` import (classic RTF 1.x subset; documented supported
      control words).
      AC: Fixture RTFs import to expected model trees.
- [ ] T5.2 `format_rtf` export (round-trip of supported subset).
      AC: model→RTF→model preserves supported attributes.
- [ ] T5.3 `services/spell`: compact dictionary queried from disk + affix rules.
      AC: Known misspellings flagged; suggestions returned; peak RAM within 1MB
      budget (dictionary not fully loaded).

Artifact: Open/save RTF + local spell-check, still 1MB.

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
- [ ] T6.3 Atari RS232 transport hook (and MiNTnet TCP variant if present).
      AC: On hardware/emulator, handshake negotiates capabilities.
      Not started: no `platform/atari` backend exists yet (M4). The transport
      seam (`plat_transport_open/read/write/close` in platform.h) is already
      structured so this is a drop-in fifth implementation of those four
      functions, alongside `platform/host`'s `pipe:`/`tcp:` -- no change to
      `offload/transport`, `offload_client.c`, or `offload.h` will be needed
      when it lands.
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
**Partially reached, host-only:** offload transport + client + a mock gateway
round-trip THESAURUS and SPELL_CHECK over TCP loopback, with full fault-
injection coverage (corrupt CRC, dead gateway, delayed gateway, short/stalled
frame). T6.3 (real Atari serial transport) and a real Pi/LLM-backed gateway
are still open -- this was a deliberate scope choice (see CLAUDE.md and the
project's bootstrap plan) to prove the engine core + offload plumbing on host
before any platform-specific GUI work begins.

---

## M7 — Offloaded Formats (the 1MB payoff)
Goal: 1MB ST opens/saves ODT and rich PDF without local parsing.

- [ ] T7.1 Define CONVERT_IMPORT/EXPORT payloads (format id + bytes).
      AC: protocol.md documents the message shapes.
- [ ] T7.2 Gateway ODT import (ZIP/XML → internal model serialization) and
      export (model → ODT).
      AC: Round-trip an ODT through the gateway; model matches a host-side
      reference import.
- [ ] T7.3 Gateway PDF export (model → PDF bytes returned to client).
      AC: Produced PDF opens in a modern viewer; text/layout correct.
- [ ] T7.4 Engine-side wiring: "Open ODT" ships bytes, receives model.
      AC: On 1MB ST with Pi attached, open and save an ODT successfully.

Artifact: 1MB ST with ODT + rich PDF via the Pi.

---

## M8 — Grammar + Optional AI Ops
Goal: Full smart features, all optional/degradable.

- [ ] T8.1 Gateway grammar check (LLM or rule engine) → issue list payload.
      AC: Paragraph in, structured issues out; ST renders them.
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

