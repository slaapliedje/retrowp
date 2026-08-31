# CLAUDE.md — RetroWP

## What This Project Is
RetroWP: an open-source, portable word processor for vintage platforms, built
around a platform-independent core engine written in strict, portable C.
The name fits 8.3 filename limits on every vintage target (`RETROWP.PRG`,
`RETROWP.EXE`). Primary target:
Atari ST (1MB, plain TOS). Additional targets: Amiga (MUI), Mac Classic (Toolbox),
DOS (text-mode). A companion "offload" service (Raspberry Pi + local LLM) provides
heavy features (grammar, large-dictionary spell, thesaurus, ODT/PDF conversion)
over a serial/TCP link so low-RAM machines stay lean.

## Prime Directives (Non-Negotiable)
1. The ENGINE contains zero platform calls. No GEM, no MUI, no Toolbox, no DOS.
   It talks to the outside world ONLY through interfaces defined in `platform.h`.
2. The internal document model is the SINGLE SOURCE OF TRUTH. Every format
   (plaintext, RTF, ODT, PDF) is an importer/exporter around it.
3. Never dump raw structs to disk or the wire. All persistence and all protocol
   messages use explicit, defined byte order (big-endian on disk/wire).
4. Font metrics are reached ONLY through the metrics abstraction. Layout code
   must never call a platform font API directly.
5. The 1MB Atari ST plain-TOS build is the baseline. A feature that cannot degrade
   gracefully at 1MB must be tiered (RAM-gated) or offloaded, never assumed.
6. Everything in the engine must compile and unit-test on a modern host (PC/Mac)
   with a stub platform backend. Off-target testing is a first-class workflow.

## The platform layer lives in another repo now
`platform.h`, `types.h`, `endian.h`, `utf8.h`, `endian.c`, `utf8.c` and every per-machine
backend are **retroplat** (`../retroplat`), a standalone library RetroWP consumes. Only the
app shells — `platform/<target>/app_shell*.c` and `main.c` — are still here, plus
`platform/common/`.

Point the build elsewhere with `make RETROPLAT=/path/to/retroplat`. Header filenames are
unchanged by the move, so every `#include "platform.h"` still resolves.

**A backend fix belongs in retroplat, not here.** If you find yourself wanting to edit a
`metrics_*.c` or `draw_*.c`, it is in the other repo.

**docs/platform-extraction.md** is the record of how and why, including the coupling
analysis (what in `platform/` was genuinely app-independent and what was the word
processor) and the verification protocol the move was held to.

## Testing Baseline (know this before you refactor)
`make test` on a clean tree gives **435/438** where Pillow is not available to the offload
gateway, and a full sweep where it is. The three variable cases are all in
`host/tests/test_image_convert.c` — pre-existing and environment-dependent. Take your own
baseline before changing anything rather than trusting either number, or you will hunt a
regression you did not cause.

`make seam-check-all` is the other standing gate, and the one a refactor is most likely to
break without noticing. It delegates to retroplat's own `make check-all`, which compiles
each backend *without an application around it*, against nothing but the seam's four
headers — so a backend reaching back into this engine fails to compile. No build that
includes an application can catch that, because there the application's headers are already
on the path. Absent cross-toolchains skip, not fail.

Known broken here and NOT caused by anything in this repo: the Retro68 `ld` is linked
against Boost 1.91 while the system has 1.92, so the Mac CMake build fails at CMake's own
compiler test, before it reads a line of this project. `make seam-check-mac` still passes,
because compiling is fine and only linking is broken.

## Architecture Layers
- `engine/`   Portable C. Document model, editing, layout, formats, offload client.
- `platform/common/` Shared UI scaffolding (action registry, app context, status
              line, help viewer) used by every backend. MAY call `platform.h`
              functions; MUST NEVER `#include` a native header (`gem.h`,
              `exec/types.h`, `MacTypes.h`, `dos.h`, ...). Enforced by
              construction, not just convention: this directory is compiled by
              every target's own toolchain (host `cc`, the Atari m68k
              cross-compiler, and eventually Amiga/Mac/DOS cross-compilers) --
              a stray native `#include` breaks the host build immediately,
              since no such header exists there.
- `platform/<target>/` The application shell for one target: `app_shell*.c` and
              `main.c`, using that target's native headers freely. Owns the UI.
              The BACKEND it calls through `platform.h` is not here — it is
              `../retroplat/backends/<target>/`.
- `services/` Standalone algorithmic modules (spell, thesaurus, grammar-lite).
              Pure C, data-driven, no UI, no platform calls. Shared everywhere.
- `offload/`  The Pi-side gateway (modern C/Python). Mirrors the serial protocol.
- `host/`     Test harness + stub backend for building/testing on a modern PC.

## Language & Style
- C89/C90-compatible core (widest vintage compiler support: m68k GCC, SAS/C,
  Aztec C, Turbo C). No C99-isms in shared code unless guarded.
- No dynamic dependencies in the engine beyond a small allocator wrapper.
- Fixed-size integer types via a local `types.h` (don't assume int width).
- Every module has a header with a documented, minimal public surface.

## Byte-Order & Portability Rules
- Big-endian is canonical for files and protocol. Provide read/write helpers.
- x86 (DOS) build must byte-swap on I/O; engine logic stays endian-neutral.
- No pointer-size assumptions; no packing assumptions across struct writes.

## Memory Rules
- All engine allocation goes through `mem_alloc/mem_free` (platform-provided).
- Prefer streaming over full-buffer where a format allows (PDF write, RTF).
- Large data (thesaurus, big dictionaries) is queried from disk or offloaded,
  not loaded whole, in the 1MB tier.

## Offload Rules
- The engine must be fully functional WITHOUT an offload device attached.
- Offload features are enhancements, requested through one client interface.
- The protocol is versioned, framed, and CRC-checked. Timeouts never hang the UI.

## Definition of Done (per feature)
- Builds on host + at least the ST backend (or stub if backend not ready).
- Unit-tested on host where logic is platform-free.
- Degrades or tiers correctly at 1MB.
- Public header documented; no platform leakage into engine/.

