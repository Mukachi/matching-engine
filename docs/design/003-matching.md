# 003 — Matching and the engine

**Status:** Agreed · **Updated:** 2026-11-04

## Proposal

Note 001's matching on note 002's books: an incoming order meets the other side's best price
first and, at that price, its oldest order first; each trade is at the resting order's price
and decrements both orders; what is left of the incoming order rests, or, if it is
immediate-or-cancel, is cancelled. Every message is answered with its reports, in sequence: the
order accepted or refused, each trade, and the remainder rested or cancelled. After every
message no book is crossed.

It does:

- the engine over the books (`me::book`, note 002's default, and its other combinations): enter
  (limit, resting or immediate-or-cancel), cancel part or whole, replace, each matched as the
  rules say and reported;
- the reports, per message and in sequence, to a sink the caller gives (D2);
- a reference engine written for clarity, on note 002's reference book: the oracle and the
  baseline;
- tests of each rule built to break it, random streams against the reference, the call graph,
  and the per-operation figures of matching (D6).

It does not:

- replay the sample day as order entry or compare with Nasdaq's executions (note 004);
- put lockfree-ipc's ring in front, or measure the day per message (note 005);
- run auctions or trading states (halts, pauses): every instrument trades continuously (D5).

## Design

**The rules this note adds** (note 001, D2):

| Rule | Source | In the engine |
|---|---|---|
| Better price first | 4757(a)(1)(A) | the incoming order meets the other side's best level first, then the next |
| Equal price, displayed: time | 4757(a)(1)(B) | at a level, the head of its queue first |
| Decrementation | 4757(a)(2) | a trade reduces both orders by its shares; a resting order partly filled keeps its place |
| Trade at the resting price | 4757(a)(3) | a buy at 10.05 meeting an ask at 10.02 trades at 10.02; so a match never trades through a better resting price |
| Immediate or cancel | 4703(a)(1) | matched as far as it goes; the remainder cancelled, never rested |
| Marketable limit, no market orders | 4703(c) | a sweep of several levels is a limit priced through them; it stops at its limit |
| Replace is a new order | ITCH 5.0, Order Replace (note 002) | the old order removed, the new one entered as any new order: it matches if it crosses (D3) |
| Book never crossed | follows from the above | an order rests only when it no longer meets the other side |

**A message and its reports.** Each operation is one message and gives, in order: *accepted*
(the order's ID) or *refused* (note 002's reasons, nothing changed); then each *trade*
(the match number, one engine-wide sequence; taker's and maker's IDs; instrument, price,
shares; the maker's shares left); then *rested* (the remainder's shares at its price) or
*cancelled* (an IOC remainder; or a cancel's or replace's removal). A trade that empties the
maker removes it; the trade report says so (shares left 0).

**Decisions** (each with what is proposed):

- **D1. Where matching lives.** *Proposed (A):* the engine on three new book primitives, each
  O(1): the best level of a side, the head order of a level, and taking shares from the head
  (the head removed at zero, the level removed when emptied). The match at a level's head then
  touches no order table (note 001's S4). *Alternative (B):* the engine on note 002's operations
  as they are: each fill a `cancel(id, shares)`, so a table lookup per fill for an order the
  engine already holds.
- **D2. How reports leave.** *Proposed (A):* a sink the engine is templated on, called per
  report (`accepted`, `refused`, `trade`, `rested`, `cancelled`), inlined; the tests' sink
  records, the benchmark's counts, note 005's writes to the ring. Nothing stored by the engine.
  *Alternative (B):* the engine fills a fixed array of report records per message and returns
  it: a copy per report, and a sweep bounded by the array.
- **D3. A replace that crosses.** *Proposed (A):* matched as a new order would be (ITCH: a
  replace is a new order), then rests or not as its TIF says. *Alternative (B):* refused:
  simpler, but a rule of our own that no source gives.
- **D4. IDs.** *Proposed (A):* every accepted order takes the next ID, IOC included, so a trade
  names its taker the same way whether it rests or not; an IOC never enters the order table.
  Table A's capacity (open item M1) then counts IOCs too, stated beside the figures. *Alternative
  (B):* an IOC gets no engine ID; its trades name it by a caller's tag.
- **D5. Trading states.** *Proposed (A):* none: every instrument continuous, as note 001 leaves
  out auctions and halt crosses. The day's halts are note 004's (its adds in a halt rest without
  matching there). *Alternative (B):* a halt flag per instrument: orders rest, nothing matches,
  until it is lifted.
- **D6. What this note measures.** *Proposed (A):* matching's own cases, synthetic (one
  instrument, as note 002's), for each combination of note 002's D1 and D2 and the reference,
  three runs published: a non-marketable add; a match of one resting order (filled whole, and in
  part); a sweep of 1, 5 and 20 levels; an IOC that meets nothing; a replace that crosses. The
  day per message is note 005's. *Alternative (B):* correctness only here; every figure in
  note 005.

**The reference engine.** On `reference_market` (maps of lists): an incoming order loops over the
other side's map from its best, over each list from its front, written to be read, no capacity.
The same reports to the same sink.

## Spec

- S1. Each rule in the table above has a test built to break it, and passes; a refused message
  reports only its refusal and changes nothing.
- S2. Random streams (fixed seeds, at least 10 million messages over books of thousands of
  levels, with marketable orders, IOCs, sweeps and crossing replaces among them): every
  combination of note 002's D1 and D2 and the reference engine give the same reports in the same
  sequence after every message, and every level touched equal; every book in full every 100,000
  messages and at the end; no book crossed after any message.
- S3. Enter (matching included), cancel and replace make no allocation and no system call (the
  compiled call graph checked, as note 002's S3).
- S4. D6's cases as percentiles over at least three runs via `run.py report`, each combination
  and the reference.

## Tasks

- [x] Book primitives (D1): best level, head order, take from the head
- [x] The engine: enter (limit, IOC), cancel, replace (D3), reports to a sink (D2), IDs (D4)
- [x] The reference engine
- [x] Tests: the rules (S1), random streams (S2), the call graph (S3)
- [x] Benchmark of D6's cases and `run.py` (S4)
- [ ] Shep's runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-11-04 | Draft | Written from note 001 (D2's rules not yet in a book: decrementation, the resting price, IOC, sweeps) and note 002 (the books, its default `me::book`). Not in either: how a replace that crosses is handled (D3), whether an IOC takes an ID (D4), trading states (D5): each proposed here |
| 2026-11-04 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 the engine on three O(1) book primitives; D2 reports to a sink the engine is templated on; D3 a crossing replace matched as a new order; D4 every accepted order takes an ID, IOC included; D5 no trading states; D6 matching's synthetic cases published here, the day per message in note 005 |
| 2026-11-04 | Agreed (building) | Built: book primitives in `market.hpp` (D1: `best_head`, record accessors, `reduce` for any order, `can_rest`, `skip_id`; `can_insert` per levels form), `engine.hpp` (enter, cancel part and whole, replace; reports to the sink, D2), `reference_engine.hpp` on `reference_market` (its `best` added). A decision the Draft did not make, taken as the simplest that keeps D6 of note 002 (a refusal changes nothing): an order that may rest is refused before it matches if it could not rest (no ID, record or level), even when it would fill whole; an IOC needs none (tested). Tests: `engine_test` (S1, 79 checks over the four combinations and the reference; planted bugs caught: trade at the taker's price, a buy not meeting an ask at its limit, an IOC resting), `engine_random_test` (S2: 10 million messages over two seeds, 4.46 million trades, 616,000 messages taking more than one order, a million IOCs, 299,000 crossing replaces, sides to 4,834 levels; a crossing replace not matched caught at message 75), `check_hot_path` (S3: 16 engine subjects added, 32 in all; a sink that allocates caught), `match_bench` and `run.py`'s match set (S4). ASan and UBSan clean. Trial (not bench mode): a match of one order ~40 ns on every book, a sweep ~12-15 ns an order taken with table A, twice that with table B (an erase in the hash per order); the reference's sweeps not linear in orders taken (270 ns an order over 1 level, 330 over 5, 115 over 20; p99 71 us at 20), to see in bench mode |
