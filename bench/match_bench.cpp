// Matching's cost (note 003, D6): the reference engine and the engine over
// every combination of note 002's D1 and D2 (default capacities), one after
// another on CPU 2.
//
// Synthetic cases on one instrument, 1,000 levels a side of 10 orders of 100
// shares each, a cent apart (note 002's synthetic book), a case at a time:
// each timed message (start() to stop(), unsubtracted, into lockfree-ipc's
// histogram) followed by untimed ones that put the book back, so every
// repetition meets the same book. The sink counts, as a caller's would that
// does nothing with the reports. The book stays in cache: each message's cost
// at its best. The day per message is note 005's.
// Run JSON through lockfree-ipc's harness. Design: docs/design/003-matching.md
//
// Usage: match_bench [output.json]
#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/reference_engine.hpp>
#include <me/reference_market.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <malloc.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
using namespace me;

constexpr int cpu = 2;
using hist = histogram<10, 40>;

constexpr int cases = 8;
const char* const case_names[cases] = {"add that does not match",   "match one order, filled whole", "match one order, in part",
                                       "sweep 1 level (10 orders)", "sweep 5 levels (50 orders)",    "sweep 20 levels (200 orders)",
                                       "IOC that meets nothing",    "replace that crosses"};
// Sweeps put back 10 to 200 orders a repetition, so fewer repetitions.
constexpr std::size_t reps[cases] = {1'000'000, 1'000'000, 1'000'000, 100'000, 100'000, 100'000, 1'000'000, 1'000'000};
constexpr std::size_t warm[cases] = {10'000, 10'000, 10'000, 1'000, 1'000, 1'000, 10'000, 10'000};
constexpr int depth = 1000, per_level = 10;
constexpr shares_t lot = 100;
constexpr price_t mid = 1'000'000;  // $100.0000; a cent a level

price_t bid(int level) { return mid - 100 * static_cast<price_t>(level + 1); }  // level 0 the best
price_t ask(int level) { return mid + 100 * static_cast<price_t>(level + 1); }

struct counting {
    std::uint64_t n = 0, shares = 0;
    order_id last = 0, maker = 0;
    bool refused_any = false;
    void accepted(order_id id) {
        ++n;
        last = id;
    }
    void refused(refusal) { refused_any = true; }
    void trade(const trade_report& t) {
        shares += t.shares;
        maker = t.maker;
    }
    void rested(order_id, price_t, shares_t) { ++n; }
    void cancelled(order_id, shares_t s, cause) { shares += s; }
};

struct result {
    std::vector<hist> h = std::vector<hist>(cases);
    bool clean = true;
};

template <class E>
result measure(E& e, counting& k) {
    result r;
    for (int l = 0; l < depth; ++l)
        for (int j = 0; j < per_level; ++j) e.enter(0, side::buy, bid(l), lot, tif::rest), e.enter(0, side::sell, ask(l), lot, tif::rest);
    const auto timed = [&](int c, std::size_t i, auto&& f) {
        const ticks t0 = start();
        f();
        const ticks t1 = stop();
        if (i >= warm[c]) r.h[c].record(t1 - t0);
    };
    const auto n = [](int c) { return reps[c] + warm[c]; };
    for (std::size_t i = 0; i < n(0); ++i) {  // rests at the best bid; then cancelled
        timed(0, i, [&] { e.enter(0, side::buy, bid(0), lot, tif::rest); });
        e.cancel(k.last);
    }
    for (std::size_t i = 0; i < n(1); ++i) {  // the best ask's head filled whole; a new ask at the back
        timed(1, i, [&] { e.enter(0, side::buy, ask(0), lot, tif::rest); });
        e.enter(0, side::sell, ask(0), lot, tif::rest);
    }
    for (std::size_t i = 0; i < n(2); ++i) {  // the head filled in part; it is cancelled and a new ask put at the back
        timed(2, i, [&] { e.enter(0, side::buy, ask(0), 40, tif::rest); });
        e.cancel(k.maker);
        e.enter(0, side::sell, ask(0), lot, tif::rest);
    }
    for (int c = 3; c <= 5; ++c) {  // 1, 5, 20 levels taken whole, nothing left to rest; the levels put back
        const int levels = c == 3 ? 1 : c == 4 ? 5 : 20;
        for (std::size_t i = 0; i < n(c); ++i) {
            timed(c, i, [&] { e.enter(0, side::buy, ask(levels - 1), static_cast<shares_t>(levels * per_level * lot), tif::rest); });
            for (int l = 0; l < levels; ++l)
                for (int j = 0; j < per_level; ++j) e.enter(0, side::sell, ask(l), lot, tif::rest);
        }
    }
    for (std::size_t i = 0; i < n(6); ++i)  // priced at the best bid: meets no ask, cancelled
        timed(6, i, [&] { e.enter(0, side::buy, bid(0), lot, tif::ioc); });
    e.enter(0, side::buy, bid(0), lot, tif::rest);
    for (std::size_t i = 0; i < n(7); ++i) {  // our bid at the best replaced to the best ask: fills its head whole
        const order_id mine = k.last;
        timed(7, i, [&] { e.replace(mine, ask(0), lot); });
        e.enter(0, side::sell, ask(0), lot, tif::rest);
        e.enter(0, side::buy, bid(0), lot, tif::rest);
    }
    r.clean = !k.refused_any;
    return r;
}

struct named {
    std::string name, levels, table;
    result r;
};

named reference() {
    auto m = std::make_unique<reference_market>();
    counting k;
    reference_engine<counting> e(*m, k);
    named x{"reference", "reference", "reference", measure(e, k)};
    std::printf("%-56s%s\n", x.name.c_str(), x.r.clean ? "" : "  REFUSED SOME");
    std::fflush(stdout);
    return x;
}

template <class L, class T>
named fast() {
    ::malloc_trim(0);
    auto m = std::make_unique<market<L, T>>();
    counting k;
    engine<market<L, T>, counting> e(*m, k);
    named x{std::string("levels ") + L::name + ", table " + T::name, L::name, T::name, measure(e, k)};
    std::printf("%-56s%s\n", x.name.c_str(), x.r.clean ? "" : "  REFUSED SOME");
    std::fflush(stdout);
    return x;
}

}  // namespace

