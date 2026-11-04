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
