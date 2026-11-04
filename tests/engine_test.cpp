// S1 of note 003: each rule of matching, a test built to break it, for the
// engine over every combination of note 002's D1 and D2 and for the reference
// engine: each message's reports in sequence, and the books after it. A
// refused message reports only its refusal and changes nothing; no book is
// crossed after any message.
// Design: docs/design/003-matching.md
#include "check.hpp"
#include "reports.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/reference_engine.hpp>
#include <me/reference_market.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace me;
using me::test::check;
using me::test::recorder;
using me::test::report;
using reports = std::vector<report>;

config small() {
    config c;
    c.orders = 64;
    c.levels = 64;
    c.level_arena = 4096;
    c.ids = 1024;
    c.instruments = 4;
    c.prefault = false;
    return c;
}

report A(order_id id) { return {report::accepted, id}; }
report X(refusal why) { return {report::refused, 0, why}; }
report T(std::uint64_t n, order_id taker, order_id maker, price_t p, shares_t q, shares_t left, instrument_t i = 0) {
    return {report::trade, 0, refusal::none, {n, taker, maker, i, p, q, left}};
}
report R(order_id id, price_t p, shares_t q) { return {report::rested, id, refusal::none, {}, p, q}; }
report C(order_id id, shares_t q, cause c) { return {report::cancelled, id, refusal::none, {}, 0, q, c}; }

std::string show(const reports& rs) {
    std::string s;
    for (const auto& r : rs) s += (s.empty() ? "" : "; ") + me::test::to_string(r);
    return s;
}

template <class M>
struct fast {
    explicit fast(const config& c = small()) : m(c), e(m, r) {}
    static constexpr bool direct = std::string_view(M::table_name).starts_with("A");
    M m;
    recorder r;
    engine<M, recorder> e;
};
struct ref {
    explicit ref(const config& c = small()) : m(c), e(m, r) {}
    static constexpr bool direct = false;
    reference_market m;
    recorder r;
    reference_engine<recorder> e;
};

std::vector<price_t> prices(const std::vector<level_state>& s) {
    std::vector<price_t> p;
    for (const auto& l : s) p.push_back(l.price);
    return p;
}
std::vector<queued> queue(const std::vector<level_state>& s, std::size_t i) { return i < s.size() ? s[i].queue : std::vector<queued>{}; }

template <class Rig>
bool uncrossed(const Rig& g) {
    for (instrument_t i = 0; i < 4; ++i) {
        const auto b = g.m.state(i, side::buy), a = g.m.state(i, side::sell);
        if (!b.empty() && !a.empty() && b.front().price >= a.front().price) return false;
    }
    return true;
}

