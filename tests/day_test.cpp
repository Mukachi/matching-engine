// S4 of note 002 (label `data`): the sample day's displayed book events (D7)
// applied to every combination of D1 and D2 and to the reference: each Add a
// new order (ITCH's reference number mapped to the engine's ID, here in the
// test, outside the books), each Order Executed, Executed With Price and
// Order Cancel a reduction of the named order, each Order Delete a whole
// cancel, each Order Replace a replace. After every event the same answers and
// every level touched the same in full; every 50 million messages and at the
// end every book in full, and equal to the feed handler's reference book fed
// the same day (its levels, and its queues with reference numbers mapped to
// IDs).
// Usage: day_test [day file] [messages] (default the whole day)
// Design: docs/design/002-book.md
#include "check.hpp"

#include "../tools/day_file.hpp"

#include <me/market.hpp>
#include <me/reference_market.hpp>

#include <itchfh/event.hpp>
#include <itchfh/reference_book.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

using namespace me;
using me::test::check;

config day() {
    config c;  // the defaults: sized for the day
    c.prefault = false;
    return c;
}

struct books {
    market<levels_sorted, table_direct> aa{day()};
    market<levels_sorted, table_hash> ab{day()};
    market<levels_map, table_direct> ba{day()};
    market<levels_map, table_hash> bb{day()};
    reference_market ref{day()};
};

