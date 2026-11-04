// S2 and S3 of note 004 (label `data`): the sample day replayed as order entry
// (D1) through the engine over every combination of note 002's D1 and D2 and
// through the reference engine, in lockstep (D3); the prediction made on the
// reference engine's book. After every message the same reports and every
// level touched the same; every 50 million messages and at the end every book
// in full equal to the reference, and the reference to the feed handler's
// reference book fed the same day (its queues' reference numbers mapped to
// IDs). Then the oracle's counts (D2), as `replay_oracle` publishes them.
// Usage: replay_day_test [day file] [messages] (default the whole day)
// Design: docs/design/004-replay.md
#include "check.hpp"
#include "reports.hpp"

#include "../tools/day_file.hpp"
#include "../tools/replay.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/reference_engine.hpp>
#include <me/reference_market.hpp>

#include <itchfh/event.hpp>
#include <itchfh/reference_book.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace me;
using me::test::check;
using me::test::recorder;

config day() {
    config c;  // the defaults: sized for the day
    c.prefault = false;
    return c;
}

template <class M>
struct fast {
    std::unique_ptr<M> m = std::make_unique<M>(day());
    recorder r;
    engine<M, recorder> e{*m, r};
};
struct engines {
    fast<market<levels_sorted, table_direct>> aa;
    fast<market<levels_sorted, table_hash>> ab;
    fast<market<levels_map, table_direct>> ba;
    fast<market<levels_map, table_hash>> bb;
    reference_market rm{day()};
    recorder rr;
    reference_engine<recorder> re{rm, rr};
    template <class F>
    bool all(F&& f) {
        return f(*aa.m, aa) && f(*ab.m, ab) && f(*ba.m, ba) && f(*bb.m, bb);
    }
};

