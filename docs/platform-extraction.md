# Extracting the platform layer into a shared library

**Status: a worked proposal, not a decision.** This refactor was carried out in full,
verified green against the baseline below, and then reverted at the author's direction.
Nothing in the tree today depends on it. **Do not execute it unprompted** — it is written
down so that if RetroWP ever wants to share its platform layer, the analysis and the
recipe already exist.

## What it is

Lift `platform.h` and its per-machine implementations out of RetroWP into a standalone
library that RetroWP and other vintage-targeting apps consume, so there is exactly one
copy of each backend rather than a fork per program.

The motivating case was a second project (GACS, a GURPS/Car Wars vehicle designer at
`~/dev/gacs`) that needed GEM, MUI, Toolbox and DOS backends and had no reason to write
them again. That project ultimately **vendored a copy** instead — see "Prior art" below.

## Why it is viable: the coupling is one-directional

`engine/` never includes a platform header — Prime Directive 1 guarantees it. The
converse is *not* enforced: `platform/` freely includes engine headers. On inspection
that turns out to be shallow, and it splits cleanly along a line that already exists.

Measured on the current tree:

| | files | lines | verdict |
|---|---|---|---|
| `platform/host/` | 10 | 630 | **all seam** — no app shell at all |
| `platform/atari/` seam files | 10 | 2,038 | **shareable** |
| `platform/amiga/` seam files | 9 | 1,260 | **shareable** |
| `platform/mac/` seam files | 10 | 1,162 | **shareable** |
| `platform/dos/` seam files | 10 | 975 | **shareable** |
| `engine/include/{platform,types,endian,utf8}.h` + `engine/src/{endian,utf8}.c` | 6 | 537 | **shareable** |
| `platform/*/app_shell*` + `main.c` | — | 11,926 | **stays** — this is the word processor |
| `platform/common/` | 8 | 2,796 | **stays** — action registry, keymap, help viewer, all built on `docmodel`/`editing`/`vimode` |

So roughly **6,600 lines are genuinely app-independent** and **14,700 lines are RetroWP**.
`platform/atari/app_shell.c` alone is 4,678 lines and is pure word processor.

The rule that falls out: *"how do I draw a rectangle on this machine"* is shareable,
*"what rectangles does my program want"* is not.

## Exactly one real leak, and it is a genuine bug — NOW FIXED

**Status update: this section's fix has been applied**, on its own merits and without
the extraction. `PLAT_STYLE_*` now lives in `platform.h`, the four `metrics_*.c` files no
longer include `docmodel.h`, and `make seam-check-all` enforces that nothing under
`platform/<target>/` outside the app shells includes an application header at all. The
account below is kept because it is the reasoning, not just the change.

`metrics_atari.c`, `metrics_amiga.c`, `metrics_mac.c` and `metrics_dos.c` include
`docmodel.h` solely to read four constants: `WP_STYLE_BOLD`, `_ITALIC`, `_UNDERLINE`,
`_STRIKE`.

Those are text-**style** bits, and `platform.h` already declares
`plat_font_open(..., u16 style_flags, ...)` — the argument that consumes them. They
belong beside it, not in the document model. The fix is to define them in `platform.h`:

```c
#define PLAT_STYLE_BOLD      (1u << 0)
#define PLAT_STYLE_ITALIC    (1u << 1)
#define PLAT_STYLE_UNDERLINE (1u << 2)
#define PLAT_STYLE_STRIKE    (1u << 3)
```

Same bit values, so `docmodel.h`'s `wp_style_flags` (which also has SUPER and SUB, both
genuinely document concerns) can keep its own definition or alias these. With that one
change, **nothing under `platform/*/` except the app shells includes an engine header.**

This is worth doing on its own merits even if the extraction never happens — and it was
done for exactly that reason. `docmodel.h`'s `wp_style_flags` keeps its own definition of
the same bit values; `host/tests/test_platform_style.c` fails if the two ever drift, which
matters because the engine passes `wp_style_flags` straight into a seam that reads them as
`PLAT_STYLE_*`, so a divergence would be silent everywhere except on real hardware.

## The seam check, which is what keeps this true

`make seam-check`, `seam-check-atari`, `-amiga`, `-mac`, `-dos`, `seam-check-all`.

Each compiles one backend **without an application around it**. The gate is the include
path, not the compiler: `build/seam/include/` holds symlinks to exactly the four
shareable headers, so a backend that includes `docmodel.h` fails with *No such file*
rather than compiling happily against `-Iengine/include`. Compiling the backends as part
of the app — which is all every other target does — structurally cannot catch this,
which is why the leak survived as long as it did. Verified by reintroducing the
`docmodel.h` include deliberately: `seam-check-atari` exits 2 while `make atari` still
builds.

Two things follow that are worth having regardless of the extraction:

- `build/seam/include/` is precisely the `include/` directory the move below would
  create, so a passing `seam-check-all` is a standing check that the extraction is still
  viable, without anyone attempting it.
- `seam-check-mac` is the **only** `make` target that compiles `platform/mac/` at all.
  `mac-engine-check` is engine-only and the real Mac build goes through Retro68's CMake,
  so those files previously had no Makefile-side compile coverage.