template <class F>
bool all(books& b, F&& f) {
    return f(b.aa) && f(b.ab) && f(b.ba) && f(b.bb);
}
bool same(const answer& a, const answer& b) { return a.why == b.why && a.id == b.id; }

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ME_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::uint64_t limit = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : ~0ull;
    day_file f(path);
    if (!f.open()) {
        check(false, "the sample day at " + path);
        return me::test::result();
    }
    books b;
    itchfh::reference_book theirs;
    std::unordered_map<std::uint64_t, order_id> id_of;  // ITCH reference number to the engine's ID
    std::uint64_t events = 0, refused[refusals] = {}, skipped = 0, disagree = 0;
    std::uint64_t first_disagreement = 0;
    // Every book in full, ours against the reference, and the reference against
    // the feed handler's (reference numbers mapped to IDs): every 50 million
    // messages and at the end (by the close the books are empty, so the
    // checkpoints are what compares full books, the peak among them).
    bool full = true, vs_theirs = true;
    std::size_t compared = 0, unmatched = 0, checkpoints = 0, most_live = 0;
    const auto compare_all = [&] {
        ++checkpoints;
        most_live = std::max(most_live, b.ref.live_orders());
        for (const std::uint16_t loc : theirs.instruments()) {
            ++compared;
            for (const side sd : {side::buy, side::sell}) {
                const auto r = b.ref.state(loc, sd);
                full = full && all(b, [&](auto& x) { return x.state(loc, sd) == r; });
                const auto ts = sd == side::buy ? itchfh::side::bid : itchfh::side::ask;
                const auto lv = theirs.levels(loc, ts);
                bool same_book = lv.size() == r.size();
                for (std::size_t j = 0; same_book && j < lv.size(); ++j) {
                    same_book = lv[j].price == r[j].price && lv[j].shares == r[j].shares && lv[j].orders == r[j].count;
                    const auto q = theirs.queue(loc, ts, lv[j].price);
                    for (std::size_t k = 0; same_book && k < q.size(); ++k) {
                        const auto it = id_of.find(q[k]);
                        same_book = it != id_of.end() && it->second == r[j].queue[k].id;
                    }
                }
                if (!same_book) {
                    vs_theirs = false;
                    ++unmatched;
                }
            }
        }
        full = full && all(b, [&](auto& x) { return x.live_orders() == b.ref.live_orders(); });
        vs_theirs = vs_theirs && b.ref.live_orders() == theirs.live_orders();
    };
    itchfh::message m;
    while (f.sequence() < limit && f.next(m)) {
        itchfh::event e{};
        e.timestamp_ns = m.timestamp_ns;
        e.locate = m.locate;
        std::vector<std::tuple<instrument_t, side, price_t>> touched;
        const auto mine = [&](std::uint64_t ref) -> order_id {
            const auto it = id_of.find(ref);
            return it == id_of.end() ? 0 : it->second;
        };
        const auto touch = [&](order_id id) {
            if (const auto o = b.ref.find(id)) touched.emplace_back(o->instrument, o->side, o->price);
        };
        bool ok = true;
        answer want;
        switch (m.type) {
            case 'A':
            case 'F': {
                e.type = itchfh::event_type::add;
                e.ref = m.A.ref, e.side = m.A.side, e.shares = m.A.shares, e.price = m.A.price;
                const side s = m.A.side == 'B' ? side::buy : side::sell;
                want = b.ref.add(m.locate, s, m.A.price, m.A.shares);
                ok = all(b, [&](auto& x) { return same(x.add(m.locate, s, m.A.price, m.A.shares), want); });
                if (want.ok()) id_of[m.A.ref] = want.id;
                touched.emplace_back(m.locate, s, m.A.price);
                break;
            }
            case 'E':
            case 'C':
            case 'X': {
                const bool cancel = m.type == 'X';
                e.type = cancel ? itchfh::event_type::cancel : m.type == 'E' ? itchfh::event_type::executed : itchfh::event_type::executed_price;
                e.ref = cancel ? m.X.ref : m.E.ref;
                e.shares = cancel ? m.X.shares : m.E.shares;
                e.price = cancel ? 0 : m.E.price;
                const order_id id = mine(e.ref);
                if (!id) {
                    ++skipped;
                    break;
                }
                touch(id);
                want = b.ref.cancel(id, e.shares);
                ok = all(b, [&](auto& x) { return same(x.cancel(id, e.shares), want); });
                if (want.ok() && !b.ref.find(id)) id_of.erase(e.ref);
                break;
            }
            case 'D': {
                e.type = itchfh::event_type::remove;
                e.ref = m.D.ref;
                const order_id id = mine(e.ref);
                if (!id) {
                    ++skipped;
                    break;
                }
                touch(id);
                want = b.ref.cancel(id);
                ok = all(b, [&](auto& x) { return same(x.cancel(id), want); });
                if (want.ok()) id_of.erase(e.ref);
                break;
            }
            case 'U': {
                e.type = itchfh::event_type::replace;
                e.ref = m.U.old_ref, e.new_ref = m.U.new_ref, e.shares = m.U.shares, e.price = m.U.price;
                const order_id id = mine(e.ref);
                if (!id) {
                    ++skipped;
                    break;
                }
                const auto o = b.ref.find(id);
                touch(id);
                want = b.ref.replace(id, m.U.price, m.U.shares);
                ok = all(b, [&](auto& x) { return same(x.replace(id, m.U.price, m.U.shares), want); });
                if (want.ok()) {
                    id_of.erase(e.ref);
                    id_of[m.U.new_ref] = want.id;
                    touched.emplace_back(o->instrument, o->side, m.U.price);
                }
                break;
            }
            case 'H':
                e.type = itchfh::event_type::trading_state;
                e.side = m.H.trading_state;
                break;
            default:
                continue;
        }
        theirs.apply(e);
        if (f.sequence() % 50'000'000 == 0) compare_all();
        if (m.type == 'H') continue;
        ++events;
        if (!want.ok()) ++refused[static_cast<int>(want.why)];
        for (const auto& [i, sd, p] : touched) {
            const auto r = b.ref.level(i, sd, p);
            ok = ok && all(b, [&](auto& x) { return x.level(i, sd, p) == r; });
        }
        if (!ok && disagree++ == 0) first_disagreement = f.sequence();
    }
    check(disagree == 0, "S4: " + std::to_string(events) + " book events of " + std::to_string(f.sequence()) +
                             " messages; every combination of D1 and D2 equal to the reference after each (answers, every level touched)" +
                             (disagree ? "; first disagreement at message " + std::to_string(first_disagreement) : ""));
    std::string why;
    for (int r = 1; r < refusals; ++r)
        if (refused[r]) why += std::string(" ") + to_string(static_cast<refusal>(r)) + " " + std::to_string(refused[r]);
    std::printf("      refused:%s; events for an order not entered (refused before, or unknown): %llu\n", why.empty() ? " none" : why.c_str(),
                static_cast<unsigned long long>(skipped));
    compare_all();
    check(f.sequence() > 0, "the sample day read (" + std::to_string(f.sequence()) + " messages)");
    check(full, "S4: every book in full equal to the reference at " + std::to_string(checkpoints) +
                    " checkpoints (every 50 million messages and the end; up to " + std::to_string(most_live) +
                    " orders live; " + std::to_string(compared) + " instrument books compared)");
    check(vs_theirs, "S4: at the same checkpoints the reference equal to the feed handler's reference book: every level and every queue, in order" +
                         (unmatched ? " (" + std::to_string(unmatched) + " sides differ)" : std::string()));

    std::printf("      peak: %zu orders live, %zu levels (levels A, table A)\n", b.aa.peak_orders(), b.aa.peak_levels());
    return me::test::result();
}
