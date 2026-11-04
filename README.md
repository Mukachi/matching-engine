# matching-engine

A limit order book and matching engine, the venue's side of the trade, in C++20 for Linux,
written from Nasdaq's public rulebook: price, then time, among displayed orders (Equity 4, Rule
4757(a)(1)), every trade at the resting order's price (4757(a)(3)), immediate-or-cancel and the
sizes and prices of Rule 4703. A real trading day, Nasdaq's sample TotalView-ITCH 5.0 file of 30
January 2020, is replayed through it as order entry, and Nasdaq's own executions that day are the
oracle: of 8,555,084, the engine matches the very order Nasdaq executed for 98.55%, and every
other is counted by its cause. Each structure is built in more than one form and measured against
the others and against a simple reference implementation, on
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc)'s harness. Where something is
unexplained, the notes say so.

## Results

Ranges over three runs on a dedicated, isolated machine. Full tables, the method and every
run's conditions: [RESULTS.md](RESULTS.md). These blocks are generated from the committed JSON in
`results/` by `python run.py report`; the test suite fails if they disagree.

The sample day against Nasdaq's executions ([note 004](docs/design/004-replay.md)):

<!-- results:oracle-summary -->
| Executions of the day | Count | Share |
|---|---|---|
| reproduced (the order Nasdaq executed met first, for its shares) | 8,430,655 | 98.55% |
| not reproduced: cross | 4,726 | 0.06% |
| not reproduced: not trading | 0 | 0.00% |
| not reproduced: better price ahead | 0 | 0.00% |
| not reproduced: executed at another price | 108,888 | 1.27% |
| not reproduced: earlier orders ahead | 10,815 | 0.13% |
| not reproduced: unexplained | 0 | 0.00% |

The sample day replayed as order entry: of its 8,555,084 executions, how many the engine matched exactly as Nasdaq did, and every other by its cause. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The sample day per message through the engine and the reference engine
([note 005](docs/design/005-measurement.md)):

<!-- results:day-summary -->
| Engine | M steps/s | p50 | p99 | p99.9 | max | Peak memory, MB |
|---|---|---|---|---|---|---|
| reference | 2.48–2.49 | 390 | 1,236–1,243 | 1,785–1,794 | 126.8–127.0 ms | 261 |
| levels A (sorted array), table A (direct) | 8.74–8.82 | 124–125 | 411–416 | 671–679 | 17.7 µs–19.2 µs | 1,321 |

The sample day as order entry, each step timed alone on one CPU; ns, range over runs. By step and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The engine behind lockfree-ipc's SPSC ring under open-loop load, beside the same steps with no
ring ([note 005](docs/design/005-measurement.md)):

<!-- results:ring-summary -->
| Rate |  | p50 | p99 | p99.9 | max | Producer lateness p99 |
|---|---|---|---|---|---|---|
| 1M/s | ring (3 of 3 runs) | 278–309 | 637–668 | 873–903 | 18.5 µs–35.0 µs | 41–74 |
| 1M/s | no ring | 133 | 412–413 | 666–669 | 17.0 µs–18.7 µs |  |
| 2M/s | ring (3 of 3 runs) | 272–292 | 576–597 | 850–873 | 4.2–18.1 µs | 40 |
| 2M/s | no ring | 133–134 | 421–423 | 689–690 | 4.1–17.6 µs |  |
| 5M/s | ring refused (3 of 3 runs) |  |  |  |  | 0.9–1.3 ms |
| 5M/s | no ring | 233–234 | 40.9 µs–42.8 µs | 560.1 µs–584.4 µs | 703.4 µs–707.9 µs |  |

The engine behind lockfree-ipc's SPSC ring under open-loop load, beside the same steps with no ring; ns, range over runs. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

Matching, case by case, on every form of the book ([note 003](docs/design/003-matching.md)):