// The day's event for the feed handler's reference book.
bool to_event(const itchfh::message& m, itchfh::event& e) {
    e = {};
    e.timestamp_ns = m.timestamp_ns;
    e.locate = m.locate;
    switch (m.type) {
        case 'A':
        case 'F': e.type = itchfh::event_type::add, e.ref = m.A.ref, e.side = m.A.side, e.shares = m.A.shares, e.price = m.A.price; return true;
        case 'E': e.type = itchfh::event_type::executed, e.ref = m.E.ref, e.shares = m.E.shares; return true;
        case 'C': e.type = itchfh::event_type::executed_price, e.ref = m.E.ref, e.shares = m.E.shares, e.price = m.E.price; return true;
        case 'X': e.type = itchfh::event_type::cancel, e.ref = m.X.ref, e.shares = m.X.shares; return true;
        case 'D': e.type = itchfh::event_type::remove, e.ref = m.D.ref; return true;
        case 'U':
            e.type = itchfh::event_type::replace, e.ref = m.U.old_ref, e.new_ref = m.U.new_ref, e.shares = m.U.shares, e.price = m.U.price;
            return true;
        case 'H': e.type = itchfh::event_type::trading_state, e.side = m.H.trading_state; return true;
        default: return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ME_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::uint64_t limit = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : ~0ull;
    day_file f(path);
    if (!f.open()) {
        check(false, "the sample day at " + path);
        return me::test::result();
    }
    auto g = std::make_unique<engines>();
    itchfh::reference_book theirs;
    replay p;
    std::uint64_t steps = 0, disagree = 0, first_disagreement = 0;
    bool full = true, vs_theirs = true;
    std::size_t compared = 0, unmatched = 0, checkpoints = 0, most_live = 0;
    const auto compare_all = [&] {
        ++checkpoints;
        most_live = std::max(most_live, g->rm.live_orders());
        for (const std::uint16_t loc : theirs.instruments()) {
            ++compared;
            for (const side sd : {side::buy, side::sell}) {
                const auto r = g->rm.state(loc, sd);
                full = full && g->all([&](auto& m, auto&) { return m.state(loc, sd) == r; });
                const auto ts = sd == side::buy ? itchfh::side::bid : itchfh::side::ask;
                const auto lv = theirs.levels(loc, ts);
                bool same = lv.size() == r.size();
                for (std::size_t j = 0; same && j < lv.size(); ++j) {
                    same = lv[j].price == r[j].price && lv[j].shares == r[j].shares && lv[j].orders == r[j].count;
                    const auto q = theirs.queue(loc, ts, lv[j].price);
                    for (std::size_t k = 0; same && k < q.size(); ++k) same = p.id_of(q[k]) == r[j].queue[k].id;
                }
                if (!same) {
                    vs_theirs = false;
                    ++unmatched;
                }
            }
        }
        full = full && g->all([&](auto& m, auto&) { return m.live_orders() == g->rm.live_orders() && m.next_id() == g->rm.next_id(); });
        vs_theirs = vs_theirs && g->rm.live_orders() == theirs.live_orders();
    };
    itchfh::message m;
    while (f.sequence() < limit && f.next(m)) {
        itchfh::event ev;
        const bool book_event = to_event(m, ev);
        const step st = p.next(m, g->rm);
        if (st.k != step::none) {
            ++steps;
            // What the step touches: its order's level before, the levels after.
            std::vector<std::tuple<instrument_t, side, price_t>> touched;
            const auto before = st.id ? g->rm.find(st.id) : std::nullopt;
            if (before) touched.emplace_back(before->instrument, before->side, before->price);
            if (st.k == step::enter || st.k == step::rest) touched.emplace_back(st.instrument, st.s, st.price);
            if (st.k == step::replace || st.k == step::replace_rest) touched.emplace_back(st.instrument, st.s, st.price);
            apply(st, g->re, g->rm);
            const auto want = g->rr.take();
            bool ok = g->all([&](auto& mk, auto& x) {
                apply(st, x.e, mk);
                return x.r.take() == want;
            });
            for (const auto& [i, sd, pr] : touched) {
                const auto r = g->rm.level(i, sd, pr);
                ok = ok && g->all([&](auto& mk, auto&) { return mk.level(i, sd, pr) == r; });
            }
            if (!ok && disagree++ == 0) first_disagreement = f.sequence();
        }
        p.done();
        if (book_event) theirs.apply(ev);
        if (f.sequence() % 50'000'000 == 0) compare_all();
    }
    compare_all();
    check(f.sequence() > 0, "the sample day read (" + std::to_string(f.sequence()) + " messages)");
    const replay_counts& c = p.counts();
    check(disagree == 0, "S2: " + std::to_string(steps) + " steps of " + std::to_string(f.sequence()) +
                             " messages; every combination of D1 and D2 equal to the reference engine after each (reports, every level touched)" +
                             (disagree ? "; first disagreement at message " + std::to_string(first_disagreement) : ""));
    check(full, "S2: every book in full equal to the reference at " + std::to_string(checkpoints) + " checkpoints (up to " +
                    std::to_string(most_live) + " orders live; " + std::to_string(compared) + " instrument books)");
    check(vs_theirs, "S2: at the same checkpoints the reference equal to the feed handler's reference book: every level and every queue" +
                         (unmatched ? " (" + std::to_string(unmatched) + " sides differ)" : std::string()));
    std::printf("      S3: %llu executions, %llu reproduced (%.4f%%); not reproduced:", (unsigned long long)c.executions,
                (unsigned long long)c.reproduced, c.executions ? 100.0 * c.reproduced / c.executions : 0.0);
    for (int k = 0; k < misses; ++k) std::printf(" %s %llu;", to_string(static_cast<miss>(k)), (unsigned long long)c.by_cause[k]);
    std::printf("\n      printable C at another price %llu (%llu of them reproduced); adds %llu, rested without matching:", (unsigned long long)c.c_other_price,
                (unsigned long long)c.c_other_price_reproduced, (unsigned long long)c.adds);
    for (int k = 0; k < misses; ++k)
        if (c.adds_rested[k]) std::printf(" %s %llu;", to_string(static_cast<miss>(k)), (unsigned long long)c.adds_rested[k]);
    std::printf(" replaces %llu, rested without matching:", (unsigned long long)c.replaces);
    for (int k = 0; k < misses; ++k)
        if (c.replaces_rested[k]) std::printf(" %s %llu;", to_string(static_cast<miss>(k)), (unsigned long long)c.replaces_rested[k]);
    std::printf(" unknown references %llu; IDs taken %llu\n", (unsigned long long)c.unknown, (unsigned long long)(g->rm.next_id() - 1));
    return me::test::result();
}
