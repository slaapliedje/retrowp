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
- `platform/<target>/` One backend per target. Implements `platform.h`, plus
              that target's own native headers freely. Owns UI + rendering.
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

