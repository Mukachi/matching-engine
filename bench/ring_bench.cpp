// The engine behind lockfree-ipc's SPSC ring (note 005, S2; note 001's D5):
// the day's first steps carried from a producer thread to the engine's thread
// under lockfree-ipc's open-loop generator, unchanged (`load.hpp`). The
// producer, on CPU 3, sends {seq, intended} through `spsc_ring` (its default,
// v3) at a fixed rate; the system under test's try_receive, on CPU 2, takes a
// message and applies step `seq` to the engine (levels A, table A) before
// returning, so the latency the harness records (intended send time to that
// return) holds the ring and the engine's work. A rate the producer cannot
// keep is refused by the harness and published as refused (D3).
//
// Beside it, the same steps at the same rates with no ring: one thread on
// CPU 2 waits for each step's intended time, applies it and records the time
// from intended to done, so a step that waits behind a slow one is late as it
// would be behind the ring. Each rate from a fresh engine, over the same steps.
// Run JSON through lockfree-ipc's harness. Design: docs/design/005-measurement.md
//
// Usage: ring_bench [output.json] [day file]
#include "../tools/steps.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/memory.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/histogram_block.hpp>
#include <lfipc/bench/load.hpp>
#include <lfipc/bench/load_block.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/placement.hpp>
#include <lfipc/bench/run_file.hpp>
#include <lfipc/spsc.hpp>

#include <malloc.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
using namespace me;

constexpr int engine_cpu = 2, producer_cpu = 3;
constexpr std::size_t measured = 50'000'000;  // steps measured at each rate, after the harness's warm-up
constexpr double rates[] = {1e6, 2e6, 5e6};
constexpr std::size_t capacity = 1024;  // as lockfree-ipc's own measurements

struct rig {
    book m;
    tally k;
    engine<book, tally> e{m, k};
};

// The system under test: the ring, and the engine behind it.
struct engine_sut {
    lfipc::spsc_ring<message, capacity> q;
    std::span<const step> steps;
    rig* g = nullptr;
    bool send(const message& m) { return q.try_push(m); }
    bool try_receive(message& m) {
        if (!q.try_pop(m)) return false;
        apply(steps[m.seq], g->e, g->m);
        return true;
    }
};

struct no_ring_result {
    double rate = 0;
    histogram<> latency;
    std::size_t warmup = 0;
};

// The same schedule on one thread: wait for each step's intended time, apply it, record intended to done.
no_ring_result no_ring(std::span<const step> steps, double rate, std::size_t warm, std::size_t n, double ticks_per_ns) {
    no_ring_result r;
    r.rate = rate, r.warmup = warm;
    auto g = std::make_unique<rig>();
    const double interval = 1e9 / rate * ticks_per_ns;
    std::vector<ticks> offset(warm + n);
    for (std::size_t i = 0; i < offset.size(); ++i) offset[i] = static_cast<ticks>(std::llround(static_cast<double>(i) * interval));
    const ticks base = start() + static_cast<ticks>(1e6 * ticks_per_ns);  // 1 ms to settle
    for (std::size_t i = 0; i < offset.size(); ++i) {
        const ticks intended = base + offset[i];
        while (start() < intended) detail::cpu_relax();
        apply(steps[i], g->e, g->m);
        const ticks done = stop();
        if (i >= warm) r.latency.record(done - intended);
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ME_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "ring");
    if (run.refused) return run.finish(false, "ring_bench");
    const auto noise = sample_noise({engine_cpu, producer_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(engine_cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "ring_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "ring_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    std::size_t most = 0;
    for (const double rate : rates) most = std::max(most, load_warmup(rate, measured) + measured);
    day_steps d = convert_day(path, most);
    if (!d.clean || d.steps.size() < most) {
        std::fprintf(stderr, "ring_bench: %s: no sample day, too short, or its conversion refused something\n", path.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "ring_bench");
    }
    auto copy = zeroed<step>(d.steps.size(), true);  // on CPU 2's node
    std::copy(d.steps.begin(), d.steps.end(), copy.get());
    const std::span<const step> steps(copy.get(), d.steps.size());
    std::vector<step>().swap(d.steps);
    std::printf("the day's first %zu steps (%llu messages)\n", steps.size(), static_cast<unsigned long long>(d.messages));

    const cross_cpu_pair pair = measure_pair(producer_cpu, engine_cpu);
    std::vector<std::unique_ptr<load_result>> ring;
    std::vector<no_ring_result> alone;
    bool ok = true;
    for (const double rate : rates) {
        ::malloc_trim(0);
        auto g = std::make_unique<rig>();
        auto sut = make_on_cpu<engine_sut>(producer_cpu);
        auto r = std::make_unique<load_result>();
        if (!sut) {
            r->reason = "cannot pin to producer CPU 3 to place the ring";
        } else {
            sut->steps = steps;
            sut->g = g.get();
            load_config c;
            c.rate_per_s = rate;
            c.messages = measured;
            c.producer_cpu = producer_cpu;
            c.consumer_cpu = engine_cpu;
            c.ticks_per_ns = tpn;
            run_load(*sut, c, *r);
            apply_placement(*r, sut.get(), producer_cpu);
        }
        const bool as_expected = !r->ok || g->k.refused_any == 0;
        ok &= as_expected;
        std::printf("ring    %9.0f/s %s\n", rate, r->ok ? "ok" : r->reason.c_str());
        std::fflush(stdout);
        ring.push_back(std::move(r));
        if (!detail::pin_to(engine_cpu)) return run.finish(false, "ring_bench");
        ::malloc_trim(0);
        alone.push_back(no_ring(steps, rate, load_warmup(rate, measured), measured, tpn));
        std::printf("no ring %9.0f/s ok\n", rate);
        std::fflush(stdout);
    }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    run.w.key("ring");
    run.w.begin_object();
    run.w.member("engine_cpu", engine_cpu);
    run.w.member("producer_cpu", producer_cpu);
    run.w.member("capacity", static_cast<std::uint64_t>(capacity));
    run.w.member("message_bytes", static_cast<std::uint64_t>(sizeof(message)));
    run.w.member("measured_per_rate", static_cast<std::uint64_t>(measured));
    run.w.member("book", "levels A: sorted array, table A: direct array");
    run.w.member("lockfree_ipc_commit", ME_LFIPC_COMMIT);
    write_load_block(run.w, "the engine behind lfipc::spsc_ring (v3)", pair, ring, tpn, "with_ring");
    run.w.key("no_ring");
    run.w.begin_array();
    for (const no_ring_result& a : alone) {
        run.w.begin_object();
        run.w.member("rate_per_s", a.rate);
        run.w.member("warmup_discarded", static_cast<std::uint64_t>(a.warmup));
        write_histogram_block(run.w, a.latency, tpn, "latency");
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "ring_bench");
}
