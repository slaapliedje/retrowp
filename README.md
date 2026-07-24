# RetroWP

An open-source, portable word processor for vintage platforms, built around
a single, platform-independent document model written in strict, portable
C89. One engine, one document model, four real vintage backends — no
per-platform forks of the logic, ever.

**Primary target:** Atari ST (1MB, plain TOS).
**Also runs on:** Amiga (MUI), Mac Classic (Toolbox/QuickDraw), and DOS
(text-mode, Open Watcom).

A companion "offload" service (Raspberry Pi + local LLM, or any modern
machine) handles heavy features — grammar checking, large-dictionary
spelling, thesaurus, ODT/PDF conversion, image downconversion — over a
serial/TCP link, so the low-RAM machines stay lean and every feature still
works standalone when nothing is attached.

## Screenshots

Real emulator captures of the actual `RETROWP` binary — not a mockup, not
the smoke-test harness — on all four shipping backends:

| Atari ST (Hatari) | Amiga (amiberry) |
| --- | --- |
| ![Atari ST](docs/screenshots/atari.png) | ![Amiga](docs/screenshots/amiga.png) |

| Mac Classic (BasiliskII) | DOS (DOSBox) |
| --- | --- |
| ![Mac Classic](docs/screenshots/mac.png) | ![DOS](docs/screenshots/dos.png) |

## Why

Every vintage word processor for these machines was written from scratch,
once, for that machine. RetroWP instead has exactly one document model,
one editing engine, one layout engine, one set of format importers/
exporters — and four thin platform backends that each implement a small,
frozen `platform.h` seam (fonts, drawing, files, clipboard, timer,
transport). Porting to a new machine means writing that seam, not
re-deriving word wrap.

## Prime Directives

These rules (from `CLAUDE.md`) are non-negotiable and enforced by the
build itself, not just convention:

1. **The engine contains zero platform calls.** No GEM, no MUI, no
   Toolbox, no DOS. It talks to the outside world only through
   `platform.h`.
2. **The internal document model is the single source of truth.** Every
   format (plaintext, RTF, ODT, PDF) is an importer/exporter around it.
3. **Never dump raw structs to disk or the wire.** All persistence and
   protocol messages use explicit, defined byte order (big-endian).
4. **Font metrics are reached only through the metrics abstraction.**
   Layout code never calls a platform font API directly.
5. **The 1MB Atari ST plain-TOS build is the baseline.** A feature that
   can't degrade gracefully at 1MB is tiered (RAM-gated) or offloaded,
   never assumed.
6. **Everything in the engine builds and unit-tests on a modern host**
   with a stub platform backend. Off-target testing is first-class.

`platform/common/` (shared UI scaffolding — action registry, app context,
status line, help viewer) is compiled by *every* target's own toolchain,
including the host. A stray native `#include` in that directory breaks
the host build immediately, since no such header exists there — the rule
is structural, not just documented.

## Status

Every milestone through **M15 (image support)** is complete: document
model, editing, layout/pagination, all four platform backends, five
switchable keybinding schemes (vi, Emacs, WordStar, WordPerfect, MS
Word–style), find & replace, RTF/ODT/PDF/TXT import-export, offload
transport with grammar/spell/thesaurus/chat/image ops, and real (or
honestly placeholder) image rendering on every backend. See
[`docs/milestones.md`](docs/milestones.md) for the full history.

Open: `T13.6`, a fifth backend for the Apple IIGS (65816 / GS/OS,
a genuinely new CPU architecture for this project).

| Backend | Status | Rendering |
| --- | --- | --- |
| Atari ST | ✅ | Real VDI text + indexed-bitmap blit |
| Amiga (MUI) | ✅ | Real graphics.library text + indexed-bitmap blit |
| Mac Classic | ✅ | Real QuickDraw text + dithered 1-bit blit |
| DOS (text-mode) | ✅ | Real B800 text; images shown as a bracketed marker (no addressable graphics exist in text mode) |
| Apple IIGS | 🚧 planned | — |

## Repository Layout

