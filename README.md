# matching-engine

A limit order book and matching engine, the venue's side of the trade, written from Nasdaq's
public rulebook (Equity 4, Rule 4757, and Rule 4703) and measured on
[lockfree-ipc](https://github.com/Mukachi/lockfree-ipc)'s harness. Work in progress: the plan is
[design note 001](docs/design/001-scope.md); every component is a design note in [docs/](docs/README.md).

## Results

Figures are generated from committed runs by `python run.py report`; every run states its machine and
conditions in [RESULTS.md](RESULTS.md).

<!-- results:book-summary -->
The books: not yet measured (design note 002).
<!-- /results -->
