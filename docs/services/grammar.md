# Grammar-Lite Service (M8, T8.2)

`services/grammar` is pure C, data-driven, no UI, and has NO platform-
specific calls at all -- not even `platform.h`'s `plat_file_*`/`mem_alloc`
seam `services/spell` needs for its on-disk dictionary. There is no data
file and no allocation here: `grammar_lite_check()` is a single in-memory
scan over a caller-supplied byte buffer. Per CLAUDE.md's architecture
layers, it is shared, portable code, compiled once and linked everywhere.

## Why this exists, and why it is deliberately NOT a smaller LLM

M8 T8.1 already built a real, LLM-backed `OFFLOAD_GRAMMAR_CHECK` gateway
op (`docs/formats/offload-protocol.md`, `offload/gateway/ollama_gateway.c`)
wired into `platform/atari/app_shell.c`'s Tools > Grammar Check menu
action. But CLAUDE.md's Offload Rules are explicit: "the engine must be
fully functional WITHOUT an offload device attached." Before this
module existed, Grammar Check with no Pi/gateway reachable just failed
with a "No offload device available" alert -- functional in the sense
that it didn't crash, but not a working feature. `services/grammar` is
what Grammar Check falls back to in that case.

It intentionally does NOT try to approximate what an LLM does. Real
grammar/usage correctness (subject-verb agreement, wrong-word-for-
context like "their" vs "they're", awkward phrasing) needs semantic
understanding of the sentence -- exactly the kind of large-vocabulary,
contextual judgment CLAUDE.md's own project brief names as offload-
worthy, and exactly what T8.1's real-Ollama-backed path already does
well (see that gateway's own empirical model comparison). Faking that
with hand-written rules would mean either a firehose of false positives
(most contextual heuristics are wrong more often than they're right) or
an ever-growing pile of special cases chasing specific sentences --
neither is "compact." Instead, every rule here is chosen because it is
**purely mechanical**: detectable from the bytes alone, with no
understanding of meaning, and (by construction) essentially never a
false positive on real prose. This is the same scoping discipline
`docs/services/spell.md`'s affix table already applies ("a genuinely
compact system, not a disguised attempt at full morphological
analysis") -- basic checks, not a rules-engine reimplementation of a
language model.

## The rule set

Five rules, each its own independent left-to-right pass over the whole
input (`grammar_lite_check()` just calls them in sequence and
concatenates results -- no shared parser state, no rule interaction):

1. **Repeated word** (`check_doubled_words`): two identical (case-
   insensitive) alphabetic words separated by nothing but spaces --
   "the the". Flags the WHOLE phrase (both words and the separating
   space), suggests a single copy of the word. Resumes scanning from
   the end of the SECOND word on a match, so "a a a" reports one issue
   covering the first pair, not two overlapping ones.
2. **Multiple spaces** (`check_multiple_spaces`): a run of 2+ literal
   space characters. Suggests collapsing to exactly one.
3. **Space before punctuation** (`check_space_before_punct`): one or
   more spaces immediately followed by `,` `.` `;` `:` `!` `?` `)`. The
   flagged span INCLUDES the punctuation mark, and the suggestion is
   just that mark alone -- "word , next" becomes "word, next" as a
   single delete-and-replace.
4. **Repeated punctuation** (`check_repeated_punct`): two or more of
   the SAME character, restricted to `!`/`?` only. `.`/`,` are
   deliberately excluded so a real `"..."` ellipsis is never flagged --
   the alternative (special-casing exactly three dots) adds real
   complexity for a rule whose whole point is staying mechanical and
   obviously correct. Doesn't catch mixed clusters like `"?!?!"` -- a
   documented gap, not an oversight.
5. **Missing initial capital** (`check_missing_initial_capital`): the
   first alphabetic character in the WHOLE input is lowercase. Scoped
   to text-initial only, not every sentence boundary within it --
   detecting mid-text sentence starts reliably needs handling
   abbreviations ("Mr. Smith"), decimals, and other period usages that
   don't end a sentence, which is real complexity this rule
   deliberately doesn't take on. Since `platform/atari/app_shell.c`
   always passes exactly one paragraph, this is "this paragraph doesn't
   start with a capital letter" in practice, which is exactly the
   common case (this app has no sentence-splitting anywhere else
   either).

**No rule ever produces an empty suggestion.** Every span is chosen
specifically so a real, non-empty replacement always exists (see rule
3's example above) -- `grammar.h` documents this as a hard contract,
not an incidental property, because it lets `platform/atari/
app_shell.c`'s `do_grammar_check()` treat every local-checker issue as
genuinely auto-appliable, the same way it already treats a gateway
issue with a non-empty suggestion.

**ASCII-only, fails open**, the same scope discipline
`docs/services/spell.md` already documents for the dictionary checker:
any byte `>= 0x80` is simply not alpha/space/punctuation to this
scanner, so a UTF-8 multi-byte sequence is never misread as ASCII
content -- it just acts as an inert separator, invisible to every rule
rather than incorrectly flagged.

## Why offsets/lengths are ASCII byte-exact and safe to apply directly

Every rule computes `start_offset`/`span_len` from a literal byte scan
of the SAME buffer it was given -- there is no substring search or
model-produced position to independently verify (contrast with
`offload/gateway/ollama_gateway.c`'s `OFFLOAD_GRAMMAR_CHECK` handler,
which has to re-locate a model's claimed quote in the original text
because the model itself cannot be trusted to count bytes). A
`grammar_lite_check()` issue's span is correct by construction: it is
exactly the bytes the matching rule looked at.

## Real bug caught during development

An early draft of `grammar.h`'s file header comment wrote
`"platform.h's plat_file_*/mem_alloc"` -- and `*/` appearing literally
inside a `/* ... */` comment (`plat_file_*` immediately followed by
`/mem_alloc`) is the block-comment TERMINATOR, closing the comment far
earlier than intended. Everything after that point was parsed as real
C code, producing a cascade of confusing errors ("expected `=`, `,`,
`;`... before `--` token", then "missing terminating `'` character" on
every apostrophe later in the file, since those apostrophes were now
outside any comment and looked like unterminated character literals to
the compiler). Caught immediately by `make test` under this project's
strict `-std=c89 -pedantic -Werror` build (services/ is held to the
same strict flags as `engine/`), not by inspection. Fixed by rewording
to avoid the literal `*/` sequence ("plat_file_ calls or mem_alloc").

## API

```c
#define GRAMMAR_MAX_MESSAGE_LEN    40
#define GRAMMAR_MAX_SUGGESTION_LEN 40

typedef struct {
    u32  start_offset;
    u32  span_len;
    char message[GRAMMAR_MAX_MESSAGE_LEN];
    char suggestion[GRAMMAR_MAX_SUGGESTION_LEN];
} grammar_issue;

u32 grammar_lite_check(const u8 *text, u32 len, grammar_issue *out, u32 max_issues);
```

Issues are NOT guaranteed to be in ascending `start_offset` order (each
rule is its own independent pass) -- callers that need presentation
order sort the result themselves.  `platform/atari/app_shell.c`'s
`do_grammar_check()` already needs to sort a gateway-sourced issue list
for its own delta-tracking reasons (`docs/atari-platform.md`), so a
locally-sourced list gets correct ordering there for free, with no
duplicate sort logic in this module.

## Testing

`host/tests/test_grammar.c` (part of `make test`): one positive/negative
pair per rule (including the case-insensitivity and ellipsis-exclusion
edge cases called out above), an empty-input case, a `max_issues` cap
test that plants ten independent doubled-word instances against a
3-issue buffer (must return exactly 3, never write past `out[2]`), and
-- mirroring `docs/services/spell.md`'s own explicit lesson that a tiny
hand-written fixture can miss a real scanning bug a longer, more
realistic input would catch -- one combined test that plants an
instance of every rule inside a single, moderately long paragraph and
asserts all five fire correctly together in one pass. Every offset
asserted in this file was computed with `python3 -c "print(s.index(...))"`
against the literal fixture string, not hand-counted -- the same
verify-with-code discipline this project's own history keeps landing on
after repeated hand-counting mistakes (see `docs/atari-platform.md`'s
T7.4 section, and `offload/gateway/fixtures.c`'s grammar fixture
comment).

## Known gaps

- **No spelling.** `services/spell` already covers that, deterministically,
  and this module deliberately doesn't overlap with it.
- **No semantic/contextual checks at all** -- wrong-word-for-context
  (their/there/they're), subject-verb agreement, awkward phrasing, and
  every other issue that needs actual language understanding are
  explicitly out of scope; that's what the offload-backed
  `OFFLOAD_GRAMMAR_CHECK` path (M8 T8.1) is for.
- **Sentence-initial capitalization only checks the very start of the
  input**, not every sentence within it -- a real mid-text detector
  would need abbreviation/decimal handling this module deliberately
  doesn't take on.
- **Repeated-punctuation only catches a single character repeated**,
  not mixed clusters like `"?!?!"`.
- **ASCII-only** -- see the scope note above; symmetric with
  `services/spell`'s and `platform/atari`'s own ASCII-first choices
  elsewhere in this project.
