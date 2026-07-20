# Editing Core (M2)

> Covers cursor movement, insert/delete, and undo/redo as implemented
> against the locked structs in `docs/document-model.md`
> (`wp_pos`/`wp_selection`/`wp_op`). Read this before touching
> `engine/src/cursor.c`, `engine/src/edit.c`, or `engine/src/undo.c`.

## Scope of This Milestone

`wp_pos` addresses a location as `(section, paragraph, run, offset)` --
an index into the run array, not a flat byte offset. This has real
consequences for what's simple and what's hard:

- **Cursor movement** (`wp_pos_next`/`wp_pos_prev`) is read-only
  traversal and freely crosses run, paragraph, and section boundaries.
- **Insert** is always a point operation (a caret, never a range).
- **Delete** in this milestone is scoped to a range **within a single
  run** (`start.section == end.section && start.paragraph == end.paragraph
  && start.run == end.run`). This matches the literal T2.2 AC ("tests
  cover start/middle/end of run **and paragraph**" -- i.e. where within
  a paragraph you edit, not selections spanning paragraphs). Deleting a
  selection that spans multiple runs or paragraphs is still out of scope
  *here* -- see `docs/vimode.md` for the M8.5 general delete-range work
  built on top of this.

**Update (M8.5): paragraph split/merge is now implemented.**
`WP_OP_SPLIT_PARA`/`WP_OP_MERGE_PARA`, defined since M2 but unused until
now, back real `wp_document_split_paragraph_raw`/
`wp_document_merge_paragraph_raw` (+ journaled `wp_edit_split_paragraph`/
`wp_edit_merge_paragraph`) in `engine/src/edit.c`/`undo.c`. This closed a
real, pre-existing gap independent of any later modal-editing work: before
this, Enter was a hard no-op in the one real UI, and a document couldn't
grow a second paragraph through editing at all. See "Paragraph Split /
Merge" below.

## Why the String Pool Never Needs to Move Bytes

The pool is append-only (per `strpool.h`): nothing is ever overwritten or
freed mid-pool. Insert always appends new bytes at the pool's current
end and references them with a new span; delete never touches pool bytes
at all -- it only removes or shrinks **run** entries so a paragraph no
longer references some bytes. The bytes stay physically present
(unreferenced, unrecoverable through the API, but not overwritten). This
means undo of insert/delete is entirely a **run-array structural**
operation -- never a byte copy -- which is what makes it fit in `wp_op`'s
fixed-size fields (see below).

## Style Inheritance on Insert

Given `pos = (section, paragraph, run = R, offset = O)` into a paragraph
with `run_count` existing runs:

- **Empty paragraph** (`run_count == 0`): the new run uses
  `doc->default_cstyle` (no prior context to inherit).
- **`O == 0 && R == 0`** (very start of a non-empty paragraph): inherits
  run 0's style (typing before existing text continues that text's
  style -- there's no "previous character" to stick to).
- **`O == 0 && R > 0`** (exactly on the boundary between run `R-1` and
  run `R`): inherits run `R-1`'s style -- standard "insertion point
  sticks to what you just typed" behavior.
- **`O == len(run[R])`** (end of run `R`, including end-of-paragraph):
  inherits run `R`'s style.
- **`0 < O < len(run[R])`** (strictly mid-run): the run is split; the new
  text inherits run `R`'s style (obviously, since you're typing inside
  it).

