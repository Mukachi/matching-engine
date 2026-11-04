# 005 — Measurement and the report

**Status:** Agreed · **Updated:** 2026-11-04

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

- [x] The step conversion, shared by both programs
- [x] `day_bench` (S1) and `ring_bench` (S2)
- [x] `run.py`'s sets and the README's blocks (S3)
- [ ] Shep's runs; review

## Review

| Date | Stage | Finding |
|---|---|---|
| 2026-11-04 | Draft | Written from note 001 (S5, D5), notes 002 to 004 (the structures, matching, the replay's steps) and lockfree-ipc's open-loop generator (`load.hpp`: any system under test with `send` and `try_receive`, latency from the intended send time), read today |
| 2026-11-04 | Agreed | Approved by Shep, every decision as proposed (option A each): D1 the default and the reference on the day; D2 each step timed by kind, an untimed pass, memory, end state; D3 the ring under lockfree-ipc's open-loop generator at 1, 2 and 5 million a second beside no ring; D4 three runs each |
| 2026-11-04 | Agreed (building) | Built: `tools/steps.hpp` (the conversion: note 004's replay on the default book, its end state kept), `day_bench`, `ring_bench` (the engine inside the system under test's `try_receive`; the no-ring baseline waits for each step's intended time on CPU 2), `run.py`'s day and ring sets and the README's blocks. Trials (not bench mode): the day, 417,219,234 steps, both engines ended as the conversion did (8,430,655 trades); the default 9.06 M steps a second, p50 120 ns, p99.9 654, max 35 us, 1,321 MB; the reference 2.56 M, p50 378, max 122 ms; 12 minutes a run. By step, an enter is 151 ns p50 against note 002's add at 105: the engine reads the other side's best order first (does it cross?). The ring: 1 M/s kept, p50 372 ns but p99 12.7 us against no ring's 142 / 492; 2 and 5 M/s refused (producer lateness p99 4.4 us, 809 us). Isolated with a scratch copy at 1 M/s: the ring with the engine's work skipped had p99 11.6 us too, so the tail is the ring and the machine outside bench mode (lockfree-ipc's one-way p99.9 252-282 ns in bench mode), not the engine; to see in bench mode |
| 2026-11-04 | Agreed (building) | Ring check run at c0219ab (bench mode) clean: the trial's tail was the machine outside bench mode. Published: day (20261104T140004Z, 141628Z, 143251Z) and ring (141234Z, 142857Z, 144522Z), all at c0219ab. The day: default 8.74-8.82 M steps a second, p50 124-125 ns, p99.9 671-679, max 17.7-19.2 us, 1,321 MB; reference 2.48-2.49 M, p50 390, max 127 ms; both ended as the conversion did. By step (default): enter 158-159 ns p50 (note 002's add 105: the crossing check), execute 82-86, cancel 106-107, replace 257-258. The ring: 1 M/s p50 278-309 ns against no ring's 133 (+145-176), p99.9 873-903 against 666-669; 2 M/s 272-292 against 133-134, p99.9 850-873 against 689-690; producer lateness p99 40-74 ns; 5 M/s refused every run (lateness p99 0.9-1.3 ms), and with no ring its p99.9 is 560-584 us: the engine does not keep 5 M/s over the day's first 55 million steps (the open's), with or without the ring |
