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
  && start.run == end.run`). Deleting a selection that spans multiple
  runs or paragraphs, and paragraph split/merge (Enter / Backspace-
  joining-two-paragraphs) are explicitly **not implemented here** --
  `WP_OP_SPLIT_PARA`/`WP_OP_MERGE_PARA` remain defined-but-unused enum
  values for that follow-up. This matches the literal T2.2 AC ("tests
  cover start/middle/end of run **and paragraph**" -- i.e. where within
  a paragraph you edit, not selections spanning paragraphs).

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
