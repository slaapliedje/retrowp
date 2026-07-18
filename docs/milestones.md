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

- [ ] T3.1 Line breaker: greedy word wrap using platform.h metrics only.
      AC: With stub monospace metrics, break points match a hand-computed
      fixture exactly.
- [ ] T3.2 Pagination: fill lines into page boxes; page/section breaks.
      AC: Known document + page size yields expected page count and per-page
      line lists.
- [ ] T3.3 Layout cache invalidation on edit (relayout only affected range).
      AC: Editing paragraph K relayouts K (and following only if reflow needed);
      test asserts minimal recompute.

Artifact: Deterministic layout output verified in tests.

---

## M4 — First Real Backend: Atari ST (1MB, plain TOS)
Goal: A real, running ST word processor at 1MB.

- [ ] T4.1 `platform/atari` skeleton: AES app init, event loop, window, menu.
      AC: Opens a window on Hatari (plain TOS, 1MB config); clean quit.
- [ ] T4.2 VDI/SpeedoGDOS metrics + glyph draw implementation of platform.h.
      AC: Text renders; measured widths drive correct on-screen wrap matching
      the engine's layout.
- [ ] T4.3 File I/O, clipboard, timer hooks for Atari.
      AC: Open/save plaintext; GEM clipboard cut/paste works.
- [ ] T4.4 Memory budget pass at 1MB.
      AC: App loads, opens a multi-page doc, edits, saves — within 1MB TOS config
      on Hatari without exhaustion.

Artifact: Shippable ST word processor (plaintext + editing + on-screen layout).

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

