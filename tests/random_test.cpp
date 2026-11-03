// S2 of note 002: random streams of operations (fixed seeds) through every
// combination of D1 and D2 and the reference in lockstep. After every
// operation: the same answer, the same order (for its ID), and every level it
// touched the same in full (price, shares, count, the queue); every 100,000
// operations and at the end, every book in full. Books grow thousands of
// levels deep, live orders held near 100,000; some operations are refused on
// purpose.
// Usage: random_test [operations per seed] (default 10,000,000 over two seeds)
// Design: docs/design/002-book.md
#include "check.hpp"

#include <me/market.hpp>
#include <me/reference_market.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace me;
using me::test::check;

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

using AA = market<levels_sorted, table_direct>;
using AB = market<levels_sorted, table_hash>;
using BA = market<levels_map, table_direct>;
using BB = market<levels_map, table_hash>;
using AC = market<levels_sorted, table_low>;
using BC = market<levels_map, table_low>;

struct books {
    AA aa{cfg()};
    AB ab{cfg()};
    BA ba{cfg()};
    BB bb{cfg()};
    AC ac{cfg()};
    BC bc{cfg()};
    reference_market ref{cfg()};
};

bool same(const answer& a, const answer& b) { return a.why == b.why && a.id == b.id; }

template <class F>
bool all(books& b, F&& f) {
    return f(b.aa) && f(b.ab) && f(b.ba) && f(b.bb) && f(b.ac) && f(b.bc);
}

bool run(std::uint64_t seed, std::size_t n, std::size_t& deepest) {
    std::mt19937_64 rng(seed);
    books b;
    std::vector<order_id> live;  // as the reference answers
    std::vector<price_t> mid(instruments);
    for (auto& m : mid) m = 1'000'000 + static_cast<price_t>(rng() % 9'000'000);  // $100 to $1,000
    const auto pick = [&](std::uint64_t k) { return rng() % k; };
    const auto price_near = [&](instrument_t i, side s) {
        // Most near the best, a long tail out to ~4,000 ticks of a cent.
        const std::uint64_t r = pick(1000);
        const std::uint32_t off = r < 600 ? static_cast<std::uint32_t>(pick(20)) : r < 950 ? static_cast<std::uint32_t>(pick(400))
                                                                                          : static_cast<std::uint32_t>(pick(4000));
        const price_t p = s == side::buy ? mid[i] - 100 * (off + 1) : mid[i] + 100 * (off + 1);
        return pick(5000) == 0 ? price_t{0} : pick(5000) == 0 ? max_price + 100 : p;  // now and then out of range
    };
    const auto size = [&] { return pick(5000) == 0 ? shares_t{0} : static_cast<shares_t>(1 + pick(pick(10) == 0 ? 20'000 : 500)); };

    for (std::size_t k = 0; k < n; ++k) {
        if (k % 1000 == 0)
            for (auto& m : mid) m = static_cast<price_t>(static_cast<std::int64_t>(m) + (static_cast<std::int64_t>(pick(201)) - 100) * 100);
        const std::uint64_t op = pick(100);
        const instrument_t ins = static_cast<instrument_t>(pick(instruments));
        const side s = pick(2) ? side::buy : side::sell;
        const bool any = !live.empty();
        const std::size_t li = any ? pick(live.size()) : 0;
        const order_id id = any && pick(50) ? live[li] : static_cast<order_id>(pick(b.ref.next_id() + 10));  // now and then unknown
        // What each operation touches: (instrument, side, price) before and after.
        std::vector<std::tuple<instrument_t, side, price_t>> touched;
        const auto before = b.ref.find(id);
        if (before) touched.emplace_back(before->instrument, before->side, before->price);
        answer want;
        bool ok = true;
        // Adds less likely as live orders pass ~100,000, so the stream stays inside
        // the books' capacity (past it the fast books refuse, as they should, and
        // the reference, which has none, does not).
        const std::uint64_t adds = live.size() < 50'000 ? 60 : live.size() < 100'000 ? 50 : 30;
        if (op < adds || !any) {
            const price_t p = price_near(ins, s);
            const shares_t q = size();
            want = b.ref.add(ins, s, p, q);
            ok = all(b, [&](auto& m) { return same(m.add(ins, s, p, q), want); });
            touched.emplace_back(ins, s, p);
            if (want.ok()) live.push_back(want.id);
        } else if (op < 70) {
            want = b.ref.cancel(id);
            ok = all(b, [&](auto& m) { return same(m.cancel(id), want); });
        } else if (op < 85) {
            const shares_t q = before && pick(4) ? static_cast<shares_t>(1 + pick(before->shares + 1)) : size();
            want = b.ref.cancel(id, q);
            ok = all(b, [&](auto& m) { return same(m.cancel(id, q), want); });
        } else {
            const side ps = before ? before->side : s;
            const price_t p = pick(3) == 0 && before ? before->price : price_near(before ? before->instrument : ins, ps);
            const shares_t q = size();
            want = b.ref.replace(id, p, q);
            ok = all(b, [&](auto& m) { return same(m.replace(id, p, q), want); });
            if (want.ok()) {
                touched.emplace_back(before->instrument, before->side, p);
                live.push_back(want.id);
            }
        }
        if (want.ok() && before && !b.ref.find(id)) {  // the old ID finished: drop it from `live`
            for (std::size_t j = 0; j < live.size(); ++j)
                if (live[j] == id) {
                    live[j] = live.back();
                    live.pop_back();
                    break;
                }
        }
        const auto ref_order = b.ref.find(want.id ? want.id : id);
        ok = ok && all(b, [&](auto& m) {
                 const auto o = m.find(want.id ? want.id : id);
                 return o.has_value() == ref_order.has_value() &&
                        (!o || (o->id == ref_order->id && o->price == ref_order->price && o->shares == ref_order->shares &&
                                o->instrument == ref_order->instrument && o->side == ref_order->side));
             });
        for (const auto& [i, sd, p] : touched) {
            const auto r = b.ref.level(i, sd, p);
            ok = ok && all(b, [&](auto& m) { return m.level(i, sd, p) == r; });
        }
        if (k % 100'000 == 99'999 || k + 1 == n) {
            for (instrument_t i = 0; i < instruments; ++i)
                for (side sd : {side::buy, side::sell}) {
                    const auto r = b.ref.state(i, sd);
                    if (r.size() > deepest) deepest = r.size();
                    ok = ok && all(b, [&](auto& m) { return m.state(i, sd) == r; });
                }
            ok = ok && all(b, [&](auto& m) { return m.live_orders() == b.ref.live_orders() && m.next_id() == b.ref.next_id(); });
        }
        if (!ok) {
            std::printf("  live %zu, refused no_orders %llu no_levels %llu no_ids %llu (levels A, table A)\n", b.ref.live_orders(),
                        (unsigned long long)b.aa.refused(refusal::no_orders), (unsigned long long)b.aa.refused(refusal::no_levels),
                        (unsigned long long)b.aa.refused(refusal::no_ids));
            check(false, "seed " + std::to_string(seed) + ": the books agree with the reference at operation " + std::to_string(k));
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 5'000'000;
    for (const std::uint64_t seed : {20261103u, 7u}) {
        std::size_t deepest = 0;
        const bool ok = run(seed, n, deepest);
        check(ok, "S2, seed " + std::to_string(seed) + ": " + std::to_string(n) +
                      " random operations, every combination of D1 and D2 equal to the reference after each (answers, the order, "
                      "every level touched; every book in full each 100,000), deepest side " + std::to_string(deepest) + " levels");
    }
    return me::test::result();
}
