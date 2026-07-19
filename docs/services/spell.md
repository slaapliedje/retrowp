# Compact Spell Service (M5, T5.3)

`services/spell` is pure C, data-driven, no UI, no platform-specific calls
beyond `platform.h`'s `plat_file_*`/`mem_alloc` seam (the same one
`engine/src/offload_client.c` already uses) -- per CLAUDE.md's
architecture layers, it's shared, portable code, compiled once and linked
everywhere.

## The "not fully loaded" requirement

The dictionary is a **plain sorted word list on disk** (one lowercase
a-z word per line, LF-terminated, byte-lexicographic order), queried by
**binary search directly against the file** via `plat_file_seek`/
`plat_file_read` -- never read into memory as a whole. Peak RAM per
lookup is a handful of fixed, small stack buffers (`SPELL_MAX_WORD_LEN`
= 32 bytes each); the only heap allocation in the whole module is the
tiny `spell_dict` handle itself (a file pointer and a size). This is a
design-level guarantee, not a runtime-measured ceiling: the code
structurally cannot buffer more than one word at a time, regardless of
how large the dictionary file grows. `services/tests` (host/tests, in
this milestone) verifies zero leaks via the same `mem_host` leak
counters the rest of the test suite uses.

Binary search over a line-oriented text file needs one extra piece
beyond array binary search: a probe at an arbitrary byte offset almost
never lands on a line boundary. `find_line_start()` + `read_line_at_or_
before()` (`spell.c`) handle this the standard way (as `look(1)` does):
scan **backward** byte-by-byte from the probe offset until a `'\n'` is
found (or the start of the file), then read forward from there to get
the line that *contains* the probe offset. This costs extra small reads
per probe but needs no index file alongside the dictionary -- one plain
sorted text file is the entire on-disk format.

**Scanning backward to the containing line, not forward to the next
line, is load-bearing for correctness, not just a style choice.** An
earlier version of this code scanned *forward* to the next line instead
(simpler to write: "if the byte before the probe isn't a newline, keep
reading until one is found"). That version passed a full test suite
built only against an 11-word hand-written fixture, but hung
indefinitely -- a genuine infinite loop, not just slowness -- against
the realistic 6000-word fixture once a lookup happened to probe inside a
line whose word compared *greater* than the target: skipping ahead to
compare the *next* line's word instead could set the search's upper
bound back to a value already visited, so the loop never made progress
on some inputs. Landing on the line that actually *contains* the probe
guarantees `line_start <= probe_offset`, which is exactly what makes
each loop iteration provably shrink the search range (see the comment
on `read_line_at_or_before` in `spell.c` for the short proof). Caught by
`test_spell_realistic_dictionary_sample` (host/tests/test_spell.c)
hanging rather than failing -- a reminder that small hand-written
fixtures can miss real algorithmic bugs that only manifest at realistic
scale, which is part of why that 6000-word fixture exists alongside the
precise hand-authored one.

## Affix rules (compact, not Hunspell-compatible)

A real Hunspell `.aff` file (checked on this dev machine: `en_US.aff`,
~200 lines) implements a small rule-condition language with prefix/
suffix classes, compound-word rules, and per-word rule-applicability
flags encoded in the `.dic` file. That's a multi-week project on its
own and far beyond what "compact" is asking for here. Instead,
`services/spell`'s affix handling is a small **fixed table** of common
English suffix transformations, tried in order after an exact-match
lookup fails:

```
strip "s"                       (cats -> cat)
strip "es"                      (boxes -> box)
strip "ies", add "y"            (flies -> fly)
strip "ed"                      (walked -> walk)
strip "ed", add "e"             (baked -> bake)
strip "ing"                     (walking -> walk)
strip "ing", add "e"            (baking -> bake)
strip "ly"                      (quickly -> quick)
strip "er"                      (faster -> fast)
strip "est"                     (fastest -> fast)
```

Each candidate stem is looked up with the same binary-search dictionary
lookup a plain word uses -- no separate mechanism. This deliberately
does not handle consonant-doubling (`running` -> `run`, not `runn`),
irregular plurals/verbs (`mice`, `went`), or prefixes (`re-`, `un-`).
Real, documented gaps -- a genuinely "compact" affix system, not a
disguised attempt at full morphological analysis.

## Suggestions

Bounded, classic edit-distance-1 candidate generation (deletion,
transposition, substitution, insertion over `a`-`z`) -- each candidate
checked via the exact-match dictionary lookup only (not re-run through
affix stripping, to keep the candidate count from multiplying further).
For a word of length L this is roughly `O(L^2)` candidates, each costing
a full binary search (`O(log N)` probes against the dictionary). This is
bounded and correct but not fast -- a real-time "squiggly underline as
you type" experience is not the target; an on-demand "check this word"
feature is. `max_suggestions` bounds both the candidate scan (stops
early once enough are found) and the caller's output buffer.

## Dictionary data

`services/spell/data/en_sample.dict` is a **test/demo fixture**, not a
claimed production dictionary: 6,000 words sampled (evenly across the
sorted list, for a-z breadth rather than only-common-words bias) from
the `en_US` Hunspell dictionary already installed on this development
machine (`hunspell-en_us`, BSD-style license, Copyright 2002 Kevin B.
Hendricks and contributors -- see the bundled `license.myspell`;
redistribution with modification is permitted provided modifications are
clearly marked, which this file's header comment does). Swapping in a
larger or different-language dictionary is a data change, not a code
change -- the file format and lookup code don't know or care about word
count or language beyond "sorted, one lowercase a-z word per line."

## API

```c
wp_status spell_dict_open(const char *native_path, spell_dict **out);
void      spell_dict_close(spell_dict *dict);

wp_status spell_check_word(spell_dict *dict, const u8 *utf8_word, u32 len,
                            wp_bool *out_known);
wp_status spell_suggest(spell_dict *dict, const u8 *utf8_word, u32 len,
                         char out[][SPELL_MAX_WORD_LEN], u32 max_suggestions,
                         u32 *out_count);
```

**ASCII-only scope, fails open.** Any word containing a byte `>= 0x80` is
outside this dictionary's coverage. `spell_check_word` reports such words
as `*out_known = WP_TRUE` (assumed correct) rather than `WP_FALSE` --
deliberately fails open, not closed: incorrectly flagging every accented
or non-English word as a misspelling would be a worse, more misleading
result than silently not checking it. `spell_suggest` returns zero
suggestions for such input (nothing useful to offer). Symmetric with the
same ASCII-only simplification already made for `platform/atari`'s
charset conversion (`docs/atari-platform.md`). A real multi-language/
accented-word dictionary is future work, not silently faked here.
