// S2 of note 003: random streams of messages (fixed seeds) through the engine
// over every combination of note 002's D1 and D2 and through the reference
// engine in lockstep, marketable orders, IOCs, sweeps and crossing replaces
// among them. After every message: the same reports in the same sequence,
// every level touched the same in full, and no book of the instrument
// crossed; every 100,000 messages and at the end, every book in full. Live
// orders held near 100,000; some messages are refused on purpose.
// Usage: engine_random_test [messages per seed] (default 5,000,000, two seeds)
// Design: docs/design/003-matching.md
#include "check.hpp"
#include "reports.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/reference_engine.hpp>
#include <me/reference_market.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

using namespace me;
using me::test::check;
using me::test::recorder;
using me::test::report;

constexpr instrument_t instruments = 8;

config cfg() {
    config c;
    c.orders = 1u << 20;
    c.levels = 1u << 18;
    c.level_arena = 1u << 22;
    c.ids = 1u << 25;
    c.instruments = instruments;
    c.prefault = false;
    return c;
}

template <class M>
struct fast {
    M m{cfg()};
    recorder r;
    engine<M, recorder> e{m, r};
};
struct ref {
    reference_market m{cfg()};
    recorder r;
    reference_engine<recorder> e{m, r};
};

struct rigs {
    fast<market<levels_sorted, table_direct>> aa;
    fast<market<levels_sorted, table_hash>> ab;
    fast<market<levels_map, table_direct>> ba;
    fast<market<levels_map, table_hash>> bb;
    ref rf;
    template <class F>
    void each(F&& f) {
        f(aa), f(ab), f(ba), f(bb);
    }
};

// The live orders, as the reference reports them: picked at random, removed in O(1).
struct live_set {
    std::vector<order_id> ids;
    std::unordered_map<order_id, std::size_t> at;
    void add(order_id id) {
        at[id] = ids.size();
        ids.push_back(id);
    }
    void drop(order_id id) {
        const auto it = at.find(id);
        if (it == at.end()) return;
        const std::size_t i = it->second;
        at.erase(it);
        if (i + 1 != ids.size()) {
            ids[i] = ids.back();
            at[ids[i]] = i;
        }
        ids.pop_back();
    }
};

struct counts {
    std::uint64_t trades = 0, sweeps = 0, iocs = 0, crossing_replaces = 0, refused = 0;
    std::size_t deepest = 0;
};