```
engine/     Portable C89. Document model, editing, layout, formats,
            offload client. Zero platform calls.
platform/
  common/   Shared UI scaffolding (actions, status line, help). May call
            platform.h; may never #include a native header.
  atari/    Atari ST backend (GEM/VDI, m68k-atari-mint-gcc)
  amiga/    Amiga backend (MUI, m68k-amigaos-gcc)
  mac/      Mac Classic backend (Toolbox/QuickDraw, Retro68)
  dos/      DOS backend (text-mode, Open Watcom)
services/   Standalone algorithmic modules (spell, grammar) -- pure C,
            data-driven, no UI, no platform calls. Shared everywhere.
offload/    The gateway (Pi or any modern machine) + the wire protocol
            shared with the engine's own offload client.
host/       Test harness, stub backend, and per-platform CI tooling for
            building and testing everything on a modern PC.
docs/       Design docs, per-format notes, per-platform bring-up notes,
            and the full milestone log.
```

## Building & Testing

### Host (portable engine + tests)

```sh
make test
```

Builds the engine, `platform/host` stub backend, and the full test suite
against strict C89 flags, then runs it. No cross-compiler needed for this.

### Atari ST

Requires `m68k-atari-mint-gcc` on `PATH` (override with `ATARICC`).

```sh
make atari                 # -> bin/atari/RETROWP.PRG
make atari-smoke           # -> bin/atari/SMOKE.PRG
host/tools/ci_atari.sh     # boots it under Hatari + EmuTOS, checks PASS/FAIL
host/tools/ci_atari_matrix.sh   # same, across 6 real TOS ROM versions
```

### Amiga

Requires `m68k-amigaos-gcc` under `$AMIGA_TOOLROOT` (default `~/opt/amiga`).

```sh
make amiga-app              # -> bin/amiga/RETROWP
make amiga-smoke            # -> bin/amiga/AMISMOKE
host/tools/ci_amiga.sh
host/tools/ci_amiga_matrix.sh    # across 3 real Kickstart versions
```

### Mac Classic

Requires a [Retro68](https://github.com/autc04/Retro68) toolchain and
CMake; see `host/tools/mac/CMakeLists.txt`.

```sh
make build/generated/help_retrowp.h
cmake -S host/tools/mac -B build/mac \
  -DCMAKE_TOOLCHAIN_FILE=<retro68-build>/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake \
  -G "Unix Makefiles"
cmake --build build/mac        # -> build/mac/RETROWP.APPL
host/tools/ci_mac.sh
```

### DOS

Requires Open Watcom under `$DOS_TOOLROOT` (default `~/opt/watcom`).

```sh
make dos                    # -> bin/dos/RETROWP.EXE
make dos-smoke               # -> bin/dos/SMOKE.EXE
host/tools/ci_dos.sh
host/tools/ci_dos_matrix.sh      # across cga/ega/svga_s3 profiles
```

### Offload gateway

```sh
python3 offload/gateway/odt_gateway.py     # ODT/PDF/font conversion
python3 offload/gateway/image_gateway.py   # Pillow-backed image downconvert
```

`offload/gateway/mock_gateway.c` provides a deterministic, dependency-free
fixture gateway used by the host test suite and every backend's own
transport selfcheck.

## Documentation

- [`docs/document-model.md`](docs/document-model.md) — the single source
  of truth: types, side tables, the image extension.
- [`docs/layout.md`](docs/layout.md) — word wrap, pagination, paragraph
  kinds.
- [`docs/editing.md`](docs/editing.md), [`docs/vimode.md`](docs/vimode.md),
  [`docs/keymaps.md`](docs/keymaps.md) — editing model and the five
  keybinding schemes.
- [`docs/formats/`](docs/formats) — RTF, ODT, TXT, PDF, and the offload
  wire protocol.
- [`docs/*-platform.md`](docs) — per-backend bring-up notes, real bugs
  found, and every emulator-specific gotcha.
- [`docs/milestones.md`](docs/milestones.md) — the complete task-by-task
  build history, M0 through M15.