<!-- results:match-summary -->
| Case | Messages timed | reference | levels A (sorted array), table A (direct) | levels A (sorted array), table B (open addressing) | levels B (std::map, pooled), table A (direct) | levels B (std::map, pooled), table B (open addressing) |
|---|---|---|---|---|---|---|
| add that does not match | 1,000,000 | 95–96 / 113–114 | 59–61 / 60–70 | 159–162 / 492–493 | 55–56 / 56 | 155–158 / 477–491 |
| match one order, filled whole | 1,000,000 | 100–101 / 106–108 | 36 / 37 | 36–37 / 186–193 | 35 / 35–37 | 36–39 / 164–187 |
| match one order, in part | 1,000,000 | 52–53 / 54–55 | 34 / 35 | 33 / 34–35 | 27–32 / 33–35 | 28–35 / 33–35 |
| sweep 1 level (10 orders) | 100,000 | 950–952 / 973–982 | 159–160 / 166–177 | 298–299 / 694–708 | 138–139 / 146–164 | 290–299 / 689–702 |
| sweep 5 levels (50 orders) | 100,000 | 5,803–5,846 / 5,934–5,970 | 699–842 / 729–868 | 1,510–1,567 / 2,238–2,304 | 652–667 / 703–790 | 1,629–1,656 / 2,322–2,377 |
| sweep 20 levels (200 orders) | 100,000 | 25.0 µs / 25.3 µs–25.6 µs | 2,752–2,865 / 2,876–2,992 | 6,349–6,494 / 7,528–7,688 | 2,621–2,632 / 2,723–2,843 | 6,873–6,931 / 7,994–8,052 |
| IOC that meets nothing | 1,000,000 | 35–36 / 35–36 | 27 / 32 | 25 / 29–30 | 25 / 28–29 | 25–26 / 29–31 |
| replace that crosses | 1,000,000 | 174–176 / 180–191 | 42–43 / 47 | 49–50 / 364–369 | 40 / 41–52 | 46–51 / 367–376 |

Matching's synthetic cases, each message timed alone on one CPU; ns, p50 / p99, range over runs. The method: [RESULTS.md](RESULTS.md).
<!-- /results -->

The books alone on the day's book events, every form of the order table and the price levels
([note 002](docs/design/002-book.md)):

<!-- results:book-summary -->
| Book | M operations/s | p50 | p99 | p99.9 | max | Peak memory, MB |
|---|---|---|---|---|---|---|
| reference | 2.55–2.57 | 380–381 | 1,208–1,210 | 1,762 | 125.2–125.3 ms | 261 |
| levels A (sorted array), table A (direct) | 10.89–10.94 | 112 | 380–381 | 627–628 | 18.1 µs–18.4 µs | 1,321 |
| levels A (sorted array), table B (open addressing) | 6.10–6.12 | 189 | 611 | 887–889 | 18.6 µs–40.6 µs | 425 |
| levels B (std::map, pooled), table A (direct) | 6.11–6.13 | 136 | 602 | 912–916 | 18.1 µs–18.5 µs | 1,321 |
| levels B (std::map, pooled), table B (open addressing) | 4.20–4.23 | 229 | 853–854 | 1,243–1,245 | 18.8 µs–21.1 µs | 425 |

The sample day's displayed book events as operations, each timed alone on one CPU; ns, range over runs. Each operation, the synthetic cases and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

## What is here

Header-only, in `include/me/`. `market.hpp` is the books: one per instrument, each side's price
levels (a sorted array in a shared arena, or `std::map` on a node pool), each level's queue an
intrusive list through order records from a pool, an order found by its ID (a direct array, or
open addressing with Robin Hood ordering). `me::book` is the default, the sorted array with the
direct array, chosen from the figures. `engine.hpp` matches over it: an incoming order meets the
other side's best price first and, there, the oldest order; it trades at the resting price, rests
what is left or, if immediate-or-cancel, cancels it; a replace is a new order and matches if it
crosses. Every message's reports, in sequence, go to a sink the engine is templated on.
`reference_market.hpp` and `reference_engine.hpp` are the same rules written for clarity with
standard containers: the oracle for every test and the baseline for every figure.

Every pool is reserved at start. Add, cancel, replace and matching allocate nothing and make no
system call, and a test checks the compiled call graph.

## Correctness

- **Every rule, a test built to break it.** Each clause the engine implements, and each refusal,
  has a test that a planted bug fails: a trade at the taker's price, a buy not meeting an ask at
  its limit, an IOC that rests, a remainder resting without room.
- **Random streams against the reference.** Ten million messages over two seeds, sweeps, IOCs and
  crossing replaces among them: every form of the book gives the reference engine's reports after
  every message, and no book is ever crossed.
