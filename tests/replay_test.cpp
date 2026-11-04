// S1 of note 004: the replay's mapping and its prediction (D1) on hand-made
// days, each case built to break it, through the engine (levels A, table A)
// and the reference engine: the step chosen, the cause counted (D2), the
// reports, and the book after it.
// Design: docs/design/004-replay.md
#include "check.hpp"
#include "reports.hpp"

#include "../tools/replay.hpp"

#include <me/engine.hpp>
#include <me/market.hpp>
#include <me/reference_engine.hpp>
#include <me/reference_market.hpp>

#include <itchfh/messages.hpp>

#include <string>
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

itchfh::message add(std::uint64_t ref, char side, std::uint32_t price, std::uint32_t shares, std::uint16_t loc = 0) {
    itchfh::message m{};
    m.type = 'A', m.locate = loc;
    m.A.ref = ref, m.A.side = side, m.A.shares = shares, m.A.price = price;
    return m;
}
itchfh::message exec(std::uint64_t ref, std::uint32_t shares, char type = 'E', char printable = 'Y', std::uint32_t price = 0) {
    itchfh::message m{};
    m.type = type;
    m.E.ref = ref, m.E.shares = shares, m.E.printable = printable, m.E.price = price;
    return m;
}
itchfh::message cancel(std::uint64_t ref, std::uint32_t shares) {
    itchfh::message m{};
    m.type = 'X';
    m.X.ref = ref, m.X.shares = shares;
    return m;
}
itchfh::message remove(std::uint64_t ref) {
    itchfh::message m{};
    m.type = 'D';
    m.D.ref = ref;
    return m;
}
itchfh::message replace(std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t price, std::uint32_t shares) {
    itchfh::message m{};
    m.type = 'U';
    m.U.old_ref = old_ref, m.U.new_ref = new_ref, m.U.price = price, m.U.shares = shares;
    return m;
}
itchfh::message state(char s, std::uint16_t loc = 0) {
    itchfh::message m{};
    m.type = 'H', m.locate = loc;
    m.H.trading_state = s;
    return m;
}

// The engine and the reference engine, each with its replay deciding on its own book.
struct day {
    market<levels_sorted, table_direct> m{small()};
    recorder r;
    engine<market<levels_sorted, table_direct>, recorder> e{m, r};
    reference_market rm{small()};
    recorder rr;
    reference_engine<recorder> re{rm, rr};
    replay p, rp;
    bool agree = true;
    // One message: both steps, both books' reports; the step and the reports.
    std::pair<step::kind, reports> operator()(const itchfh::message& msg) {
        const step a = p.next(msg, m), b = rp.next(msg, rm);
        apply(a, e, m), apply(b, re, rm);
        p.done(), rp.done();
        const reports x = r.take(), y = rr.take();
        agree = agree && a.k == b.k && x == y && m.state(0, side::buy) == rm.state(0, side::buy) && m.state(0, side::sell) == rm.state(0, side::sell);
        return {a.k, x};
    }
};

report T(std::uint64_t n, order_id taker, order_id maker, price_t p, shares_t q, shares_t left) {
    return {report::trade, 0, refusal::none, {n, taker, maker, 0, p, q, left}};
}
report A(order_id id) { return {report::accepted, id}; }
report R(order_id id, price_t p, shares_t q) { return {report::rested, id, refusal::none, {}, p, q}; }
report C(order_id id, shares_t q, cause c) { return {report::cancelled, id, refusal::none, {}, 0, q, c}; }

std::vector<queued> bids(const day& d) {
    const auto s = d.m.state(0, side::buy);
    std::vector<queued> v;
    for (const auto& l : s)
        for (const auto& q : l.queue) v.push_back(q);
    return v;
}

}  // namespace