template <class Rig>
void rules(const std::string& name, bool fixed) {
    const std::string n = "[" + name + "] ";
    bool never_crossed = true;
    // Each message's reports, the book checked uncrossed after it.
    const auto msg = [&](Rig& g, auto&& f) {
        f(g.e);
        never_crossed = never_crossed && uncrossed(g);
        return g.r.take();
    };
    const auto rest = [&](Rig& g, side s, price_t p, shares_t q, instrument_t i = 0) {
        msg(g, [&](auto& e) { e.enter(i, s, p, q, tif::rest); });
    };
    {
        Rig g;
        rest(g, side::sell, 1030, 10), rest(g, side::sell, 1010, 10), rest(g, side::sell, 1020, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1030, 10, tif::rest); });
        check(r == reports{A(4), T(1, 4, 2, 1010, 10, 0)} && prices(g.m.state(0, side::sell)) == std::vector<price_t>{1020, 1030},
              n + "4757(a)(1)(A): a buy meets the lowest ask first, not the first entered: " + show(r));
    }
    {
        Rig g;
        rest(g, side::buy, 1000, 10), rest(g, side::buy, 1020, 10), rest(g, side::buy, 1010, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::sell, 1000, 10, tif::rest); });
        check(r == reports{A(4), T(1, 4, 2, 1020, 10, 0)}, n + "4757(a)(1)(A): a sell meets the highest bid first: " + show(r));
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 5), rest(g, side::sell, 1010, 5);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 5, tif::rest); });
        check(r == reports{A(3), T(1, 3, 1, 1010, 5, 0)} && queue(g.m.state(0, side::sell), 0) == std::vector<queued>{{2, 5}},
              n + "4757(a)(1)(B): at a price, the oldest order first: " + show(r));
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1050, 10, tif::rest); });
        rest(g, side::buy, 1000, 10);
        const auto s = msg(g, [](auto& e) { e.enter(0, side::sell, 900, 10, tif::rest); });
        check(r == reports{A(2), T(1, 2, 1, 1010, 10, 0)} && s == reports{A(4), T(2, 4, 3, 1000, 10, 0)},
              n + "4757(a)(3): a trade at the resting order's price, not the taker's limit (buy 1050 at 1010, sell 900 at 1000)");
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 10), rest(g, side::sell, 1020, 10), rest(g, side::sell, 1030, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1020, 25, tif::rest); });
        check(r == reports{A(4), T(1, 4, 1, 1010, 10, 0), T(2, 4, 2, 1020, 10, 0), R(4, 1020, 5)} &&
                  prices(g.m.state(0, side::sell)) == std::vector<price_t>{1030} &&
                  queue(g.m.state(0, side::buy), 0) == std::vector<queued>{{4, 5}},
              n + "a sweep: best level first, each at its own price, stopping at the limit; the rest rests at the limit: " + show(r));
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 300), rest(g, side::sell, 1010, 100);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 100, tif::rest); });
        const bool kept = queue(g.m.state(0, side::sell), 0) == std::vector<queued>{{1, 200}, {2, 100}};
        const auto s = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 250, tif::rest); });
        check(r == reports{A(3), T(1, 3, 1, 1010, 100, 200)} && kept && s == reports{A(4), T(2, 4, 1, 1010, 200, 0), T(3, 4, 2, 1010, 50, 50)} &&
                  queue(g.m.state(0, side::sell), 0) == std::vector<queued>{{2, 50}},
              n + "4757(a)(2): a trade reduces the resting order, which keeps its place: " + show(r) + " / " + show(s));
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1020, 15, tif::ioc); });
        check(r == reports{A(2), T(1, 2, 1, 1010, 10, 0), C(2, 5, cause::ioc)} && !g.m.find(2) && g.m.state(0, side::buy).empty(),
              n + "4703(a)(1): an IOC matches as far as it goes; the rest cancelled, never rested: " + show(r));
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 10);
        const auto before = g.m.state(0, side::sell);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1000, 5, tif::ioc); });
        msg(g, [](auto& e) { e.enter(0, side::buy, 990, 5, tif::rest); });
        check(r == reports{A(2), C(2, 5, cause::ioc)} && g.m.state(0, side::sell) == before && g.m.find(3),
              n + "4703(a)(1): an IOC that meets nothing is cancelled whole, the book unchanged; D4: it took ID 2, the next order 3: " + show(r));
    }
    {
        Rig g;
        rest(g, side::buy, 1000, 10), rest(g, side::sell, 1020, 10);
        const auto r = msg(g, [](auto& e) { e.replace(1, 1010, 7); });
        check(r == reports{C(1, 10, cause::replaced), A(3), R(3, 1010, 7)} && !g.m.find(1),
              n + "a replace that does not cross: the old cancelled, the new accepted and rested: " + show(r));
    }
    {
        Rig g;
        rest(g, side::buy, 1000, 10), rest(g, side::sell, 1020, 4);
        const auto r = msg(g, [](auto& e) { e.replace(1, 1020, 10); });
        check(r == reports{C(1, 10, cause::replaced), A(3), T(1, 3, 2, 1020, 4, 0), R(3, 1020, 6)},
              n + "D3: a replace that crosses matches as a new order, the rest rests: " + show(r));
    }
    {
        Rig g;
        rest(g, side::buy, 1000, 5), rest(g, side::sell, 1020, 10);
        const auto r = msg(g, [](auto& e) { e.replace(1, 1030, 5); });
        msg(g, [](auto& e) { e.enter(0, side::buy, 900, 1, tif::rest); });
        check(r == reports{C(1, 5, cause::replaced), A(3), T(1, 3, 2, 1020, 5, 5)} && g.m.find(4) && g.m.state(0, side::buy).size() == 1,
              n + "D3: a replace filled whole rests nothing; its ID taken (the next order 4): " + show(r));
    }
    {
        Rig g;
        rest(g, side::sell, 1010, 10), rest(g, side::sell, 2010, 10, 1);
        const auto r = msg(g, [](auto& e) { e.enter(1, side::buy, 2010, 10, tif::rest); });
        const auto s = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 10, tif::rest); });
        check(r == reports{A(3), T(1, 3, 2, 2010, 10, 0, 1)} && s == reports{A(4), T(2, 4, 1, 1010, 10, 0, 0)},
              n + "match numbers one sequence across instruments; each book matches only its own instrument");
    }
    {
        Rig g;
        rest(g, side::buy, 1000, 10), rest(g, side::buy, 1000, 10);
        const auto r = msg(g, [](auto& e) { e.cancel(1, 4); });
        const bool kept = queue(g.m.state(0, side::buy), 0) == std::vector<queued>{{1, 6}, {2, 10}};
        const auto s = msg(g, [](auto& e) { e.cancel(1); });
        check(r == reports{C(1, 4, cause::requested)} && kept && s == reports{C(1, 6, cause::requested)} && queue(g.m.state(0, side::buy), 0) == std::vector<queued>{{2, 10}},
              n + "cancels report what they removed; cancelling part keeps the place");
    }
    {
        // Each refusal reports only itself and changes nothing.
        Rig g;
        rest(g, side::buy, 1000, 10), rest(g, side::sell, 1010, 10);
        const auto b = g.m.state(0, side::buy), a = g.m.state(0, side::sell);
        bool only = true, unchanged = true;
        const auto refused = [&](refusal why, auto&& f) {
            const auto r = msg(g, f);
            only = only && r == reports{X(why)};
            unchanged = unchanged && g.m.state(0, side::buy) == b && g.m.state(0, side::sell) == a;
        };
        refused(refusal::size, [](auto& e) { e.enter(0, side::buy, 1010, 0, tif::rest); });
        refused(refusal::size, [](auto& e) { e.enter(0, side::buy, 1010, max_shares + 1, tif::ioc); });
        refused(refusal::price, [](auto& e) { e.enter(0, side::buy, 0, 10, tif::rest); });
        refused(refusal::price, [](auto& e) { e.enter(0, side::buy, max_price + 1, 10, tif::ioc); });
        refused(refusal::instrument, [](auto& e) { e.enter(4, side::buy, 1010, 10, tif::rest); });
        refused(refusal::unknown_id, [](auto& e) { e.cancel(99); });
        refused(refusal::unknown_id, [](auto& e) { e.cancel(99, 1); });
        refused(refusal::size, [](auto& e) { e.cancel(1, 0); });
        refused(refusal::too_many, [](auto& e) { e.cancel(1, 11); });
        refused(refusal::unknown_id, [](auto& e) { e.replace(99, 1010, 10); });
        refused(refusal::size, [](auto& e) { e.replace(1, 1010, 0); });
        refused(refusal::price, [](auto& e) { e.replace(1, 0, 10); });
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 990, 1, tif::rest); });
        check(only && unchanged && r == reports{A(3), R(3, 990, 1)},
              n + "each refusal (size, price, instrument, unknown ID, zero, too many; by enter, cancel and replace) reports only "
                  "itself, changes nothing and takes no ID");
    }
    if (fixed) {
        // Room is needed only by what rests (Shep's decision B): two records, both in use.
        config c = small();
        c.orders = 2;
        Rig g(c);
        rest(g, side::buy, 1000, 10), rest(g, side::sell, 1010, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1005, 5, tif::rest); });
        const auto s = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 5, tif::rest); });
        const auto t = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 10, tif::rest); });
        check(r == reports{X(refusal::no_orders)} && s == reports{A(3), T(1, 3, 2, 1010, 5, 5)} &&
                  t == reports{A(4), T(2, 4, 2, 1010, 5, 0), R(4, 1010, 5)},
              n + "no record free: an order that does not cross is refused; one that fills whole needs none; a remainder rests in "
                  "the room its trades freed: " + show(s) + " / " + show(t));
    }
    if constexpr (Rig::direct) {
        // No ID table A can hold: a remainder with no room cancelled, its trades kept.
        config c = small();
        c.ids = 2;
        Rig g(c);
        rest(g, side::buy, 1000, 10), rest(g, side::sell, 1010, 10);
        const auto r = msg(g, [](auto& e) { e.enter(0, side::buy, 1005, 5, tif::rest); });
        const auto s = msg(g, [](auto& e) { e.enter(0, side::buy, 1010, 15, tif::rest); });
        const auto q = g.m.state(0, side::buy);
        check(r == reports{X(refusal::no_ids)} && s == reports{A(3), T(1, 3, 2, 1010, 10, 0), C(3, 5, cause::no_room)} &&
                  q.size() == 1 && q[0].price == 1000 && g.m.state(0, side::sell).empty() && g.e.no_room() == 1,
              n + "a remainder with no room to rest is cancelled (no room), its trades kept: " + show(s));
    }
    check(never_crossed, n + "no book crossed after any message of these tests");
}

}  // namespace

int main() {
    rules<fast<market<levels_sorted, table_direct>>>("levels A, table A", true);
    rules<fast<market<levels_sorted, table_hash>>>("levels A, table B", true);
    rules<fast<market<levels_map, table_direct>>>("levels B, table A", true);
    rules<fast<market<levels_map, table_hash>>>("levels B, table B", true);
    rules<ref>("reference", false);
    return me::test::result();
}
