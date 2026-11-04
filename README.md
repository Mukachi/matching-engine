# matching-engine

A limit order book and matching engine, the venue's side of the trade, written from Nasdaq's
public rulebook (Equity 4, Rule 4757, and Rule 4703) and measured on
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc)'s harness. Work in progress: the plan is
[design note 001](docs/design/001-scope.md); every component is a design note in [docs/](docs/README.md).

## Results

Figures are generated from committed runs by `python run.py report`; every run states its machine and
conditions in [RESULTS.md](RESULTS.md).

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

<!-- results:day-summary -->
| Engine | M steps/s | p50 | p99 | p99.9 | max | Peak memory, MB |
|---|---|---|---|---|---|---|
| reference | 2.48–2.49 | 390 | 1,236–1,243 | 1,785–1,794 | 126.8–127.0 ms | 261 |
| levels A (sorted array), table A (direct) | 8.74–8.82 | 124–125 | 411–416 | 671–679 | 17.7 µs–19.2 µs | 1,321 |

The sample day as order entry, each step timed alone on one CPU; ns, range over runs. By step and the method: [RESULTS.md](RESULTS.md).
<!-- /results -->

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
