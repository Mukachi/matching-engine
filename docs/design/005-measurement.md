# 005 — Measurement and the report

**Status:** Draft · **Updated:** 2026-11-04

## Proposal

Note 001's last measurements: the sample day through the engine per message (note 001, S5),
and the engine behind lockfree-ipc's SPSC ring, as a caller on another thread would drive it
(note 001, D5). Notes 002 and 003 measured each structure and each matching case alone; this
note measures the engine on the day's flow, as note 004 replays it, and what the ring costs in
front of it.

It does:

- the day per message: note 004's steps, converted once before anything is timed, through the
  engine and the reference engine, each step timed alone, by kind; throughput from an untimed
  pass; the most memory each held;
- the ring: the steps carried from a producer thread to the engine's thread through
  `lfipc::spsc_ring`, under lockfree-ipc's open-loop load (`load.hpp`), latency from each
  message's intended send time to the engine having applied it;
- both through `run.py report`, with the README's blocks.

It does not:

- re-measure the structures (note 002) or the matching cases (note 003);
- take orders over a network or a protocol (note 001: out of scope);
- run more than one engine thread (note 001, D5).

## Design

**The steps.** The day is read and decided once, before any timing, by note 004's replay on a
book of the default form: each step (24 bytes: kind, instrument, side, price, shares, ID) into
one array, about 10 GB, on the measured CPU's node. Every engine then gets the same steps, so
no timed window holds decoding or the reference map, and the engines end in the state note
004's lockstep test checked.

**Decisions** (each with what is proposed):

- **D1. Which engines on the day.** *Proposed (A):* the default (`me::book`, levels A with
  table A) and the reference engine: the other three combinations were measured on the day's
  book events in note 002 and on matching in note 003, and the day here adds the IOCs of
  note 004's executions (8.4 million) to the same flow. About 10 minutes a run. *Alternative
  (B):* all four combinations and the reference, as note 002: about 25 minutes a run.
- **D2. The day per message.** *Proposed (A):* as note 002's day: each step between `start()`
  and `stop()`, unsubtracted, into a histogram per kind (enter, cancel part, cancel, replace,
  execute, and the applied kinds of note 004's D1 together), the first million discarded; an
  untimed pass for steps a second; peak resident memory less what was there before. Each
  engine's end state checked against the conversion's (live orders, next ID, trades).
  *Alternative (B):* throughput only.
- **D3. The ring.** *Proposed (A):* lockfree-ipc's open-loop generator unchanged: the producer
  on CPU 3 sends `{seq, intended}` through `spsc_ring` (its default, v3) at a fixed rate; the
  system under test's `try_receive`, on CPU 2, takes a message and applies step `seq` to the
  engine before returning, so the latency recorded (intended send time to receipt) holds the
  ring and the engine's work; a rate the producer cannot keep (its lateness p99 above the send
  interval, as when the engine falls behind and the ring fills) is refused by the harness, as
  lockfree-ipc's note 003 refuses it, and published as refused.
  Rates 1, 2 and 5 million messages a second (the engine alone does ~10.9 million on the
  day's book events), each over the day's first 50 million steps after a warm-up; beside it,
  the same steps applied on CPU 2 with no ring at all, at the same rates, as the baseline the
  ring is compared with. *Alternative (B):* the ring at full speed only (throughput): no
  latency under load.
- **D4. Runs.** *Proposed (A):* three runs of each program in bench mode, published as ranges,
  as every set before. *Alternative (B):* one run each.

## Spec

- S1. The day per message, for the default engine and the reference: percentiles per step kind
  and over all steps (p50, p99, p99.9, max), steps a second, peak memory; each engine's end state
  equal to the conversion's.
- S2. The ring under open-loop load at D3's rates: latency percentiles from intended send time to
  the step applied, with the producer's lateness, beside the same steps applied with no ring;
  a rate the harness refuses published as refused, not left out.
- S3. Both published from at least three runs via `run.py report`, the README's figures from
  generated blocks only.

## Tasks

- [ ] The step conversion, shared by both programs
- [ ] `day_bench` (S1) and `ring_bench` (S2)
- [ ] `run.py`'s sets and the README's blocks (S3)
- [ ] Shep's runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-11-04 | Draft | Written from note 001 (S5, D5), notes 002 to 004 (the structures, matching, the replay's steps) and lockfree-ipc's open-loop generator (`load.hpp`: any system under test with `send` and `try_receive`, latency from the intended send time), read today |
