// The sample day per message (note 005, S1): note 004's steps, converted once
// before anything is timed, through the default engine (levels A, table A)
// and the reference engine (D1), one after the other on CPU 2. For each
// engine two passes, each from a fresh book:
//   timed: every step between start() and stop(), unsubtracted, into a
//          histogram per kind (D2), the first million discarded; beside an
//          empty window ("none") for the clock's own cost;
//   whole day: every step untimed, as steps a second.
// The most memory each engine held, as note 002's benchmark reads it (resident
// memory every million steps, outside every window, less what was resident
// before), and its end state against the conversion's.
// Run JSON through lockfree-ipc's harness. Design: docs/design/005-measurement.md
//
// Usage: day_bench [output.json] [day file]
#include "../tools/steps.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/memory.hpp>
#include <me/reference_engine.hpp>
#include <me/reference_market.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
using namespace me;

constexpr int cpu = 2;
constexpr std::size_t warmup = 1'000'000;
using hist = histogram<10, 40>;

constexpr int kinds = 7;  // 0 all
const char* const kind_names[kinds] = {"all", "enter", "cancel part", "cancel", "replace", "execute", "applied"};
int kind_of(step::kind k) {
    switch (k) {
        case step::enter: return 1;
        case step::cancel_part: return 2;
        case step::cancel_all: return 3;
        case step::replace: return 4;
        case step::execute: return 5;
        default: return 6;  // rest, replace_rest, execute_apply: note 004's D1, applied to the book
    }
}

std::size_t resident_kb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    std::size_t v = 0;
    while (f >> k)
        if (k == "VmRSS:") {
            f >> v;
            return v;
        }
    return 0;
}

struct result {
    std::string name;
    std::vector<hist> h = std::vector<hist>(kinds);
    double seconds = 0, steps_per_s = 0, peak_resident_mb = 0;
    bool clean = true;  // both passes ended as the conversion did
};

// One engine and its book, made fresh: `Rig` holds them.
template <class Rig>
result measure(const std::string& name, std::span<const step> steps, const end_state& want) {
    result r;
    r.name = name;
    {
        ::malloc_trim(0);
        const std::size_t before = resident_kb();
        std::size_t peak = before;
        auto g = std::make_unique<Rig>();
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (i % 1'000'000 == 0) peak = std::max(peak, resident_kb());
            const ticks t0 = start();
            apply(steps[i], g->e, g->m);
            const ticks t1 = stop();
            if (i < warmup) continue;
            const int k = kind_of(steps[i].k);
            r.h[k].record(t1 - t0);
            r.h[0].record(t1 - t0);
        }
        peak = std::max(peak, resident_kb());
        r.peak_resident_mb = static_cast<double>(peak - before) / 1024.0;
        r.clean &= g->end() == want && g->k.refused_any == 0;
    }
    ::malloc_trim(0);
    auto g = std::make_unique<Rig>();
    const auto w0 = std::chrono::steady_clock::now();
    for (const step& s : steps) apply(s, g->e, g->m);
    const auto w1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(w1 - w0).count();
    r.steps_per_s = static_cast<double>(steps.size()) / r.seconds;
    r.clean &= g->end() == want && g->k.refused_any == 0;
    std::printf("%-48s %7.2f M steps/s  %6.1f s  %8.0f MB%s\n", name.c_str(), r.steps_per_s / 1e6, r.seconds, r.peak_resident_mb,
                r.clean ? "" : "  NOT AS THE CONVERSION ENDED");
    std::fflush(stdout);
    return r;
}

struct fast_rig {
    book m;
    tally k;
    engine<book, tally> e{m, k};
    end_state end() const { return {m.live_orders(), m.next_id(), k.trades}; }
};
struct reference_rig {
    reference_market m;
    tally k;
    reference_engine<tally> e{m, k};
    end_state end() const { return {m.live_orders(), m.next_id(), k.trades}; }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ME_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "day");
    if (run.refused) return run.finish(false, "day_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "day_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "day_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    day_steps d = convert_day(path);
    if (!d.clean) {
        std::fprintf(stderr, "day_bench: %s: no sample day, or its conversion refused something (see README)\n", path.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "day_bench");
    }
    auto copy = zeroed<step>(d.steps.size(), true);  // on CPU 2's node: the harness bound memory there
    std::copy(d.steps.begin(), d.steps.end(), copy.get());
    const std::span<const step> steps(copy.get(), d.steps.size());
    const std::size_t n = steps.size();
    std::vector<step>().swap(d.steps);
    std::printf("the day: %llu messages, %zu steps\n", static_cast<unsigned long long>(d.messages), n);

    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= warmup) none.record(t1 - t0);
    }
    std::vector<result> results;
    results.push_back(measure<reference_rig>("reference", steps, d.end));
    results.push_back(measure<fast_rig>("levels A: sorted array, table A: direct array", steps, d.end));

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
    run.w.key("day");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", "01302020.NASDAQ_ITCH50.gz");
    run.w.member("messages", d.messages);
    run.w.member("steps", static_cast<std::uint64_t>(n));
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("trades", d.end.trades);
    run.w.member("lockfree_ipc_commit", ME_LFIPC_COMMIT);
    run.w.member("itch_feed_handler_commit", ME_ITCHFH_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.key("engines");
    run.w.begin_array();
    for (const result& r : results) {
        ok &= r.clean;
        run.w.begin_object();
        run.w.member("name", r.name);
        run.w.member("as_converted", r.clean);
        run.w.member("steps_per_s", r.steps_per_s);
        run.w.member("seconds", r.seconds);
        run.w.member("peak_resident_mb", r.peak_resident_mb);
        run.w.key("kinds");
        run.w.begin_array();
        for (int k = 0; k < kinds; ++k) {
            run.w.begin_object();
            run.w.member("kind", std::string(kind_names[k]));
            write_hist(r.h[k]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "day_bench");
}
