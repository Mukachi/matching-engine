// The oracle's counts (note 004, D4): the sample day replayed as order entry
// through the engine on the default book (`me::book`), each step decided on
// that book (D1), on CPU 2. Of the day's executions, how many the engine
// reproduced (the order Nasdaq executed is the one the IOC met, for the same
// shares) and every other by its cause (D2); the adds and replaces rested
// without matching, by cause; the IDs taken (table A's capacity, open item
// M1). A reproduced execution is checked as it is entered: exactly one trade,
// with the order expected, for the shares. Nothing here is timed, so the counts
// repeat run to run (S4). Run JSON through lockfree-ipc's harness.
// Design: docs/design/004-replay.md
//
// Usage: replay_oracle [output.json] [day file]
#include "../tools/day_file.hpp"
#include "../tools/replay.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <memory>
#include <string>

namespace {

using namespace lfipc::bench;
using namespace me;

constexpr int cpu = 2;

// Counts the trades of the step in hand, and what they matched.
struct checking {
    std::uint64_t trades = 0, refused_any = 0, no_room = 0;
    order_id maker = 0;
    shares_t shares = 0;
    void accepted(order_id) {}
    void refused(refusal) { ++refused_any; }
    void trade(const trade_report& t) {
        ++trades;
        maker = t.maker;
        shares = t.shares;
    }
    void rested(order_id, price_t, shares_t) {}
    void cancelled(order_id, shares_t, cause c) { no_room += c == cause::no_room; }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ME_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "oracle");
    if (run.refused) return run.finish(false, "replay_oracle");
    const auto clk = run_clock();
    write_clock_block(run.w, clk);
    if (!clk.ok || !detail::pin_to(cpu)) {
        std::fprintf(stderr, "replay_oracle: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "replay_oracle");
    }
    day_file f(path);
    auto m = std::make_unique<book>();
    checking k;
    engine<book, checking> e(*m, k);
    replay p;
    std::uint64_t steps = 0, wrong = 0;
    itchfh::message msg;
    while (f.next(msg)) {
        const step st = p.next(msg, *m);
        if (st.k == step::none) continue;
        ++steps;
        const std::uint64_t before = k.trades;
        apply(st, e, *m);
        if (st.k == step::execute) wrong += k.trades != before + 1 || k.maker != st.id || k.shares != st.shares;
        p.done();
    }
    const replay_counts& c = p.counts();
    std::printf("the day: %llu messages, %llu steps; %llu executions, %llu reproduced; %llu wrong\n", (unsigned long long)f.sequence(),
                (unsigned long long)steps, (unsigned long long)c.executions, (unsigned long long)c.reproduced, (unsigned long long)wrong);
    const bool ok = f.sequence() > 0 && wrong == 0 && k.refused_any == 0 && k.no_room == 0 && c.unknown == 0;
    const auto by = [&](const char* key, const std::uint64_t (&v)[misses]) {
        run.w.key(key);
        run.w.begin_object();
        for (int i = 0; i < misses; ++i) run.w.member(to_string(static_cast<miss>(i)), v[i]);
        run.w.end_object();
    };
    run.w.key("oracle");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", "01302020.NASDAQ_ITCH50.gz");
    run.w.member("book", "levels A: sorted array, table A: direct array");
    run.w.member("itch_feed_handler_commit", ME_ITCHFH_COMMIT);
    run.w.member("messages", f.sequence());
    run.w.member("steps", steps);
    run.w.member("executions", c.executions);
    run.w.member("reproduced", c.reproduced);
    by("not_reproduced", c.by_cause);
    run.w.member("c_other_price", c.c_other_price);
    run.w.member("c_other_price_reproduced", c.c_other_price_reproduced);
    by("c_other_price_not_reproduced", c.c_other_price_by_cause);
    run.w.member("adds", c.adds);
    by("adds_rested", c.adds_rested);
    run.w.member("replaces", c.replaces);
    by("replaces_rested", c.replaces_rested);
    run.w.member("unknown_references", c.unknown);
    run.w.member("ids_taken", m->next_id() - 1);
    run.w.member("ids_capacity", static_cast<std::uint64_t>(config{}.ids));
    run.w.member("reproduced_wrong", wrong);
    run.w.member("refused", k.refused_any);
    run.w.member("no_room", k.no_room);
    run.w.end_object();
    return run.finish(ok, "replay_oracle");
}
