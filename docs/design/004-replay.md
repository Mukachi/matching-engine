# 004 — The replay and the oracle

**Status:** Agreed · **Updated:** 2026-11-04

## Proposal

Note 001's D3: the sample day replayed as order entry through the engine (note 003), so its
matching meets real flow, and Nasdaq's own executions are a second oracle that the reference
engine cannot share our mistakes with. Each add, cancel, delete and replace is entered as
itself; each execution is entered as an aggressive order on the other side, at the resting
order's price, for the shares executed. If our priority is Nasdaq's, the order we match is the
one Nasdaq executed. Every execution we reproduce is counted, and every one we do not is
counted by its cause.

It does:

- the replay: the day's messages turned into the engine's messages, with the day's reference
  numbers mapped to the engine's IDs outside the engine (as note 002's day test);
- the comparison with Nasdaq: each execution (E, and C) reproduced or not, each miss by cause;
- the whole day through every combination of note 002's D1 and D2 and the reference engine in
  lockstep, and every book against the feed handler's reference book;
- the counts published through `run.py`.

It does not:

- time the replay (note 005: the day per message, and lockfree-ipc's ring in front);
- infer the day's aggressors: the feed never shows them, so one real incoming order is, in the
  replay, an IOC per execution it caused and an add for what it left resting;
- model what the feed does not show: non-displayed interest, crosses' order imbalance, the
  rule's changes since 2020 (each a cause, below).

## Design

**What the day gives** (ITCH 5.0; the feed handler's note 003 counted them): 423,285,709
messages; adds (A, F), Order Executed (E), Executed With Price (C), cancels (X), deletes (D),
replaces (U); about 10.3 million executions, of them 8,615 non-printable C (a cross's
executions, reported in bulk by its Cross Trade, Q); trading states per instrument (H, 8,921);
every crossed book of the day began in a halt or pause (the feed handler's note 002, D6).

**The mapping:**

| Day | Engine |
|---|---|
| A, F: an order rests | `enter` (rests until cancelled) at its side, price and shares |
| X: shares cancelled | `cancel(id, shares)` |
| D: deleted | `cancel(id)` |
| U: replaced | `replace(id, price, shares)`; the new reference mapped to the new ID |
| E: executed | `enter` IOC on the other side, at the order's price, for the shares executed |
| C: executed with a price | as E, at the order's price (the execution's own price compared apart) |

**Decisions** (each with what is proposed):

- **D1. Keeping the books Nasdaq's.** Once our engine matches a different order from Nasdaq's,
  our book is no longer Nasdaq's, and every later comparison inherits the difference.
  *Proposed (A): predict, then enter or apply.* Before each message the replay asks the
  engine's book what entering it would do: for an execution, whether the executed order is the
  first the IOC would meet (the head of the other side's best level, with the shares); for an
  add, that it does not cross. When the engine would do what Nasdaq did, the message is
  entered, through matching; when not, Nasdaq's outcome is applied to the book directly (the
  execution as note 002's cancel of part, the add as note 002's add, resting without matching)
  and the message counted by cause. So the books stay Nasdaq's after every message, and every
  execution is judged on the book Nasdaq had. *Alternative (B):* everything entered, the books
  left to diverge, agreement counted: after the first miss the counts mean little.
- **D2. The causes of a miss.** *Proposed (A):* each execution not reproduced is put in the
  first class it fits, in this order, and each add that would cross likewise:
  1. *a cross*: a non-printable C (the open, the close, a halt's cross);
  2. *not trading*: the instrument halted, paused or quoting only (H);
  3. *a better price ahead*: the executed order is not at the best price of its side in our
     book (a crossed or locked book);
  4. *earlier orders ahead*: at the best price, but not first in time (non-displayed interest
     or orders at the head that Nasdaq did not execute; the rule's changes since 2020);
  5. *unexplained*: each one an open item until explained (note 001, S3).

  A printable C at a price other than its order's is counted apart as well, reproduced or not.
  *Alternative (B):* misses counted, not classified.
- **D3. The books in lockstep.** *Proposed (A):* every combination of note 002's D1 and D2 and
  the reference engine, given the same messages: the same reports after every message, every
  level touched equal; every book in full every 50 million messages and at the end equal to the
  reference and the reference to the feed handler's reference book (as note 002's S4). The
  prediction (D1) is made on the reference engine's book. *Alternative (B):* the default book
  and the reference only: faster (a whole day is ~25 minutes with all four).
- **D4. Publishing the counts.** *Proposed (A):* a program, `replay_oracle`, runs the day (the
  default book, the prediction of D1) and writes an `oracle` block through lockfree-ipc's
  harness: executions reproduced, misses by cause, adds applied by cause, and the IDs taken
  (table A's capacity, open item M1: the replay's IOCs take IDs too). The counts do not depend
  on timing, so one run is published (a second checks they repeat); `run.py` writes them.
  *Alternative (B):* the counts in the day test's output and the note's Review only: typed in
  by hand, against the established rule.

## Spec

- S1. The mapping and D1's prediction tested on hand-made days, each case built to break it: an
  execution reproduced; one of an order behind another at its price; one at a worse price than
  the best; a non-printable C; an execution in a halt; an add that would cross; a replace's new
  reference.
- S2. The whole day: every combination of note 002's D1 and D2 and the reference engine give the
  same reports after every message, every level touched equal; at every 50 million messages and
  at the end every book equal to the reference and the reference to the feed handler's reference
  book.
- S3. Of the day's executions, the share reproduced (the same order, the same shares) published;
  every one not reproduced counted by D2's cause; every unexplained one an open item.
- S4. The counts published via `run.py report` from `replay_oracle`'s run, repeated exactly by a
  second run.

## Tasks

- [x] The replay (the mapping, the reference map, D1's prediction), shared by the test and the
      program
- [x] Tests: hand-made days (S1)
- [x] The day in lockstep (S2, label `data`)
- [ ] `replay_oracle` and `run.py`'s oracle set (S3, S4)
- [ ] Shep's runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-11-04 | Draft | Written from note 001 (D3, S2, S3) and the feed handler's notes 002 and 003 (the day's counts: ~10.3 million executions, 8,615 non-printable C, 8,921 trading actions; crossed books begin in halts). Found in writing: entering every execution lets one miss spoil every later comparison (D1); the day's IOCs take IDs, so table A's capacity (M1) is used further, about 233 million of 268 million |
| 2026-11-04 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 predict, then enter or apply; D2 misses by cause in order; D3 every combination and the reference in lockstep; D4 `replay_oracle` publishes the counts, one run repeated by a second |
| 2026-11-04 | Agreed (building) | Built: `tools/replay.hpp` (the mapping, the reference map, D1's prediction, D2's causes; a replace that would cross rests through the book's replace, counted as adds are), `replay_test` (S1, 11 checks), `replay_day_test` (S2: the whole day, 417,219,234 steps, every combination equal to the reference engine after each, every book equal to the reference and to the feed handler's reference book at 9 checkpoints, up to 1,871,048 orders live), `replay_oracle` and `run.py`'s oracle set (refuses runs whose counts differ). Found: the day tests passed on an empty day (the gzip pipe opens before the file is read): both now fail with no messages (note 002's `day_test` too). The Draft's ~10.3 million executions was the feed handler's trade count (non-displayed trades included): the day has 8,555,084 executions of displayed orders. Trial: 8,430,655 reproduced (98.55%), none matched the wrong order; not reproduced: cross 4,726, earlier orders ahead 119,703, no other cause; 2,180 adds would have crossed, all in a halt; IDs 231,818,732 of 268,435,456 (86%). Throwaway diagnostics of the 119,703: 108,888 are printable C at a price other than the order's (83% of the day's 130,855; ITCH 5.0 sends C when an order executes at a price other than its display price, so ranked at a price it did not show); 10,815 are E (0.13% of E), every one of the 152,779 orders ahead of them an add (36.6 million orders born of replaces marked, 132,002 of them same price and fewer shares: none ahead of a miss), so a size-down replace keeping its place is ruled out; cause not found. For Shep: a class for C at another price (D2) |
