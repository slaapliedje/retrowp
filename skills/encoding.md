# Skill: Character Encoding — UTF-8 Core, Native Edges

> Purpose: Define the ONE encoding rule for the whole project so that character
> set conversion never leaks into the engine. Read this before touching any code
> that reads/writes text, draws glyphs, or imports/exports a format.

## The One Rule
The engine speaks UTF-8. Only. Everywhere internal.
Every conversion to/from a platform or format charset happens at an EDGE:
  1. Format importers/exporters (format charset <-> UTF-8), and
  2. The platform layer (native display/input charset <-> UTF-8).
Nothing in `engine/` or `services/` ever converts, assumes, or inspects a
non-UTF-8 encoding. If you find yourself byte-mapping characters inside the
engine, STOP — you are in the wrong layer.

## Why UTF-8 Internally
- Single source of truth: the document model stores UTF-8 (see
  docs/document-model.md, string pool + wp_span).
- Portable: identical bytes on 68k (big-endian) and x86 (little-endian). UTF-8
  is a byte stream — endianness never applies to it. (Contrast with UTF-16,
  which we DO NOT use anywhere.)
- Round-trippable: ODT/modern RTF are Unicode-capable, so no information is lost
  moving through the model.
- Future-proof: new languages/glyphs need no engine changes.

## The Two Edges (where conversion is REQUIRED)

### Edge A — Platform Layer (display + input)
Each target has a native character set that is NOT UTF-8:
  - Atari ST / TOS:  Atari ST character set (codepage-like, DOS-ish upper half).
  - Amiga:           ISO-8859-1 (Latin-1) as the common system charset.
  - Mac Classic:     Mac Roman.
  - DOS:             an OEM code page (e.g., CP437/CP850).
The platform backend converts at exactly two points:
  - DISPLAY: engine hands UTF-8 spans to `plat_measure_text` / `plat_draw_text`;
    the backend converts UTF-8 -> native for its font system, per glyph/run.
  - INPUT: keyboard/clipboard bytes arrive native; the backend converts
    native -> UTF-8 BEFORE handing text to the engine.
The engine NEVER sees native bytes. The platform NEVER receives non-UTF-8.

### Edge B — Format Importers/Exporters
Each format declares its own encoding; the converter maps it to/from UTF-8:
  - Plaintext (`format_txt`): assume UTF-8 by default; allow an explicit
    source-charset parameter for legacy files (e.g., import a Latin-1 .txt).
  - Classic RTF: 7-bit ASCII + `\'hh` hex escapes in a code page declared by
    `\ansicpgN` / font charset; convert those to UTF-8 on import, and emit the
    correct escapes on export.
  - ODT / modern RTF (offloaded): already Unicode; the gateway returns UTF-8
    directly (see protocol below).

## Forbidden (do NOT do these)
- Do NOT store anything but UTF-8 in the string pool.
- Do NOT use `char`-based upper-ASCII assumptions in engine logic (e.g., testing
  `c > 127` to mean a specific accented letter). Use codepoint-aware helpers.
- Do NOT use `strlen`/byte counts as CHARACTER counts. Bytes != codepoints.
- Do NOT introduce UTF-16 or wchar_t anywhere. (wchar_t width is platform-
  dependent — banned in shared code.)
- Do NOT convert encodings inside `services/` (spell/thesaurus/grammar). Those
  operate on UTF-8 too; their DATA FILES are UTF-8.

## UTF-8 Helpers (engine-provided, platform-free)
Provide these in `engine/` (e.g., `utf8.c`/`utf8.h`) so every layer shares one
correct implementation. They are pure functions over byte buffers — no platform
calls, no allocation surprises.

```c
/* Decode one codepoint at s[0..len). Returns bytes consumed (1..4), or 0 on
   invalid sequence (caller decides replacement policy). Writes cp on success. */
u32 utf8\_decode(const u8 *s, u32 len, u32 *cp\_out);

/* Encode cp into out (must have room for up to 4 bytes). Returns bytes written
   (1..4), or 0 if cp is not a valid scalar value. */
u32 utf8\_encode(u32 cp, u8 *out);

/* Number of bytes of the codepoint starting at s[0] (from lead byte), 1..4;
   0 if s[0] is a continuation/invalid lead. */
u32 utf8\_seq\_len(u8 lead);

/* Step a byte offset forward/back by ONE codepoint within [0,len].
   Returns new offset; clamps at bounds. Used by cursor movement. */
u32 utf8\_next(const u8 *s, u32 len, u32 offset);
u32 utf8\_prev(const u8 *s, u32 len, u32 offset);

/* Codepoint count of a span (for word logic, NOT for byte math). */
u32 utf8\_cp\_count(const u8 *s, u32 len);

/* Validate a buffer; returns WP\_OK or WP\_BADFMT and the offending offset. */
wp\_status utf8\_validate(const u8 *s, u32 len, u32 *bad\_offset);