int main(int argc, char** argv) {
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "match");
    if (run.refused) return run.finish(false, "match_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "match_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "match_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= 1'000'000) none.record(t1 - t0);
    }

    std::vector<named> results;
    results.push_back(reference());
    results.push_back(fast<levels_sorted, table_direct>());
    results.push_back(fast<levels_sorted, table_hash>());
    results.push_back(fast<levels_map, table_direct>());
    results.push_back(fast<levels_map, table_hash>());

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    const auto write_hist = [&](const hist& h) {
        run.w.member("count", h.count());
        run.w.member("p50_ns", ns(h.percentile(50)));
        run.w.member("p99_ns", ns(h.percentile(99)));
        run.w.member("p999_ns", ns(h.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(h.max()) / tpn);
    };
    bool ok = true;
    run.w.key("match");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("book", "one instrument, 1,000 levels a side of 10 orders of 100 shares, a cent apart");
    run.w.member("lockfree_ipc_commit", ME_LFIPC_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.key("engines");
    run.w.begin_array();
    for (const named& x : results) {
        ok &= x.r.clean;
        run.w.begin_object();
        run.w.member("name", x.name);
        run.w.member("levels", x.levels);
        run.w.member("table", x.table);
        run.w.member("nothing_refused", x.r.clean);
        run.w.key("cases");
        run.w.begin_array();
        for (int c = 0; c < cases; ++c) {
            run.w.begin_object();
            run.w.member("case", std::string(case_names[c]));
            run.w.member("warmup_discarded", static_cast<std::uint64_t>(warm[c]));
            write_hist(x.r.h[c]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "match_bench");
}