int main() {
    bool agree = true;
    {
        day d;
        const auto [k, r] = d(add(11, 'B', 1000, 100));
        const auto [k2, r2] = d(exec(11, 40));
        check(k == step::enter && r == reports{A(1), R(1, 1000, 100)} && k2 == step::execute && r2 == reports{A(2), T(1, 2, 1, 1000, 40, 60)} &&
                  d.p.counts().reproduced == 1 && d.p.counts().executions == 1,
              "an execution of the order first in line reproduced: an IOC on the other side at its price, for the shares; ID 2 taken");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100)), d(add(12, 'B', 1000, 100));
        const auto [k, r] = d(exec(12, 30));
        check(k == step::execute_apply && r == reports{C(2, 30, cause::requested)} && bids(d) == std::vector<queued>{{1, 100}, {2, 70}} &&
                  d.p.counts().by_cause[static_cast<int>(miss::earlier_orders)] == 1 && d.p.counts().reproduced == 0,
              "an execution of an order behind another at its price: not reproduced (earlier orders ahead), applied to that order");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100)), d(add(12, 'B', 1010, 100));
        const auto [k, r] = d(exec(11, 100));
        check(k == step::execute_apply && bids(d) == std::vector<queued>{{2, 100}} &&
                  d.p.counts().by_cause[static_cast<int>(miss::better_price)] == 1,
              "an execution of an order at a worse price than the best: a better price ahead, applied; the order gone");
        const auto [k2, r2] = d(exec(12, 10));
        check(k2 == step::execute && d.p.counts().reproduced == 1, "its reference dropped; the next execution, of the best, reproduced");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100)), d(add(12, 'B', 1000, 100));
        const auto [k, r] = d(exec(12, 50, 'C', 'N', 1005));
        const auto [k2, r2] = d(exec(11, 50, 'C', 'N', 1005));
        check(k == step::execute_apply && d.p.counts().by_cause[static_cast<int>(miss::cross)] == 1 && k2 == step::execute &&
                  d.p.counts().reproduced == 1 && d.p.counts().c_other_price == 0,
              "a non-printable C not first in line counted as a cross; one first in line reproduced; neither counted as a printed price");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100)), d(add(12, 'B', 1000, 100));
        d(state('H'));
        const auto [k, r] = d(exec(12, 10));
        const auto [k2, r2] = d(add(13, 'S', 990, 50));
        const auto bid = d.m.state(0, side::buy), ask = d.m.state(0, side::sell);
        check(k == step::execute_apply && d.p.counts().by_cause[static_cast<int>(miss::not_trading)] == 1 && k2 == step::rest && r2.empty() &&
                  d.p.counts().adds_rested[static_cast<int>(miss::not_trading)] == 1 && !ask.empty() && ask[0].price == 990 && bid[0].price == 1000,
              "halted: an execution not first in line counted as not trading; an add that crosses rests without matching (the book crossed, as Nasdaq's was)");
        d(state('T'));
        const auto [k3, r3] = d(add(14, 'S', 995, 50));
        check(k3 == step::rest && d.p.counts().adds_rested[static_cast<int>(miss::unexplained)] == 1,
              "trading again: an add that would cross rests, counted unexplained");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100)), d(add(21, 'S', 1020, 100));
        const auto [k, r] = d(replace(11, 15, 1010, 80));
        const auto [k2, r2] = d(exec(15, 80));
        check(k == step::replace && r == reports{C(1, 100, cause::replaced), A(3), R(3, 1010, 80)} && k2 == step::execute &&
                  r2 == reports{A(4), T(1, 4, 3, 1010, 80, 0)} && d.m.state(0, side::buy).empty(),
              "a replace: the new reference mapped to the new ID, its execution reproduced");
        const auto [k3, r3] = d(add(12, 'B', 1000, 10));
        const auto [k4, r4] = d(replace(12, 16, 1025, 10));
        check(k4 == step::replace_rest && r4.empty() && d.p.counts().replaces_rested[static_cast<int>(miss::unexplained)] == 1 &&
                  d.m.state(0, side::buy)[0].price == 1025,
              "a replace that would cross rests without matching, counted");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100)), d(add(12, 'B', 1000, 100));
        const auto [k, r] = d(exec(12, 20, 'C', 'Y', 1002));
        const auto [k2, r2] = d(exec(12, 20));
        const auto& c = d.p.counts();
        check(k == step::execute_apply && c.by_cause[static_cast<int>(miss::other_price)] == 1 && k2 == step::execute_apply &&
                  c.by_cause[static_cast<int>(miss::earlier_orders)] == 1 && c.c_other_price_by_cause[static_cast<int>(miss::other_price)] == 1,
              "not first in line: a printable C at another price counted as executed at another price, an E as earlier orders ahead");
        agree = agree && d.agree;
    }
    {
        day d;
        d(add(11, 'B', 1000, 100));
        d(exec(11, 10, 'C', 'Y', 1003));
        const auto [k, r] = d(cancel(11, 90));
        const auto [k2, r2] = d(remove(11));
        const auto [k3, r3] = d(exec(99, 10));
        check(d.p.counts().c_other_price == 1 && d.p.counts().c_other_price_reproduced == 1 && k == step::cancel_part &&
                  r == reports{C(1, 90, cause::requested)} && k2 == step::none && k3 == step::none && d.p.counts().unknown == 2 &&
                  d.p.id_of(11) == 0,
              "a printable C at another price counted apart; a cancel of all it has finishes the order; events for a reference gone or never added counted, nothing done");
        agree = agree && d.agree;
    }
    check(agree, "the reference engine, deciding on its own book, took the same steps and gave the same reports and books in every case");
    return me::test::result();
}
