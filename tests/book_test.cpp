// S1 of note 002: each rule of the book, and each refusal, a test built to
// break it, for every combination of D1 and D2 and for the reference; after a
// refusal the book is unchanged.
// Design: docs/design/002-book.md
#include "check.hpp"

#include <me/market.hpp>
#include <me/reference_market.hpp>

#include <string>
#include <vector>

namespace {

using namespace me;
using me::test::check;

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

std::vector<price_t> prices(const std::vector<level_state>& s) {
    std::vector<price_t> p;
    for (const auto& l : s) p.push_back(l.price);
    return p;
}
std::vector<order_id> ids(const level_state& l) {
    std::vector<order_id> v;
    for (const auto& q : l.queue) v.push_back(q.id);
    return v;
}

template <class M>
void rules(const std::string& name, bool fixed) {
    const std::string n = "[" + name + "] ";
    {
        M m(small());
        m.add(0, side::buy, 1000, 10);
        m.add(0, side::buy, 1020, 10);
        m.add(0, side::buy, 1010, 10);
        m.add(0, side::sell, 2000, 10);
        m.add(0, side::sell, 1980, 10);
        m.add(0, side::sell, 1990, 10);
        check(prices(m.state(0, side::buy)) == std::vector<price_t>{1020, 1010, 1000} &&
                  prices(m.state(0, side::sell)) == std::vector<price_t>{1980, 1990, 2000},
              n + "4757(a)(1)(A): better prices first on each side (bids highest, asks lowest)");
    }
    {
        M m(small());
        const order_id a = m.add(1, side::buy, 1000, 5).id, b = m.add(1, side::buy, 1000, 7).id, c = m.add(1, side::buy, 1000, 9).id;
        const auto s = m.state(1, side::buy);
        check(s.size() == 1 && ids(s[0]) == std::vector<order_id>{a, b, c} && s[0].shares == 21 && s[0].count == 3,
              n + "4757(a)(1)(B): equal prices in time order, new orders at the back; the level's shares and count");
        check(a == 1 && b == 2 && c == 3, n + "IDs the engine's, in sequence from 1");

        m.cancel(b, 3);
        auto t = m.state(1, side::buy);
        check(ids(t[0]) == std::vector<order_id>{a, b, c} && t[0].queue[1].shares == 4 && t[0].shares == 18 && m.find(b)->shares == 4,
              n + "ITCH 5.0 Order Cancel: cancelling part keeps the order's place, its shares and the level's reduced");
        m.cancel(b, 4);
        t = m.state(1, side::buy);
        check(ids(t[0]) == std::vector<order_id>{a, c} && !m.find(b) && t[0].count == 2, n + "cancelling all it has left removes it");

        const order_id d = m.replace(a, 1000, 2).id;
        t = m.state(1, side::buy);
        check(d == 4 && ids(t[0]) == std::vector<order_id>{c, d} && !m.find(a) && m.find(d)->shares == 2,
              n + "ITCH 5.0 Order Replace: a new ID at the back of its level, even for a smaller size at the same price");
        const order_id e = m.replace(c, 1010, 9).id;
        t = m.state(1, side::buy);
        check(prices(t) == std::vector<price_t>{1010, 1000} && ids(t[0]) == std::vector<order_id>{e} && ids(t[1]) == std::vector<order_id>{d},
              n + "a replace to another price: a new level, the order moved");
        m.replace(d, 1010, 2);
        t = m.state(1, side::buy);
        check(prices(t) == std::vector<price_t>{1010} && t[0].count == 2, n + "a replace that empties its old level frees it");
        const order_id f = m.add(1, side::buy, 1010, 1).id;
        const order_id g = m.replace(f, 1010, 1).id;
        t = m.state(1, side::buy);
        check(ids(t[0]).back() == g && t[0].count == 3, n + "a replace of the last in its level to the same price: still at the back");
    }
    {
        M m(small());
        const order_id a = m.add(0, side::sell, 1500, 10).id;
        m.add(0, side::sell, 1500, 10);
        m.add(0, side::sell, 1501, 10);
        const auto before0 = m.state(0, side::sell);
        const auto before1 = m.state(0, side::buy);
        const order_id next = m.next_id();
        const auto same = [&] { return m.state(0, side::sell) == before0 && m.state(0, side::buy) == before1 && m.next_id() == next; };
        bool all = true;
        const auto refused = [&](answer x, refusal why, const std::string& what) {
            const bool ok = x.why == why && x.id == 0 && same();
            check(ok, n + "refused (" + to_string(why) + "), nothing changed: " + what);
            all = all && ok;
        };
        refused(m.add(0, side::sell, 1500, 0), refusal::size, "4703(b): add of 0 shares");
        refused(m.add(0, side::sell, 1500, max_shares + 1), refusal::size, "4703(b): add of 1,000,000 shares");
        refused(m.add(0, side::sell, 0, 10), refusal::price, "4703(c): add priced 0");
        refused(m.add(0, side::sell, max_price + 100, 10), refusal::price, "4703(c): add above $199,999.99");
        refused(m.add(4, side::sell, 1500, 10), refusal::instrument, "add for an instrument beyond the configured");
        refused(m.cancel(99, 1), refusal::unknown_id, "cancel part of an ID never entered");
        refused(m.cancel(99), refusal::unknown_id, "cancel of an ID never entered");
        refused(m.cancel(0), refusal::unknown_id, "cancel of ID 0");
        refused(m.cancel(a, 0), refusal::size, "cancel of 0 shares");
        refused(m.cancel(a, 11), refusal::too_many, "cancel of more shares than remain");
        refused(m.replace(a, 1500, 0), refusal::size, "4703(b): replace to 0 shares");
        refused(m.replace(a, max_price + 1, 10), refusal::price, "4703(c): replace above the maximum");
        refused(m.replace(99, 1500, 10), refusal::unknown_id, "replace of an ID never entered");
        m.cancel(a);
        const auto after0 = m.state(0, side::sell);
        const order_id next2 = m.next_id();
        const bool gone = m.cancel(a).why == refusal::unknown_id && m.cancel(a, 1).why == refusal::unknown_id &&
                          m.replace(a, 1500, 1).why == refusal::unknown_id && m.state(0, side::sell) == after0 && m.next_id() == next2;
        check(gone, n + "an ID finished (cancelled) is unknown to cancel and replace, nothing changed");
        check(m.add(0, side::buy, 1, 1).ok() && m.add(0, side::buy, max_price, max_shares).ok(),
              n + "4703(b), (c): 1 share at $0.0001 and 999,999 at $199,999.99 accepted");
        check(m.refused(refusal::size) == 4 && m.refused(refusal::unknown_id) == 7, n + "D6: refusals counted by reason");
    }
    if (!fixed) return;
    {
        config c = small();
        c.orders = 3;
        M m(c);
        m.add(0, side::buy, 100, 1);
        const order_id b = m.add(0, side::buy, 101, 1).id;
        m.add(0, side::buy, 102, 1);
        const auto before = m.state(0, side::buy);
        check(m.add(0, side::buy, 100, 1).why == refusal::no_orders && m.state(0, side::buy) == before,
              n + "D4: past the orders live at once, refused, nothing changed");
        m.cancel(b);
        check(m.add(0, side::buy, 103, 1).ok(), n + "D4: a cancelled order's record taken again");
        check(m.replace(b + 2, 105, 3).ok(), n + "D4: a replace at full order capacity reuses its record");
    }
    {
        config c = small();
        c.levels = 2;
        M m(c);
        m.add(0, side::buy, 100, 1);
        const order_id b = m.add(0, side::sell, 200, 1).id;
        const auto before = m.state(0, side::sell);
        check(m.add(0, side::sell, 201, 1).why == refusal::no_levels && m.add(0, side::sell, 200, 1).ok(),
              n + "D4: past the levels live at once, a new level refused; an existing one still taken");
        check(m.replace(b, 202, 1).why == refusal::no_levels && m.find(b), n + "D4: a replace needing a level past capacity refused, the order kept");
        (void)before;
    }
}

template <class M>
void ids_capacity(const std::string& name) {
    config c = small();
    c.ids = 2;
    M m(c);
    const order_id a = m.add(0, side::buy, 100, 1).id;
    m.add(0, side::buy, 100, 1);
    const auto before = m.state(0, side::buy);
    check(m.add(0, side::buy, 100, 1).why == refusal::no_ids && m.replace(a, 100, 1).why == refusal::no_ids &&
              m.state(0, side::buy) == before,
          "[" + name + "] D1 A: past the IDs entered, adds and replaces refused, nothing changed");
}

}  // namespace

int main() {
    rules<market<levels_sorted, table_direct>>("levels A, table A", true);
    rules<market<levels_sorted, table_hash>>("levels A, table B", true);
    rules<market<levels_map, table_direct>>("levels B, table A", true);
    rules<market<levels_map, table_hash>>("levels B, table B", true);
    rules<market<levels_sorted, table_low>>("levels A, table C", true);
    rules<market<levels_map, table_low>>("levels B, table C", true);
    rules<reference_market>("reference", false);
    ids_capacity<market<levels_sorted, table_direct>>("levels A, table A");
    ids_capacity<market<levels_map, table_direct>>("levels B, table A");
    return me::test::result();
}
