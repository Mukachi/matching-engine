#!/usr/bin/env python3
"""Results report: RESULTS.md and the README's figures, generated from committed JSON.

  python run.py report          write RESULTS.md and fill the README's result blocks
  python run.py report --check  regenerate in memory; exit 1 naming any difference

As the repos before it (lockfree-ipc's note 007), and built on lockfree-ipc's
report: its environment checker, conditions table and formatting are used from
the copy CMake fetched at the pinned commit (build/_deps/lockfree_ipc-src), so a
run here is held to the same rules. Only files listed in results/published.toml
are read, each validated first: environment checker, published, bench mode,
clean build, its component's block present, one commit, machine and kernel
command line per set. Figures are min-max over a set's runs. Standard library
only.
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
import tomllib
from pathlib import Path

SETS = ("book", "match", "oracle")
BLOCK_OF = {}
PATH_OF = {}

BLOCK = re.compile(r"(<!-- results:(?P<name>[a-z0-9-]+) -->\n)(?P<body>.*?)(<!-- /results -->)", re.S)
MEAN = re.compile(r"\b(mean|average|avg)\b", re.I)


class ReportError(Exception):
    pass


def harness(root, lfipc=None):
    """lockfree-ipc's run.py, from the fetched copy, as a module."""
    path = (Path(lfipc) if lfipc else root / "build" / "_deps" / "lockfree_ipc-src") / "run.py"
    if not path.is_file():
        raise ReportError(f"{path} missing: configure the build first (cmake -S . -B build)")
    spec = importlib.util.spec_from_file_location("lfipc_run", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod, path.parent


def validate(root, lf_root, name, files):
    runs = []
    for f in files:
        path = root / "results" / f
        if not path.is_file():
            raise ReportError(f"{name}: {f}: missing")
        check = subprocess.run([sys.executable, "-I", str(lf_root / "tests" / "check_environment_json.py"), str(path)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            fails = [l for l in check.stdout.splitlines() if l.startswith("FAIL")]
            raise ReportError(f"{name}: {f}: environment check failed: {fails[:2]}")
        d = json.loads(path.read_text())
        e = d["environment"]
        if not (e["published"] and e["ok"] and e["bench_mode"]["ok"]):
            raise ReportError(f"{name}: {f}: not a published run (refused or outside results/)")
        if not e["build"]["clean"]:
            raise ReportError(f"{name}: {f}: built from a tree with uncommitted changes")
        block = BLOCK_OF.get(name, name)
        if block not in d or not d.get("clock", {}).get("ok"):
            raise ReportError(f"{name}: {f}: no {block} block, or the clock refused")
        if name in PATH_OF and d[block]["path"] != PATH_OF[name]:
            raise ReportError(f"{name}: {f}: a {d[block]['path']} run in the {PATH_OF[name]} set")
        runs.append((f, d))
    if not runs:
        raise ReportError(f"{name}: no runs listed")
    for key, get in (("commit", lambda e: e["build"]["commit"]),
                     ("machine", lambda e: (e["machine"]["product"], e["cpu"]["model"], e["cpu"]["microcode"])),
                     ("kernel command line", lambda e: e["kernel"]["cmdline"])):
        if len({get(d["environment"]) for _, d in runs}) > 1:
            raise ReportError(f"{name}: runs mix more than one {key}")
    return runs


def load_sets(root, lf_root):
    manifest = root / "results" / "published.toml"
    if not manifest.is_file():
        return {}
    m = tomllib.loads(manifest.read_text())
    unknown = set(m) - set(SETS)
    if unknown:
        raise ReportError(f"published.toml: unknown sets {sorted(unknown)}")
    return {n: (m[n].get("note", ""), validate(root, lf_root, n, m[n]["runs"])) for n in SETS if n in m}


# ---------------------------------------------------------------- note 002


def books(runs):
    """Each book's results over the runs, in the runs' order; every run the same books."""
    names = [b["name"] for b in runs[0][1]["book"]["books"]]
    for f, d in runs:
        if [b["name"] for b in d["book"]["books"]] != names:
            raise ReportError(f"book: {f}: different books")
        for b in d["book"]["books"]:
            if not b["nothing_refused"]:
                raise ReportError(f"book: {f}: {b['name']}: refused some operations")
    return [(n, [d["book"]["books"][i] for _, d in runs]) for i, n in enumerate(names)]


def label(name):
    return name if name == "reference" else name.replace("levels A: sorted array", "levels A (sorted array)") \
        .replace("levels B: std::map, pooled", "levels B (std::map, pooled)").replace("table A: direct array", "table A (direct)") \
        .replace("table B: open addressing", "table B (open addressing)")


def lat(lf, values):
    """As lf.rng, but a range reaching 1 ms written in ms at both ends."""
    if max(values) < 1_000_000:
        return lf.rng(values)
    lo, hi = f"{min(values) / 1e6:,.1f}", f"{max(values) / 1e6:,.1f}"
    return f"{lo} ms" if lo == hi else f"{lo}–{hi} ms"


def entry(bs, part, key, value):
    return [next(x for x in b[part] if x[key] == value) for b in bs]


def day_rows(lf, runs):
    rows = []
    for name, bs in books(runs):
        all_ = entry(bs, "day", "op", "all")
        rows.append([label(name), lf.rng([b["ops_per_s"] / 1e6 for b in bs], lambda v: f"{v:.2f}")]
                    + [lat(lf, [x[f] for x in all_]) for f in ("p50_ns", "p99_ns", "p999_ns", "max_ns")]
                    + [lf.rng([b["peak_resident_mb"] for b in bs], lambda v: f"{v:,.0f}")])
    return rows


DAY_HEAD = ["Book", "M operations/s", "p50", "p99", "p99.9", "max", "Peak memory, MB"]
OPS = ("add", "cancel part", "cancel", "replace")


def op_rows(lf, runs):
    rows = []
    for name, bs in books(runs):
        row = [label(name)]
        for op in OPS:
            x = entry(bs, "day", "op", op)
            row += [lf.rng([v["p50_ns"] for v in x]), lf.rng([v["p99_ns"] for v in x])]
        rows.append(row)
    return rows


OP_HEAD = ["Book"] + [h for op in OPS for h in (f"{op} p50", "p99")]


def synthetic_rows(lf, runs):
    bks = books(runs)
    cases = [c["case"] for c in bks[0][1][0]["synthetic"]]
    rows = []
    for c in cases:
        row = [c]
        for _, bs in bks:
            x = entry(bs, "synthetic", "case", c)
            row.append(f"{lf.rng([v['p50_ns'] for v in x])} / {lf.rng([v['p99_ns'] for v in x])}")
        rows.append(row)
    return [label(n) for n, _ in bks], rows


def book_section(lf, note, runs):
    b0 = runs[0][1]["book"]
    none = lf.rng([d["book"]["none"]["p50_ns"] for _, d in runs])
    names, srows = synthetic_rows(lf, runs)
    return [f"## The books (note {note})", "",
            f"The sample day's displayed book events ({b0['messages']:,} messages; {b0['operations']:,} operations, "
            f"{b0['left_out']:,} left out) as operations on the books: each Add an add, each Order Executed, Executed With "
            "Price and Order Cancel a cancel of part, each Order Delete a cancel, each Order Replace a replace, the engine's "
            "IDs resolved before anything is timed. One book after another on CPU "
            f"{b0['cpu']}: every operation timed alone, unsubtracted (an empty window's p50 is {none} ns), the first "
            f"{b0['warmup_discarded']:,} discarded; operations a second from a second, untimed pass; the most memory each "
            f"held. {len(runs)} runs; ranges over the runs.", "",
            lf.table(DAY_HEAD, day_rows(lf, runs)), "",
            "The default, `me::book`, is levels A with table A (note 002, S6). Its limit: table A takes one ID for every add "
            "and replace in a session, up to its configured capacity (2^28 by default, 1 GB); the sample day used about 223 "
            "million (83%), and past the capacity an add is refused (open item M1).", "",
            "Each operation of the day:", "",
            lf.table(OP_HEAD, op_rows(lf, runs)), "",
            f"Synthetic cases ({b0['synthetic_book']}), each {b0['synthetic_repetitions']:,} times after "
            f"{b0['synthetic_warmup']:,} of warm-up, each timed operation followed by an untimed one that puts the book back. "
            "The book stays in cache, so these are each operation's cost at its best; the day's are the realistic ones. "
            "p50 / p99:", "",
            lf.table(["Case"] + names, srows), ""]


# ---------------------------------------------------------------- note 003


def engines(runs):
    """Each engine's results over the runs, in the runs' order; every run the same engines."""
    names = [e["name"] for e in runs[0][1]["match"]["engines"]]
    for f, d in runs:
        if [e["name"] for e in d["match"]["engines"]] != names:
            raise ReportError(f"match: {f}: different engines")
        for e in d["match"]["engines"]:
            if not e["nothing_refused"]:
                raise ReportError(f"match: {f}: {e['name']}: refused some messages")
    return [(n, [d["match"]["engines"][i] for _, d in runs]) for i, n in enumerate(names)]


def match_rows(lf, runs):
    es = engines(runs)
    rows = []
    for c in es[0][1][0]["cases"]:
        row = [c["case"], f"{c['count']:,}"]
        for _, xs in es:
            v = entry(xs, "cases", "case", c["case"])
            row.append(f"{lat(lf, [x['p50_ns'] for x in v])} / {lat(lf, [x['p99_ns'] for x in v])}")
        rows.append(row)
    return ["Case", "Messages timed"] + [label(n) for n, _ in es], rows


def match_section(lf, note, runs):
    m0 = runs[0][1]["match"]
    none = lf.rng([d["match"]["none"]["p50_ns"] for _, d in runs])
    head, rows = match_rows(lf, runs)
    return [f"## Matching (note {note})", "",
            f"The engine over each book and the reference engine, synthetic cases on {m0['book']}, one engine after another on "
            f"CPU {m0['cpu']}: each message timed alone, unsubtracted (an empty window's p50 is {none} ns), after a warm-up of 1% "
            "of its repetitions, each followed by untimed messages that put the book back. A sweep takes whole levels and rests "
            "nothing; a replace that crosses fills the best ask's head whole. The book stays in cache: each message's cost at "
            f"its best. {len(runs)} runs; ranges over the runs. p50 / p99:", "",
            lf.table(head, rows), "",
            "A sweep's p50 moves by up to about 10% a book with the code's layout between builds, in either direction: the "
            "change to room for what rests (note 003's Review) left the sweep's work unchanged and moved levels A with table A "
            "up ~10% and levels B with table A down ~11%. A difference in sweeps under ~10% is not a design difference (open "
            "item M2).", ""]


# ---------------------------------------------------------------- note 004



def oracle(runs):
    """The oracle's counts: the same in every run (they do not depend on timing, S4), checked."""
    o = runs[0][1]["oracle"]
    for f, d in runs[1:]:
        if d["oracle"] != o:
            raise ReportError(f"oracle: {f}: counts differ from {runs[0][0]}")
    if o["reproduced_wrong"] or o["refused"] or o["no_room"] or o["unknown_references"]:
        raise ReportError(f"oracle: {runs[0][0]}: a reproduced execution matched the wrong order, or a message was refused or unknown")
    return o


def pct(n, of):
    return f"{100 * n / of:.2f}%"


def oracle_rows(o):
    n = o["executions"]
    rows = [["reproduced (the order Nasdaq executed met first, for its shares)", f"{o['reproduced']:,}", pct(o["reproduced"], n)]]
    rows += [[f"not reproduced: {k}", f"{v:,}", pct(v, n)] for k, v in o["not_reproduced"].items()]
    return rows


ORACLE_HEAD = ["Executions of the day", "Count", "Share"]


def oracle_section(lf, note, runs):
    o = oracle(runs)
    rested = lambda part: ", ".join(f"{k} {v:,}" for k, v in o[part].items() if v) or "none"
    return [f"## The day against Nasdaq's executions (note {note})", "",
            f"The sample day ({o['messages']:,} messages) replayed as order entry through the engine on {o['book'].replace('levels A: sorted array, table A: direct array', 'levels A with table A')}: "
            "each execution (E, C) entered as an immediate-or-cancel order on the other side at the executed order's price, for "
            "its shares, when the executed order is the first that order would meet; otherwise Nasdaq's outcome applied to the "
            "book and the execution counted by its cause, so the book stays Nasdaq's after every message. "
            f"{o['executions']:,} executions; counts from {len(runs)} runs, identical:", "",
            lf.table(ORACLE_HEAD, oracle_rows(o)), "",
            f"Of the day's {o['adds']:,} adds, those that would have crossed the book, rested without matching: {rested('adds_rested')}; "
            f"of its {o['replaces']:,} replaces: {rested('replaces_rested')}. Printable C executions at a price other than "
            f"their order's: {o['c_other_price']:,} ({o['c_other_price_reproduced']:,} reproduced). IDs taken: "
            f"{o['ids_taken']:,} of table A's {o['ids_capacity']:,} ({pct(o['ids_taken'], o['ids_capacity'])}; open item M1).", "",
            "Executed at another price: a printable C at a price other than its order's (ITCH 5.0 sends C when an order executes "
            "at a price other than its display price), so the order was ranked at a price the feed never showed. Earlier orders "
            "ahead: the cause is not found (open item M3); every order ahead of one is an add, and a replace keeping its place "
            "when only its size falls is ruled out.", ""]


def results_md(lf, sets):
    lines = ["# Results", "",
             "Generated by `python run.py report` from the runs listed in `results/published.toml`. "
             "Do not edit: `python run.py report --check` fails if this file and the JSON disagree. "
             "Latencies in ns unless marked µs; percentiles only.", ""]
    if not sets:
        return "\n".join(lines + ["No published results yet.", ""]).rstrip() + "\n"
    lines += lf.conditions(sets)
    if "book" in sets:
        lines += book_section(lf, *sets["book"])
    if "match" in sets:
        lines += match_section(lf, *sets["match"])
    if "oracle" in sets:
        lines += oracle_section(lf, *sets["oracle"])
    return "\n".join(lines).rstrip() + "\n"


def readme_blocks(lf, sets):
    blocks = {"book-summary": "The books: not yet measured (design note 002).\n",
              "match-summary": "Matching: not yet measured (design note 003).\n",
              "oracle-summary": "The day against Nasdaq's executions: not yet run (design note 004).\n"}
    if "book" in sets:
        runs = sets["book"][1]
        blocks["book-summary"] = (lf.table(DAY_HEAD, day_rows(lf, runs))
                                  + "\n\nThe sample day's displayed book events as operations, each timed alone on one CPU; ns, range "
                                  "over runs. Each operation, the synthetic cases and the method: [RESULTS.md](RESULTS.md).\n")
    if "match" in sets:
        head, rows = match_rows(lf, sets["match"][1])
        blocks["match-summary"] = (lf.table(head, rows) + "\n\nMatching's synthetic cases, each message timed alone on one CPU; ns, p50 / "
                                   "p99, range over runs. The method: [RESULTS.md](RESULTS.md).\n")
    if "oracle" in sets:
        o = oracle(sets["oracle"][1])
        blocks["oracle-summary"] = (lf.table(ORACLE_HEAD, oracle_rows(o)) + f"\n\nThe sample day replayed as order entry: of its {o['executions']:,} "
                                    "executions, how many the engine matched exactly as Nasdaq did, and every other by its cause. "
                                    "The method: [RESULTS.md](RESULTS.md).\n")
    return blocks


def report(root, check, lfipc=None):
    lf, lf_root = harness(root, lfipc)
    sets = load_sets(root, lf_root)
    results = results_md(lf, sets)
    readme_path = root / "README.md"
    readme = readme_path.read_text()
    blocks = readme_blocks(lf, sets)
    found = {m["name"] for m in BLOCK.finditer(readme)}
    if set(blocks) - found:
        raise ReportError(f"README.md has no block for {sorted(set(blocks) - found)}")
    new_readme = BLOCK.sub(lambda m: m.group(1) + blocks.get(m["name"], m["body"]) + m.group(4), readme)
    for text, what in ((results, "RESULTS.md"), (new_readme, "README.md")):
        if MEAN.findall(text):
            raise ReportError(f"{what}: generated text mentions {MEAN.findall(text)}")
    if not check:
        (root / "RESULTS.md").write_text(results)
        readme_path.write_text(new_readme)
        print(f"wrote RESULTS.md and {len(blocks)} README block(s)")
        return 0
    diffs = []
    current = (root / "RESULTS.md").read_text() if (root / "RESULTS.md").is_file() else ""
    if current != results:
        diffs.append("RESULTS.md")
    old = {m["name"]: m["body"] for m in BLOCK.finditer(readme)}
    new = {m["name"]: m["body"] for m in BLOCK.finditer(new_readme)}
    diffs += [f"README.md block '{k}'" for k in new if old.get(k) != new[k]]
    if diffs:
        print("report --check: out of date: " + ", ".join(diffs) + " (run: python run.py report)")
        return 1
    print("report --check: RESULTS.md and README.md agree with the published JSON")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    rp = sub.add_parser("report", help="generate RESULTS.md and README blocks from published JSON")
    rp.add_argument("--check", action="store_true", help="compare only; exit 1 on any difference")
    rp.add_argument("--root", type=Path, default=Path(__file__).resolve().parent, help=argparse.SUPPRESS)
    rp.add_argument("--lockfree-ipc", dest="lfipc", help="the fetched lockfree-ipc (default: build/_deps/lockfree_ipc-src)")
    args = ap.parse_args()
    try:
        return report(args.root, args.check, args.lfipc)
    except ReportError as e:
        print(f"report: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