## The move

    <shared>/include/    platform.h  types.h  endian.h  utf8.h
    <shared>/src/        endian.c  utf8.c
    <shared>/backends/   host/ atari/ amiga/ mac/ dos/
                         (everything in platform/<target>/ EXCEPT app_shell* and main.c)

`git mv` will not cross repositories: copy, then `git rm` from RetroWP.

Keep the header **filenames** identical (`types.h`, not `wp_types.h`). Every `#include
"types.h"` in the engine then resolves to the shared copy with no source change, provided
the shared include dir is on the path and the old file is gone.

## The Makefile recipe

The build uses `$(wildcard platform/<t>/*.c)` with `patsubst` object rules per target, so
sourcing from a second root means a parallel wildcard, a nested `patsubst`, and one
cloned pattern rule per target. It is about **50 lines of Makefile in total** and touches
no other file.

1. Near the top:
   ```make
   RETROPLAT ?= ../retroplat
   ```
2. Add `-I$(RETROPLAT)/include -I$(RETROPLAT)/backends/<target>` to `INCLUDES`,
   `ATARI_INCLUDES`, `AMIGA_INCLUDES`, `MAC_INCLUDES` and `DOS_INCLUDES` (DOS uses
   `-i=` rather than `-I`).
3. Extend the source lists:
   ```make
   ENGINE_SRC   = $(wildcard engine/src/*.c) $(wildcard $(RETROPLAT)/src/*.c)
   PLATFORM_SRC = $(wildcard $(RETROPLAT)/backends/host/*.c)
   ```
4. Nest the `patsubst` so both roots map into the same object dir:
   ```make
   ENGINE_OBJ = $(patsubst engine/src/%.c,$(BUILD)/engine/%.o,\
                $(patsubst $(RETROPLAT)/src/%.c,$(BUILD)/engine/%.o,$(ENGINE_SRC)))
   ```
5. Clone each object pattern rule for the new root. GNU Make picks whichever rule's
   prerequisite exists, so the two coexist:
   ```make
   $(ATARI_BUILD)/app/%.o: $(RETROPLAT)/backends/atari/%.c | $(ATARI_BUILD)/app
   	$(ATARICC) $(ATARI_RELAXED_CFLAGS) ... -c $< -o $@
   ```
   Do this for `$(BUILD)/engine`, `$(BUILD)/platform`, and the `app` and `engine` rules
   of each of ATARI, AMIGA, MAC and DOS.

Note `mac-engine-check` compiles the engine only and never built the Mac backend, so
after the move nothing in RetroWP compiles `backends/mac/` — the shared library needs its
own standalone compile check to cover it.

## Verification protocol — read this before starting

**Take the baseline first.** RetroWP's suite is *not* fully green on a clean tree:

```
make test   ->  435/438 test cases passed
```

The three failures are all in `host/tests/test_image_convert.c` and are
environment-dependent — they need Pillow available to the offload gateway. They are
**pre-existing and unrelated to the platform layer**. Refactor first and test afterwards
and you will spend real time hunting a regression you did not cause.

The two gates that must hold:

| gate | expected |
|---|---|
| `make test` | 435/438, same three `image_convert` failures |
| `make atari` | `bin/atari/RETROWP.PRG` links, ~292 KB |
| `make seam-check-all` | every installed toolchain passes; absent ones skip |

The 435 above is environment-dependent, not a constant: with Pillow available to the
offload gateway the same tree gives a full sweep. Take your own baseline rather than
trusting either number.

Both held after the extraction was applied.

Work on a branch. The whole change is deletions plus a Makefile edit
(**56 files, +50 / −6,624**), so `git reset --hard` reverts it completely — which is
exactly how it was undone.

## Deferred: the `wp_` → `plat_` rename

A shared library carrying `wp_status` / `WP_OK` / `WP_TYPES_H` is odd, but renaming them
touches every file in every consumer. It is mechanical and separable, and it was
deliberately **not** done as part of the extraction — mixing a rename into a structural
move makes both harder to verify. Do it afterwards, as its own commit, with both gates
green before and after. New seam additions can use `plat_`/`PLAT_` from the start (the
style flags above already do).

## Prior art: GACS vendored instead

`~/dev/gacs` needed exactly this layer and took the other road: it holds a **copy** under
`gacs/platform/`, with `gacs/platform/README.md` recording the provenance. RetroWP is
neither modified by nor dependent on it.

The trade that copy accepts, stated plainly there: **a backend fix does not propagate**
between the two and has to be carried across by hand. If that ever becomes painful, this
document is the recipe for closing it. The one local change GACS made — the
`PLAT_STYLE_*` decoupling described above — **has now been upstreamed here**, so a
file-by-file diff of the two copies is once again empty apart from that change's own
wording.

GACS is also a working demonstration that the seam is genuinely app-independent: its GEM
shell (`gacs/shells/gem/main.c`, ~380 lines) drives an AES window and draws entirely
through `plat_draw_text` / `plat_fill_rect`, sharing no UI code with RetroWP at all. It
runs under EmuTOS in Hatari. The one thing it needs beyond `platform.h` is
`metrics_atari_internal.h`'s `atari_gc` (`{ VdiHdl vdi; short window_handle; }`), because
`plat_gc` is opaque and the application owns its own GEM window — a documented per-backend
escape hatch, not a leak.
