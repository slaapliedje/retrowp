# Format Module / Plugin Architecture (Design Note, Not Yet Implemented)

> Captured ahead of M5 (RTF) so the idea isn't lost. Not built yet --
> `format_txt` is still called directly by name everywhere. Revisit and
> flesh this out when `format_rtf` lands, since that's the point a
> second format module actually exists to design a registry around.

## The Idea

Datatype-style pluggability (per the Amiga's `datatypes` system): a
common interface every format module implements, plus a registry so
callers pick a module by extension/content-sniff instead of calling
`format_txt_import` by name. New formats get added by writing a module
and registering it, not by touching call sites throughout the engine.

## Adjusted for This Project's Constraints

Amiga datatypes are *runtime*-loaded shared libraries. That's not
available on plain-TOS 1MB Atari ST (no dynamic linker, and CLAUDE.md
already limits the engine to a small allocator wrapper with no other
dynamic dependencies). Two tiers instead:

- **On-device (engine-side), compile-time registry**: a `wp_format`
  vtable struct (import/export function pointers, name, extensions, a
  magic-sniff function) that `format_txt` and future `format_rtf` both
  implement, plus a small static table listing which modules are linked
  into a given build. Lightweight formats only -- plain text, classic
  RTF (M5's scope per docs/milestones.md).
- **Offload-side (Pi gateway), real plugin loading**: heavy formats --
  WordPerfect, MS Office, ODT -- have no memory constraint on the Pi, so
  genuine dynamic plugin loading (Python entry points, shared libraries)
  is reasonable there. This is arguably closer to the actual spirit of
  Amiga datatypes than anything on-device could be. Maps onto the
  already-defined `OFFLOAD_CONVERT_IMPORT`/`OFFLOAD_CONVERT_EXPORT` ops
  in `engine/include/offload.h` and M7 in docs/milestones.md -- the
  gateway decides which converter handles a given format id, the ST
  engine never needs to know how.

## Open Questions for M5

- Exact `wp_format` vtable shape (does export need the streaming variant
  CLAUDE.md's Memory Rules call for, per docs/formats/txt.md's note that
  `format_txt_export` deliberately isn't streaming?).
- How the registry is populated: a hand-written static array (simplest,
  matches this project's general preference for explicit over clever) is
  the likely default -- no reflection/auto-registration machinery needed
  for a handful of compiled-in formats.
- Whether magic-sniffing is needed at all for M5's scope (classic RTF
  always starts with `{\rtf`, plaintext doesn't have a reliable magic
  number) or whether extension-based dispatch is sufficient until ODT
  (M7) actually needs content-sniffing (ZIP magic bytes).