- **The whole day, three ways.** All 417 million book events through every form and the reference
  in lockstep; every book, at every checkpoint, equal to the feed handler's reference book, built
  independently in [itch-feed-handler](https://github.com/Mukachi/itch-feed-handler); and Nasdaq's
  executions as a second oracle the reference cannot share our mistakes with.
- **Sanitizers.** The tests run clean under AddressSanitizer and UBSan.

## Findings

- **Nasdaq's executions.** 98.55% reproduced exactly. 1.27% are Nasdaq executing an order at a
  price other than the one it displayed (ITCH's Order Executed With Price): the order was ranked
  at a price the feed never shows. 0.13%, executions with earlier orders ahead at the same price,
  are not explained; the obvious cause, a replace keeping its place when only its size falls, is
  ruled out (note 004; [open items](docs/open.md)).
- **A structure that looked right failed on real flow.** An order table indexed by the ID's low
  bits, meant to keep the direct array's locality in bounded memory, ran at 0.5 million operations
  a second on the day against 10.9: once the IDs wrap the table, the day's long-resting orders
  make probe runs thousands long. It was built, measured and dropped (note 002).
- **Room only for what rests.** The first engine refused an order that would fill whole if the
  book had no room for it to rest. It now matches first and cancels only a remainder with no room;
  matching became 6 to 12 ns faster (note 003).
- **Code layout moves the figures.** That change left a sweep's work the same and moved its time
  by about 10%, up for one form of the book and down for another; sweep differences under 10% are
  not design differences (note 003; open items).
- **The ring costs about 150 ns at the median.** Behind lockfree-ipc's SPSC ring the engine keeps
  1 and 2 million messages a second; 5 million is beyond it over the open, with or without the
  ring (note 005).

## How it is measured

- **Per operation, unsubtracted.** Every operation or message timed alone with the TSC, after a
  discarded warm-up, into a histogram; percentiles only. The day is converted once before any
  timing, so no timed window holds decoding or the map from Nasdaq's reference numbers.
- **The harness** is lockfree-ipc's, fetched at a pinned commit: the noise floor and the
  machine's conditions in every run, the open-loop load generator unchanged, and a refusal to
  publish unless the machine is in bench mode and the build is clean.
- **The machine.** HPE ProLiant DL380 Gen9, 2 × Xeon E5-2699 v4, Ubuntu 26.04, kernel 7.0, set
  up as lockfree-ipc describes ([its machine scripts](https://github.com/Mukachi/lockfree-ipc/tree/main/tools/machine)):
  reserved, isolated CPUs on node 0, with services, kernel threads and interrupts fenced off them.

## The sample day

Nasdaq publishes sample days of TotalView-ITCH 5.0 at emi.nasdaq.com without stated terms, so
none is in this repository and none is redistributed. The feed handler's script, fetched with it
by the first `cmake` (below), downloads the one used here (5.6 GB, size and SHA-256 checked) into
its own `data/`; move it into this repository's `data/`, which git ignores:

```sh
python build/_deps/itch_feed_handler-src/tools/fetch_day.py
mkdir -p data && mv build/_deps/itch_feed_handler-src/data/01302020.NASDAQ_ITCH50.gz data/
```

## Build and test

Linux; C++20; tested with GCC 15.2, CMake 4.2 and Python 3.14.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build -LE data
```

CMake fetches lockfree-ipc and itch-feed-handler at pinned commits into `build/`. Checks that
need the sample day carry the CTest label `data` (each takes about half an hour).

## Running the benchmarks

```sh
build/book_bench out.json      # the books on the day's book events (note 002)
build/match_bench out.json     # matching, case by case (note 003)
build/replay_oracle out.json   # the day against Nasdaq's executions (note 004)
build/day_bench out.json       # the day per message (note 005)
build/ring_bench out.json      # the engine behind the SPSC ring (note 005)
```

Each writes one JSON file: the environment, the noise floor, the clock, then the results. Run
without a path, a benchmark writes into `results/`, and refuses unless the machine is in bench
mode and the build is clean; given a path elsewhere, it runs anywhere and marks the file
unpublished. The CPU numbers are this machine's. To publish runs: list them in
`results/published.toml`, then `python run.py report`.

## Design notes

Every component has a design note written and agreed before its code: proposal, design, spec,
tasks, and a review log with every finding, including the wrong turns. Index:
[docs/README.md](docs/README.md).

## Licence

[MIT](LICENSE). Nasdaq, TotalView and ITCH are Nasdaq's; this is an independent implementation
of their published rules and specifications.