**Insert always creates exactly one new run for the inserted text** --
it never extends or merges into an adjacent run, even when that would be
possible (e.g. appending at a run's tail when the new bytes happen to
land contiguously in the pool). This is a deliberate simplification:
run-coalescing for typing efficiency is real future work (noted, not
implemented), traded here for an implementation and undo story that's
easy to get right. A mid-run insert therefore produces **two** new array
entries: the inserted-text run, and a new run for the original run's
tail (the original run's entry is shrunk in place to become the head).

## Delete (Single-Run Range)

Given a run `R` and a sub-range `[O1, O2)` within it (`0 <= O1 < O2 <=
len(run[R])`), delete reconstructs run `R`'s slot in one of three ways:

- **Whole run removed** (`O1 == 0 && O2 == len`): `run_remove_at(R)`.
- **One fragment survives** (`O1 == 0` xor `O2 == len`): run `R`'s span
  is resized in place (offset and/or length adjusted) to keep the
  surviving side. No array insert/remove.
- **Split** (`O1 > 0 && O2 < len`): run `R` is resized in place to the
  left fragment; a new run for the right fragment is inserted at `R+1`.

## Paragraph Split / Merge (M8.5)

**Split** validates `pos` with exactly the same branch structure "Style
Inheritance on Insert" above uses (same question: "is this pos meaningful
in this paragraph's structure," just without inserting text), reporting a
`split_run` index -- every run at/after it moves into a new paragraph
inserted at `pos.paragraph + 1`. A `pos` landing strictly mid-run splits
that run first (identical to insert's mid-run case), so the tail
fragment becomes a real run the move-loop picks up uniformly. The new
paragraph's style is a **copy** of the original's (both paragraphs share
style right after Enter, matching most editors). Runs are moved by
**value** (a `wp_run` is just a pool span + a style handle, owns no
memory), so the survivor's `run_count` is simply truncated to `split_run`
afterward -- nothing to free, the same reasoning `run_remove_at` already
relies on.

**Merge** is the exact inverse: the second paragraph's runs are
`wp_paragraph_run_add`-ed onto the first (**never coalesced** into fewer
runs, matching insert's own never-coalesce rule), then the second
paragraph's slot is removed. Removing a paragraph is not free the way
removing a run is -- a paragraph owns a `runs` array and a layout cache,
so `paragraph_remove_at` (`engine/src/edit.c`, `edit_internal.h`) calls
`wp_paragraph_layout_free` and frees `runs` before shifting later
paragraphs down. Single-section only: merging across a section boundary
is out of scope this pass (nothing user-facing produces multi-section
documents yet, matching M2's own established precedent for scope limits).

**The canonical "end of paragraph" position, and why split/merge are
exact inverses of each other.** A position can never be represented as
`(run = run_count, offset = 0)` -- `wp_pos_next`/`prev`'s own
`end_of_paragraph` helper (cursor.c) and insert's validation both only
ever produce `(run = run_count - 1, offset = len(that run))` for "end of
paragraph" (or `(0, 0)` if the paragraph is empty). Merge's `*out_pos`
(the journal's `at` for `WP_OP_MERGE_PARA`) is computed the same way,
captured from the **first** paragraph's run count *before* absorbing the
second paragraph's runs -- so re-splitting at that exact position always
reconstructs precisely the runs that were absorbed, whether the absorbed
paragraph was empty, the survivor was empty, or neither was.

## Undo Journal: Field Reuse Per Op Kind

`wp_op` is deliberately minimal (`at`, `text`, `style_before`,
`style_after`, `next`) -- it was sized for simple ops, not arbitrary
compound edits. Both `WP_OP_INSERT_TEXT` and `WP_OP_DELETE_TEXT` fit
without changing the struct, by reusing fields per kind:

- **`WP_OP_INSERT_TEXT`**: `at` = the pre-insert position `(R, O)`;
  `text` = the span of the inserted bytes. Undo does **not** try to
  recompute the inserted run's array index from `at` -- insert's
  placement rule (see "Style Inheritance" above) branches on paragraph
  state (e.g. `run_count == 0`) that's only true *before* the insert, so
  reapplying it to the *post*-insert paragraph doesn't reliably land on
  the same index. Instead, since the pool is append-only (offsets are
  never reused), `text` alone uniquely identifies the run: undo scans
  the paragraph for the run whose span exactly equals `text`
  (`find_run_by_span`) and calls `run_remove_at` on it. Redo reuses the
  same placement rule that a fresh insert would (via the shared
  `insert_span_at_pos` helper), but passes `text` directly instead of
  appending new bytes -- since the paragraph is back in its pre-insert
  shape after undo, the rule's state-dependent branches apply correctly
  again.
- **`WP_OP_DELETE_TEXT`**: `at` = `(section, paragraph, run = R)` (the
  run's index -- `at.offset` is unused); `text` = the **deleted
  sub-span** (in pool coordinates: `offset` = where those bytes sit in
  the pool, `length` = how many) -- deliberately *not* the run's
  original full span. `style_before` = the run's cstyle (delete never
  changes it); `style_after` is repurposed as a case tag: `0` = whole
  run removed, `1` = left fragment survived (deleted was the right
  part), `2` = right fragment survived (deleted was the left part),
  `3` = split (both fragments survived). Storing the *deleted* span
  rather than the original full span is what makes both directions work
  from the same field, with no 5th field needed:
  - **Undo** restores the full run by extending whatever survived back
    over the deleted span (case 1: `length += text.length`; case 2:
    `offset = text.offset; length += text.length`; case 3: sum both
    fragments' lengths plus the gap into run `R`, then `run_remove_at`
    the fragment at `R+1`; case 0: `run_insert_at` a fresh run from
    `text`/`style_before`).
  - **Redo** doesn't need to replay case-specific logic at all: once
    undo has restored one contiguous run at `R`, `text.offset` minus
    that run's (now-restored) `text.offset` gives the deleted range's
    start relative to the run, and `text.length` gives its length --
    redo just calls `wp_document_delete_range_raw` again with that
    recomputed range, which independently re-derives the correct case
    from the range itself.
- **`WP_OP_SPLIT_PARA`** (M8.5): `at` = the pre-split position exactly as
  passed in; `text`/`style_before`/`style_after` unused. Undo calls
  `wp_document_merge_paragraph_raw(doc, at.section, at.paragraph, NULL)`;
  redo calls `wp_document_split_paragraph_raw(doc, at, NULL)` again --
  both directions reuse the raw structural functions directly, no new
  logic in `wp_edit_undo`/`redo` beyond dispatching to them.
- **`WP_OP_MERGE_PARA`** (M8.5): `at` = the **join-point position**
  `wp_document_merge_paragraph_raw` itself produces as `out_pos` (see
  "Paragraph Split / Merge" above) -- deliberately chosen because it's
  exactly what `wp_document_split_paragraph_raw` needs to invert the
  merge. `style_before` = the **absorbed (second) paragraph's original
  pstyle handle**, captured by `wp_edit_merge_paragraph` before calling
  the raw merge. This field is load-bearing, not redundant: the
  survivor keeps *its own* style through a merge, so a naive re-split on
  undo would default the resurrected paragraph to the survivor's style
  (re-split always copies from the current paragraph, matching a real
  Enter keypress) -- undo must explicitly call `wp_paragraph_pstyle_set`
  on the resurrected paragraph afterward to restore the style that was
  actually absorbed. Redo calls `wp_document_merge_paragraph_raw` again
  at `(at.section, at.paragraph)`.

## Why Undo Doesn't Need to Restore the *Exact* Original Run Layout

A mid-run insert splits one run into (up to) three; undoing it only
removes the middle (inserted-text) run, leaving the original left/right
fragments as **two adjacent same-style runs** rather than the original
one. This is deliberately accepted: the fuzz test (T2.3) verifies
content equivalence via `format_txt_export`, which concatenates a
paragraph's runs' text regardless of run boundaries -- two adjacent
same-style fragments and one merged run export identically. Run-array
layout is not part of the correctness contract here, only rendered
content and paragraph/style structure (which insert/delete in this
milestone never touch). A `format_txt_export` comparison is a valid
"deep compare" oracle specifically because this milestone's op set
(insert/delete) never changes cstyle/pstyle -- a future milestone adding
`WP_OP_SET_CSTYLE`/`WP_OP_SET_PSTYLE` to the fuzz coverage will need a
comparator that also checks style, not just text.

## General Delete-Range and Undo Grouping (M8.5)

`wp_selection {anchor, caret}` (defined since M2, unused until now) gets
its first real consumers here: `wp_document_extract_text` (read-only,
paragraph-boundary-joined-with-`\n`, same two-call sizing convention as
`format_txt_export`) and `wp_edit_delete_selection` (the general
multi-run, multi-paragraph delete this milestone's single-run
`wp_edit_delete_range` deliberately didn't attempt). `wp_pos_compare`
(pure tuple order -- `wp_pos`'s fields already form a total order by
construction) normalizes an `(anchor, caret)` pair into `(start, end)`.

**`wp_edit_delete_selection`'s algorithm**, single-section only (same
scope as split/merge): delete the tail of `start`'s paragraph (from
`start` to that paragraph's end), delete the head of `end`'s paragraph
(from its start to `end`), fully **empty out** (not merge away -- see
below) every whole paragraph strictly between them, then merge
`start`'s paragraph forward `(end.paragraph - start.paragraph)` times.
The "empty out, then merge" step is the one non-obvious part: merging a
middle paragraph directly (without first draining its runs) would
**splice its content into the survivor** -- the opposite of deleting it.
Emptying it first (draining runs one at a time via the already-journaled
`wp_edit_delete_range`) means the subsequent merge just removes a slot
that already has nothing to contribute.

**`out_pos` recovery after multi-step mutation.** The normalized `start`
position, captured before any mutation, can become stale by the time
every delete/merge step above has run -- e.g. if `start.offset == 0`
lands exactly on the LAST run of its paragraph, that run may get
`run_remove_at`'d entirely, leaving `start.run` referencing an index
that no longer exists. The fix: capture `prefix_len` (the byte count
preceding `start` within its own paragraph) BEFORE any mutation --
invariant throughout, since nothing before `start` is ever touched --
then recompute the final `(run, offset)` by walking `prefix_len` bytes
into the (now-restructured) survivor paragraph afterward
(`pos_at_byte_offset_in_paragraph`, `undo.c`), landing on the same
canonical run-boundary convention `wp_pos_next`/insert already use
(a boundary between two runs always resolves to the START of the LATER
run, never the raw length of the earlier one).

**Undo grouping.** A single `wp_edit_delete_selection` call can push
several `wp_op` nodes (one delete per touched run, one merge per
paragraph boundary crossed) but must undo/redo as exactly ONE
user-facing step. Two new marker kinds, `WP_OP_GROUP_BEGIN`/
`WP_OP_GROUP_END` (`wp_undo_group_begin`/`end`, `undo.c`), carry no
payload -- pure brackets on the stack, not edits. `wp_edit_undo`/`redo`
became a **depth-counted loop**: a plain op (not part of any group)
still pops-applies-pushes in exactly one iteration, byte-for-byte the
same behavior as before this change (verified: every pre-existing
insert/delete/split/merge test still passes unmodified). Hitting a
`WP_OP_GROUP_END` while undoing increments a depth counter instead of
applying anything; the loop then keeps consuming ops -- applying each
one's inverse -- until a `WP_OP_GROUP_BEGIN` brings depth back to 0,
which (nesting is always well-formed, since every `group_end` call has a
matching prior `group_begin`) is guaranteed to be this group's own
opening marker, not some earlier, unrelated one. Redo mirrors this
exactly: undo pushes a group's markers onto the redo stack in the same
order it popped them from the undo stack, which (LIFO twice) restores
their ORIGINAL chronological order -- so redo walks forward through
that same sequence, with `GROUP_BEGIN` and `GROUP_END`'s roles swapped
(entering vs. leaving) since it's now traversing forward instead of
backward. Depth-counting rather than "stop at the first marker" is what
makes **nested** groups compose -- not yet exercised by
`wp_edit_delete_selection` itself (which only ever produces one flat,
unnested group), but load-bearing for `docs/vimode.md`'s Insert-mode
session grouping, which wraps an outer group around zero or more
already-grouped operations (e.g. `cw`'s own leading
`wp_edit_delete_selection`).

## Undo/Redo Stacks

Two stacks, both built from `wp_op.next` as the link field (each node
belongs to exactly one stack at a time):

- Applying a new edit pushes its undo-op onto the **undo stack** and
  clears the **redo stack** (any new edit invalidates prior redo
  history -- standard editor behavior).
- Undo pops the undo stack, applies the inverse, pushes the same node
  onto the redo stack.
- Redo pops the redo stack, re-applies the forward op (using the same
  span/position -- the pool bytes are still there, append-only), pushes
  it back onto the undo stack.