bool run(std::uint64_t seed, std::size_t n, counts& c) {
    std::mt19937_64 rng(seed);
    rigs g;
    live_set live;
    std::vector<price_t> mid(instruments);
    for (auto& m : mid) m = 1'000'000 + static_cast<price_t>(rng() % 9'000'000);  // $100 to $1,000
    const auto pick = [&](std::uint64_t k) { return rng() % k; };
    // A resting price: most near the touch, a long tail out to ~4,000 cents.
    const auto passive = [&](instrument_t i, side s) {
        const std::uint64_t r = pick(1000);
        const std::uint32_t off = r < 600 ? static_cast<std::uint32_t>(pick(20)) : r < 950 ? static_cast<std::uint32_t>(pick(400))
                                                                                          : static_cast<std::uint32_t>(pick(4000));
        const price_t p = s == side::buy ? mid[i] - 100 * (off + 1) : mid[i] + 100 * (off + 1);
        return pick(5000) == 0 ? price_t{0} : pick(5000) == 0 ? max_price + 100 : p;  // now and then out of range
    };
    // A marketable price: through the mid by up to 30 cents, so through a few levels.
    const auto through = [&](instrument_t i, side s) {
        const auto off = static_cast<price_t>(100 * pick(31));
        return s == side::buy ? mid[i] + off : mid[i] - off;
    };
    const auto size = [&] { return pick(5000) == 0 ? shares_t{0} : static_cast<shares_t>(1 + pick(pick(10) == 0 ? 20'000 : 500)); };

    for (std::size_t k = 0; k < n; ++k) {
        if (k % 1000 == 0)
            for (auto& m : mid) m = static_cast<price_t>(static_cast<std::int64_t>(m) + (static_cast<std::int64_t>(pick(201)) - 100) * 100);
        const std::uint64_t op = pick(100);
        const instrument_t ins = static_cast<instrument_t>(pick(instruments));
        const side s = pick(2) ? side::buy : side::sell;
        const bool any = !live.ids.empty();
        const order_id id = any && pick(50) ? live.ids[pick(live.ids.size())] : static_cast<order_id>(pick(g.rf.m.next_id() + 10));
        const auto before = g.rf.m.find(id);
        std::vector<std::tuple<instrument_t, side, price_t>> touched;
        if (before) touched.emplace_back(before->instrument, before->side, before->price);
        instrument_t on = before ? before->instrument : ins;
        side taker = s;  // the side of the order that may match
        const std::uint64_t enters = live.ids.size() < 50'000 ? 60 : live.ids.size() < 100'000 ? 50 : 30;
        // One message, the same to every engine.
        const auto send = [&](auto&& f) {
            f(g.rf.e);
            g.each([&](auto& x) { f(x.e); });
        };
        if (op < enters || !any) {
            const bool marketable = pick(3) == 0;
            const tif t = marketable && pick(2) ? tif::ioc : tif::rest;
            const price_t p = marketable ? through(ins, s) : passive(ins, s);
            const shares_t q = size();
            c.iocs += t == tif::ioc;
            send([&](auto& e) { e.enter(ins, s, p, q, t); });
            touched.emplace_back(ins, s, p);
            on = ins;
        } else if (op < 70) {
            send([&](auto& e) { e.cancel(id); });
        } else if (op < 85) {
            const shares_t q = before && pick(4) ? static_cast<shares_t>(1 + pick(before->shares + 1)) : size();
            send([&](auto& e) { e.cancel(id, q); });
        } else {
            const side ps = before ? before->side : s;
            taker = ps;
            const bool cross = pick(4) == 0;
            const price_t p = cross ? through(on, ps) : pick(3) == 0 && before ? before->price : passive(on, ps);
            const shares_t q = size();
            send([&](auto& e) { e.replace(id, p, q); });
            if (before) touched.emplace_back(before->instrument, before->side, p);
        }
        // The reference's reports: what every engine must have said.
        const std::vector<report> want = g.rf.r.take();
        bool ok = true;
        g.each([&](auto& x) { ok = x.r.take() == want && ok; });
        std::size_t trades = 0;
        for (const report& r : want) {
            if (r.k == report::trade) {
                ++trades;
                touched.emplace_back(r.t.instrument, taker == side::buy ? side::sell : side::buy, r.t.price);
                if (r.t.maker_left == 0) live.drop(r.t.maker);
            } else if (r.k == report::rested) {
                live.add(r.id);
            } else if (r.k == report::cancelled) {
                live.drop(r.id);
            } else if (r.k == report::refused) {
                ++c.refused;
            }
        }
        c.trades += trades;
        c.sweeps += trades > 1;
        c.crossing_replaces += before && op >= 85 && trades > 0;
        for (const auto& [i, sd, p] : touched) {
            const auto r = g.rf.m.level(i, sd, p);
            g.each([&](auto& x) { ok = ok && x.m.level(i, sd, p) == r; });
        }
        const auto bid = g.rf.m.best(on, side::buy), ask = g.rf.m.best(on, side::sell);
        const bool crossed = bid && ask && bid->first >= ask->first;
        if (k % 100'000 == 99'999 || k + 1 == n) {
            for (instrument_t i = 0; i < instruments; ++i)
                for (side sd : {side::buy, side::sell}) {
                    const auto r = g.rf.m.state(i, sd);
                    if (r.size() > c.deepest) c.deepest = r.size();
                    g.each([&](auto& x) { ok = ok && x.m.state(i, sd) == r; });
                }
            g.each([&](auto& x) { ok = ok && x.m.live_orders() == g.rf.m.live_orders() && x.m.next_id() == g.rf.m.next_id(); });
        }
        if (!ok || crossed) {
            std::string w;
            for (const report& r : want) w += me::test::to_string(r) + "; ";
            std::printf("  message %zu: reference said: %s\n", k, w.c_str());
            check(false, "seed " + std::to_string(seed) + ": the engines agree with the reference and no book is crossed, at message " +
                             std::to_string(k) + (crossed ? " (crossed)" : ""));
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
    for (const std::uint64_t seed : {20261104u, 11u}) {
        counts c;
        const bool ok = run(seed, n, c);
        check(ok, "S2, seed " + std::to_string(seed) + ": " + std::to_string(n) +
                      " random messages, the engine over every combination of D1 and D2 equal to the reference engine after each "
                      "(reports in sequence, every level touched; every book in full each 100,000), no book crossed");
        std::printf("      %llu trades, %llu messages sweeping more than one order, %llu IOCs, %llu crossing replaces, %llu refused; "
                    "deepest side %zu levels\n",
                    (unsigned long long)c.trades, (unsigned long long)c.sweeps, (unsigned long long)c.iocs,
                    (unsigned long long)c.crossing_replaces, (unsigned long long)c.refused, c.deepest);
    }
    return me::test::result();
}
